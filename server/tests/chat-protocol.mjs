import assert from 'node:assert/strict';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createServer } from 'node:net';
import { setTimeout as delay } from 'node:timers/promises';

// 必须显式指定一次性 MySQL 测试库；绝不回退到开发配置或 SQLite。
assert(Number(process.versions.node.split('.')[0]) >= 22, '需要 Node.js 22+');
assert(/^netchat_test_[a-z0-9_]+$/.test(process.env.NETCHAT_DB_NAME ?? ''),
  '必须显式设置 NETCHAT_DB_NAME=netchat_test_*');
for (const name of ['NETCHAT_DB_HOST', 'NETCHAT_DB_USER', 'NETCHAT_DB_PASSWORD',
  'NETCHAT_TEST_READONLY_USER', 'NETCHAT_TEST_READONLY_PASSWORD']) {
  assert(process.env[name], `缺少测试环境变量 ${name}`);
}
assert.equal(process.argv.length, 2, '不支持参数或旧 --reproduce 模式');

const root = fileURLToPath(new URL('../..', import.meta.url));
const temp = await mkdtemp(join(tmpdir(), 'netchat-protocol-'));
const sockets = [];
const processes = [];
const password = 'Test-password-123';
const suiteAbort = new AbortController();
const onInt = () => suiteAbort.abort(new Error('协议测试被 SIGINT 中断'));
const onTerm = () => suiteAbort.abort(new Error('协议测试被 SIGTERM 中断'));
process.on('SIGINT', onInt);
process.on('SIGTERM', onTerm);
const suiteDeadline = setTimeout(() => suiteAbort.abort(new Error('协议测试整体超时')), 180000);
const interrupted = new Promise((_, reject) => {
  suiteAbort.signal.addEventListener('abort', () => reject(suiteAbort.signal.reason), { once: true });
});
let server;

async function bounded(promise, milliseconds, description) {
  let timer;
  try {
    return await Promise.race([promise, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error(`${description}超时`)), milliseconds);
    })]);
  } finally { clearTimeout(timer); }
}

async function freePort() {
  const listener = createServer();
  listener.listen(0, '127.0.0.1');
  await once(listener, 'listening');
  const port = listener.address().port;
  await new Promise((resolve, reject) => listener.close(error => error ? reject(error) : resolve()));
  return port;
}

async function startServer(overrides = {}) {
  suiteAbort.signal.throwIfAborted();
  const port = await freePort();
  const config = join(temp, `config-${processes.length}.json`);
  await writeFile(config, JSON.stringify({ server: { host: '127.0.0.1', port }, workers: 2 }));
  suiteAbort.signal.throwIfAborted();
  const child = spawn(process.env.NETCHAT_TEST_BINARY || resolve(root, 'server/build/server'), [], {
    cwd: temp,
    env: {
      ...process.env, ...overrides,
      NETCHAT_CONFIG_PATH: config,
      NETCHAT_STATIC_DIR: process.env.NETCHAT_STATIC_DIR || resolve(root, 'client/dist'),
    },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  const instance = { child, port, base: `http://127.0.0.1:${port}`, logs: '', done: false };
  instance.exited = new Promise(resolve => {
    child.once('error', error => { instance.done = true; resolve({ error }); });
    child.once('exit', (code, signal) => { instance.done = true; resolve({ code, signal }); });
  });
  const collect = data => { instance.logs = (instance.logs + data).slice(-16000); };
  child.stdout.on('data', collect);
  child.stderr.on('data', collect);
  processes.push(instance);
  const deadline = Date.now() + 15000;
  while (true) {
    suiteAbort.signal.throwIfAborted();
    assert(!instance.done, '服务在就绪前退出');
    assert(Date.now() < deadline, '服务启动超时');
    try {
      const health = await fetch(`${instance.base}/api/health`, { signal: AbortSignal.timeout(1000) });
      if (health.ok) return instance;
    } catch {}
    await delay(50);
  }
}

async function stopServer(instance, checkExit = true) {
  if (!instance.done) instance.child.kill('SIGTERM');
  let outcome;
  try { outcome = await bounded(instance.exited, 10000, '服务正常退出'); }
  catch (error) {
    instance.child.kill('SIGKILL');
    await bounded(instance.exited, 3000, '服务强制退出');
    throw error;
  }
  if (checkExit) {
    assert.ifError(outcome.error);
    assert.equal(outcome.signal, null, '服务不应因信号被强制终止');
    assert.equal(outcome.code, 0, '服务正常停止应返回 0');
  }
}

async function api(path, { token, body, status = 200, instance = server } = {}) {
  suiteAbort.signal.throwIfAborted();
  const url = new URL(`/api/${path}`, instance.base);
  if (token) url.searchParams.set('token', token);
  const response = await fetch(url, {
    ...(body === undefined ? {} : {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
    }),
    signal: AbortSignal.any([suiteAbort.signal, AbortSignal.timeout(5000)]),
  });
  assert.equal(response.status, status, `${path} HTTP 状态错误`);
  return response.json();
}

async function connect(account, instance = server) {
  suiteAbort.signal.throwIfAborted();
  const ws = new WebSocket(`ws://127.0.0.1:${instance.port}/ws?token=${account.token}`);
  const messages = [];
  const waiters = new Set();
  let parseError;
  ws.addEventListener('message', event => {
    try { messages.push(JSON.parse(event.data)); }
    catch (error) { parseError = error; }
    for (const notify of waiters) notify();
  });
  const client = {
    ws, messages, instance,
    send: message => ws.send(JSON.stringify(message)),
    wait(predicate) {
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
          waiters.delete(check);
          reject(new Error(`等待消息超时；最近消息类型：${messages.slice(-10).map(m => m.type).join(',')}`));
        }, 5000);
        function check() {
          const message = messages.find(predicate);
          if (!message && !parseError) return;
          clearTimeout(timer);
          waiters.delete(check);
          if (parseError) reject(parseError); else resolve(message);
        }
        waiters.add(check);
        check();
      });
    },
    async close() {
      if (ws.readyState === WebSocket.CLOSED) return;
      const closed = once(ws, 'close');
      ws.close();
      await bounded(closed, 3000, 'WebSocket 关闭');
    },
  };
  sockets.push(client);
  await bounded(once(ws, 'open'), 5000, 'WebSocket 连接');
  await client.wait(m => m.type === 'online_list');
  return client;
}

async function closeClients(instance) {
  await Promise.all(sockets.filter(socket => socket.instance === instance).map(socket => socket.close()));
}

async function chat(sender, receiver, from, to, id, text = `消息-${id}`) {
  suiteAbort.signal.throwIfAborted();
  sender.send({ type: 'chat', to, msg: text, id });
  const [received, echo] = await Promise.all([
    receiver.wait(m => m.type === 'chat' && m.id === id),
    sender.wait(m => m.type === 'chat' && m.id === id),
    sender.wait(m => m.type === 'delivered' && m.id === id),
  ]);
  assert.deepEqual(received, echo);
  assert.equal(received.from, from);
  assert.equal(received.to, to);
  assert.equal(received.msg, text);
  assert.equal(receiver.messages.filter(m => m.type === 'chat' && m.id === id).length, 1);
  assert.equal(sender.messages.filter(m => m.type === 'chat' && m.id === id).length, 1);
}

async function runTests() {
  server = await startServer();
  const alice = await api('auth/register', { body: { username: 'test_alice', password } });
  const a = await connect(alice);
  assert.equal((await api('users', { token: alice.token })).length, 1, '协议测试要求空白业务库');
  // A 不重新请求列表也必须得知注册后才上线的 B。
  const bob = await api('auth/register', { body: { username: 'test_bob', password } });
  const b = await connect(bob);
  await a.wait(m => m.type === 'user_online' && m.userId === bob.userId);
  for (const account of [alice, bob]) {
    assert.equal(typeof account.userId, 'string');
    assert(BigInt(account.userId) > BigInt(Number.MAX_SAFE_INTEGER));
  }
  const login = await api('auth/login', { body: { username: 'test_alice', password } });
  assert.equal(login.userId, alice.userId);
  const users = await api('users', { token: alice.token });
  assert(users.some(u => u.userId === bob.userId));
  assert(users.every(u => typeof u.userId === 'string'));
  const online = await a.wait(m => m.type === 'online_list' && m.users?.some(u => u.userId === bob.userId));
  assert(online.users.every(u => typeof u.userId === 'string'));

  const registrations = await Promise.all(Array.from({ length: 8 }, () => fetch(`${server.base}/api/auth/register`, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username: 'same_name_race', password }),
    signal: AbortSignal.any([suiteAbort.signal, AbortSignal.timeout(10000)]),
  }).then(async response => ({ status: response.status, body: await response.json() }))));
  assert.equal(registrations.filter(r => r.status === 200).length, 1, '并发同名注册只能成功一次');
  assert.equal(registrations.filter(r => r.status === 400).length, 7, '其他同名注册必须返回 400');
  assert(registrations.filter(r => r.status === 400).every(r => r.body.error === 'Username already exists'));

  await chat(a, b, alice.userId, bob.userId, 'a-to-b');
  await chat(b, a, bob.userId, alice.userId, 'b-to-a');
  b.send({ type: 'read', from: alice.userId });
  await a.wait(m => m.type === 'read' && m.from === bob.userId);
  const history = await api(`messages/history?with=${bob.userId}`, { token: alice.token });
  assert.equal(history.length, 2);
  assert(history.every(m => typeof m.fromUserId === 'string' && typeof m.toUserId === 'string'));
  const longText = '中文🙂🚀长消息'.repeat(6000);
  assert(Buffer.byteLength(longText) > 64 * 1024);
  await chat(a, b, alice.userId, bob.userId, 'long-unicode', longText);

  await api('friends/add', { token: alice.token, body: { userId: bob.userId } });
  await b.wait(m => m.type === 'friend_request' && m.from === alice.userId);
  assert((await api('friends/requests', { token: bob.token })).some(u => u.userId === alice.userId));
  // 制造双向 pending，接受时反向 upsert 也必须升级为 accepted。
  await api('friends/add', { token: bob.token, body: { userId: alice.userId } });
  await api('friends/accept', { token: bob.token, body: { userId: alice.userId } });
  await a.wait(m => m.type === 'friend_accepted' && m.by === bob.userId);
  assert((await api('friends', { token: alice.token })).some(u => u.userId === bob.userId));
  assert((await api('friends', { token: bob.token })).some(u => u.userId === alice.userId));
  assert(!(await api('friends/requests', { token: alice.token })).some(u => u.userId === bob.userId));
  await api('friends/accept', { token: bob.token, body: { userId: alice.userId }, status: 400 });

  for (const [index, to] of [undefined, null, true, {}, [], '', '0', '-1', '01', '+1', ' 1', '1x',
    '9223372036854775808', Number(bob.userId), 1.5].entries()) {
    const id = `invalid-${index}`;
    a.send({ type: 'chat', to, msg: '非法目标', id });
    await a.wait(m => m.type === 'error' && m.id === id);
    for (const action of ['add', 'accept', 'reject', 'remove']) {
      await api(`friends/${action}`, { token: alice.token, body: { userId: to }, status: 400 });
    }
  }
  a.send({ type: 'chat', to: bob.userId, msg: {}, id: 'invalid-text' });
  await a.wait(m => m.type === 'error' && m.id === 'invalid-text');
  for (const [index, message] of [
    { type: 'read', from: {} }, { type: 'friend_add', userId: [] },
    { type: 'friend_accept', userId: null }, { type: {} },
  ].entries()) {
    const id = `invalid-fields-${index}`;
    a.send({ ...message, id });
    await a.wait(m => m.type === 'error' && m.id === id);
  }
  await api('messages/history?with=123abc', { token: alice.token, status: 400 });
  const b2 = await connect(bob);
  await b.close();
  await chat(a, b2, alice.userId, bob.userId, 'after-reconnect');
  assert((await api('users', { token: alice.token })).some(u => u.userId === bob.userId && u.online));

  await closeClients(server);
  await stopServer(server);
  server = await startServer();
  const persistedLogin = await api('auth/login', { body: { username: 'test_alice', password } });
  assert.equal(persistedLogin.userId, alice.userId);
  const persisted = await api(`messages/history?with=${bob.userId}`, { token: alice.token });
  assert.equal(persisted.length, 4, '重启后所有历史应保留，旧 token 仍有效');
  assert(persisted.some(m => m.content === longText), 'LONGTEXT 中文 emoji 正文应完整持久化');
  assert((await api('friends', { token: alice.token })).some(u => u.userId === bob.userId));

  // 使用只授予 SELECT 的测试账号连接同一测试库，绝不改应用账号权限或业务表。
  const readonly = await startServer({
    NETCHAT_DB_USER: process.env.NETCHAT_TEST_READONLY_USER,
    NETCHAT_DB_PASSWORD: process.env.NETCHAT_TEST_READONLY_PASSWORD,
  });
  await api('auth/login', { instance: readonly, body: { username: 'test_alice', password }, status: 503 });
  const readerA = await connect(alice, readonly);
  const readerB = await connect(bob, readonly);
  const failureId = 'readonly-write-failure';
  readerA.send({ type: 'chat', to: bob.userId, msg: '不得确认的写入', id: failureId });
  const failure = await readerA.wait(m => m.type === 'error' && m.id === failureId);
  assert.match(failure.msg, /Database unavailable/);
  await delay(300);
  for (const client of [readerA, readerB]) {
    assert(!client.messages.some(m => m.id === failureId && ['chat', 'delivered'].includes(m.type)),
      '写入失败不得发送 chat 回显/投递或 delivered');
  }
  assert.equal((await api(`messages/history?with=${bob.userId}`, { token: alice.token })).length, 4);
  await closeClients(readonly);
  await stopServer(readonly);
  await stopServer(server);
  console.log('通过：大ID、新用户上线、双向聊天、回显/回执、历史/好友、非法输入、重连、并发注册、长Unicode、重启持久化、只读写入失败');
}

try {
  await Promise.race([runTests(), interrupted]);
} catch (error) {
  console.error(error);
  for (const instance of processes) console.error(instance.logs.slice(-6000));
  process.exitCode = 1;
} finally {
  clearTimeout(suiteDeadline);
  suiteAbort.abort(new Error('协议测试已结束'));
  for (const socket of sockets) socket.ws.close();
  for (const instance of processes) {
    if (!instance.done) {
      try { await stopServer(instance, false); }
      catch (error) { console.error(error.message); process.exitCode = 1; }
    }
  }
  await rm(temp, { recursive: true, force: true });
  process.off('SIGINT', onInt);
  process.off('SIGTERM', onTerm);
}
