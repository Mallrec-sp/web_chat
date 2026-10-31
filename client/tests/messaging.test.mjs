import assert from 'node:assert/strict';
import { beforeEach, afterEach, test } from 'node:test';
import { build } from 'esbuild';
import { fileURLToPath } from 'node:url';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

// 直接打包实际 store，避免为测试另外实现一套消息逻辑。
const bundle = await build({
  stdin: {
    contents: `export { useChatStore } from './src/store/chatStore';
      export { wsClient } from './src/api/ws';
      export { parseSavedSession } from './src/api/session';
      export { default as http } from './src/api/http';
      export { default as UserList } from './src/components/UserList';`,
    resolveDir: fileURLToPath(new URL('..', import.meta.url)),
    loader: 'ts',
  },
  bundle: true,
  platform: 'node',
  format: 'esm',
  packages: 'external',
  write: false,
});
// 使用模块所在目录解析外部依赖；仅将 React/Zustand/axios 依赖保留为外部模块。
const { mkdtemp, writeFile, rm } = await import('node:fs/promises');
const directory = await mkdtemp(fileURLToPath(new URL('../.test-', import.meta.url)));
let useChatStore, wsClient, parseSavedSession, http, UserList;
try {
  await writeFile(`${directory}/store.mjs`, bundle.outputFiles[0].text);
  ({ useChatStore, wsClient, parseSavedSession, http, UserList } = await import(`${directory}/store.mjs`));
} finally {
  await rm(directory, { recursive: true, force: true });
}

class FakeWebSocket {
  static OPEN = 1;
  static instances = [];
  readyState = FakeWebSocket.OPEN;
  sent = [];
  constructor() { FakeWebSocket.instances.push(this); }
  send(value) { this.sent.push(JSON.parse(value)); }
  close() { this.readyState = 3; }
  receive(value) { this.onmessage?.({ data: JSON.stringify(value) }); }
}

globalThis.WebSocket = FakeWebSocket;
globalThis.window = { location: { protocol: 'http:', host: 'localhost:8081' } };
globalThis.localStorage = { removeItem() {} };
const A = '117532576972800001';
const B = '117532576972800002';
const C = '117532576972800003';
const session = { userId: A, username: 'alice', token: 'test-token' };

beforeEach(() => {
  useChatStore.getState().logout();
  FakeWebSocket.instances = [];
  useChatStore.setState({ session, token: session.token, activeChat: B });
  useChatStore.getState()._initWs();
});
afterEach(() => {
  useChatStore.setState({ token: null });
  useChatStore.getState().logout();
});

function send() {
  useChatStore.getState().sendMessage(B, '你好');
  const socket = FakeWebSocket.instances.at(-1);
  return { socket, message: socket.sent.at(-1) };
}
function echo(message) {
  return { ...message, from: A, username: 'alice', time: Date.now() };
}

test('乐观消息和回显合并，已送达状态不被回显降级', () => {
  const { socket, message } = send();
  assert.equal(message.to, B);
  socket.receive({ type: 'delivered', id: message.id, time: Date.now() });
  socket.receive(echo(message));
  socket.receive(echo(message));
  const messages = useChatStore.getState().conversations.get(B).messages;
  assert.equal(messages.length, 1);
  assert.equal(messages[0].status, 'delivered');
});

test('切换会话后回显仍归属于原接收者', () => {
  const { socket, message } = send();
  useChatStore.setState({ activeChat: C });
  socket.receive(echo(message));
  const conversations = useChatStore.getState().conversations;
  assert.equal(conversations.get(B).messages.length, 1);
  assert.equal(conversations.get(B).unread, 0);
  assert.equal(conversations.has(C), false);
});

test('重复收到对端消息只插入一次且未读只加一', () => {
  useChatStore.setState({ activeChat: C });
  const socket = FakeWebSocket.instances.at(-1);
  const message = { type: 'chat', id: 'peer-1', from: B, to: A, username: 'bob', msg: '你好', time: Date.now() };
  socket.receive(message);
  socket.receive(message);
  const conversation = useChatStore.getState().conversations.get(B);
  assert.equal(conversation.messages.length, 1);
  assert.equal(conversation.unread, 1);
});

test('重复初始化只有一个消息监听器', () => {
  useChatStore.getState()._initWs();
  useChatStore.getState()._initWs();
  let events = 0;
  const original = useChatStore.getState()._handleWsMessage;
  useChatStore.setState({ _handleWsMessage: () => { events++; } });
  try {
    FakeWebSocket.instances.at(-1).receive({ type: 'online_list', users: [] });
    assert.equal(events, 1);
  } finally {
    useChatStore.setState({ _handleWsMessage: original });
  }
});

test('已读消息不被迟到的回显或送达回执降级', () => {
  const { socket, message } = send();
  socket.receive(echo(message));
  socket.receive({ type: 'read', from: B, username: 'bob' });
  socket.receive({ type: 'delivered', id: message.id, time: Date.now() });
  socket.receive(echo(message));
  const messages = useChatStore.getState().conversations.get(B).messages;
  assert.equal(messages.length, 1);
  assert.equal(messages[0].status, 'read');
});

test('旧数字 ID 登录缓存必须失效，字符串大整数保持原样', () => {
  assert.equal(parseSavedSession(JSON.stringify({ ...session, userId: Number(A) })), null);
  assert.deepEqual(parseSavedSession(JSON.stringify(session)), session);
  for (const userId of ['0', '-1', '01', '1x', '9223372036854775808']) {
    assert.equal(parseSavedSession(JSON.stringify({ ...session, userId })), null);
  }
  assert.equal(parseSavedSession('null'), null);
  assert.equal(parseSavedSession('{'), null);
});

test('切换账号后旧连接消息不会进入新账号', () => {
  const old = FakeWebSocket.instances.at(-1);
  useChatStore.setState({ session: { ...session, userId: C, token: 'new-token' }, token: 'new-token' });
  useChatStore.getState()._initWs();
  old.receive({ type: 'chat', from: B, to: A, id: 'stale', username: 'bob', msg: '旧账号消息', time: Date.now() });
  assert.equal(useChatStore.getState().conversations.size, 0);
});

function renderUserList() {
  const initialState = useChatStore.getInitialState();
  const saved = { ...initialState };
  // SSR 读取创建时的快照，临时填入事件处理后的状态来验证真实组件输出。
  Object.assign(initialState, useChatStore.getState());
  try {
    return renderToStaticMarkup(createElement(UserList));
  } finally {
    Object.assign(initialState, saved);
  }
}

test('后注册的用户上线时立即加入列表，重复上线不重复添加', () => {
  const socket = FakeWebSocket.instances.at(-1);
  useChatStore.setState({ users: [{ userId: C, username: 'carol', online: false }] });
  socket.receive({ type: 'user_online', userId: B, username: 'bob' });
  socket.receive({ type: 'user_online', userId: B, username: 'bob' });
  assert.deepEqual(useChatStore.getState().users, [
    { userId: C, username: 'carol', online: false },
    { userId: B, username: 'bob', online: true },
  ]);
  assert.match(renderUserList(), />bob<\/div>/);
});

test('在线快照补充未知用户，保留离线用户并显示真实状态', () => {
  useChatStore.setState({ users: [{ userId: C, username: 'carol', online: true }] });
  const socket = FakeWebSocket.instances.at(-1);
  socket.receive({ type: 'online_list', users: [{ userId: B, username: 'bob' }] });
  socket.receive({ type: 'online_list', users: [{ userId: B, username: 'bob' }] });
  assert.deepEqual(useChatStore.getState().users, [
    { userId: C, username: 'carol', online: false },
    { userId: B, username: 'bob', online: true },
  ]);
  socket.receive({ type: 'user_offline', userId: B });
  assert.equal(useChatStore.getState().users.find(u => u.userId === B).online, false);
  const html = renderUserList();
  assert.equal((html.match(/class="status">离线/g) || []).length, 2);
});

test('未知用户来信立即显示列表入口和未读数，不切换当前会话', () => {
  useChatStore.setState({ activeChat: null });
  const socket = FakeWebSocket.instances.at(-1);
  const message = { type: 'chat', id: 'unknown-peer', from: B, to: A, username: 'bob', msg: '未刷新也能收到', time: Date.now() };
  socket.receive(message);
  socket.receive(message);
  const state = useChatStore.getState();
  assert.deepEqual(state.users, [{ userId: B, username: 'bob', online: true }]);
  assert.equal(state.activeChat, null);
  assert.equal(state.conversations.get(B).messages.length, 1);
  assert.equal(state.conversations.get(B).unread, 1);
  const html = renderUserList();
  assert.match(html, />bob<\/div>/);
  assert.match(html, /class="unread-badge">1<\/div>/);
});

test('迟到的 HTTP 用户列表不会覆盖加载期间的上线或离线事件', async () => {
  useChatStore.setState({ users: [{ userId: C, username: 'carol', online: true }] });
  const adapter = http.defaults.adapter;
  let finish;
  http.defaults.adapter = config => new Promise(resolve => {
    finish = data => resolve({ data, status: 200, statusText: 'OK', headers: {}, config });
  });
  try {
    const loading = useChatStore.getState().loadUsers();
    const socket = FakeWebSocket.instances.at(-1);
    socket.receive({ type: 'user_online', userId: B, username: 'bob' });
    socket.receive({ type: 'user_offline', userId: C });
    finish([{ userId: C, username: 'carol', online: true }]);
    await loading;
    assert.equal(useChatStore.getState().users.find(u => u.userId === B)?.online, true);
    assert.equal(useChatStore.getState().users.find(u => u.userId === C)?.online, false);
  } finally {
    http.defaults.adapter = adapter;
  }
});

test('在线快照也适用于随后 HTTP 才加载出来的用户', async () => {
  const adapter = http.defaults.adapter;
  let finish;
  http.defaults.adapter = config => new Promise(resolve => {
    finish = data => resolve({ data, status: 200, statusText: 'OK', headers: {}, config });
  });
  try {
    const loading = useChatStore.getState().loadUsers();
    FakeWebSocket.instances.at(-1).receive({ type: 'online_list', users: [{ userId: B, username: 'bob' }] });
    finish([{ userId: C, username: 'carol', online: true }]);
    await loading;
    assert.equal(useChatStore.getState().users.find(u => u.userId === B)?.online, true);
    assert.equal(useChatStore.getState().users.find(u => u.userId === C)?.online, false);
  } finally {
    http.defaults.adapter = adapter;
  }
});

test('退出登录后，旧 HTTP 响应不能重新填充用户列表', async () => {
  const adapter = http.defaults.adapter;
  let finish;
  http.defaults.adapter = config => new Promise(resolve => {
    finish = data => resolve({ data, status: 200, statusText: 'OK', headers: {}, config });
  });
  try {
    const loading = useChatStore.getState().loadUsers();
    useChatStore.setState({ token: null });
    useChatStore.getState().logout();
    finish([{ userId: B, username: 'bob', online: true }]);
    await loading;
    assert.deepEqual(useChatStore.getState().users, []);
    assert.equal(useChatStore.getState().loadingUsers, false);
  } finally {
    http.defaults.adapter = adapter;
  }
});
