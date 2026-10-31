import { create } from 'zustand';
import type { UserId, User, Session, MessageRecord, Conversation, IncomingMessage, MessageStatus } from '../types';
import { login as apiLogin, register as apiRegister, logout as apiLogout, fetchUsers as apiFetchUsers, fetchHistory as apiFetchHistory } from '../api/http';
import { wsClient } from '../api/ws';

let unsubscribeWs: (() => void) | undefined;
let connectedToken: string | null = null;
let onlineUserIds: Set<UserId> | null = null;
let usersRequestId = 0;

// 保留原有顺序和离线用户，按 ID 更新或新增，不产生重复条目。
function mergeUsers(current: User[], incoming: User[]): User[] {
  const users = new Map(current.map(user => [user.userId, user]));
  for (const user of incoming) {
    users.set(user.userId, { ...users.get(user.userId), ...user });
  }
  return [...users.values()];
}

interface ChatState {
  session: Session | null;
  token: string | null;
  users: User[];
  conversations: Map<UserId, Conversation>;
  activeChat: UserId | null;
  loadingUsers: boolean;
  loadingHistory: boolean;
  friendRequests: { userId: UserId; username: string }[];
  friendRequestsCount: number;

  login: (username: string, password: string) => Promise<string | null>;
  register: (username: string, password: string) => Promise<string | null>;
  logout: () => void;
  selectUser: (userId: UserId) => void;
  sendMessage: (to: UserId, msg: string) => void;
  loadUsers: () => Promise<void>;
  loadHistory: (userId: UserId) => Promise<void>;

  _initWs: () => void;
  _handleWsMessage: (msg: IncomingMessage) => void;
}

export const useChatStore = create<ChatState>((set, get) => ({
  session: null,
  token: null,
  users: [],
  conversations: new Map(),
  activeChat: null,
  loadingUsers: false,
  loadingHistory: false,
  friendRequests: [],
  friendRequestsCount: 0,

  login: async (username, password) => {
    const res = await apiLogin(username, password);
    if (res.error) return res.error;
    if (!res.token || !res.userId) return '登录失败';

    const session: Session = { userId: res.userId, username, token: res.token };
    localStorage.setItem('netchat_session', JSON.stringify(session));
    set({ session, token: res.token });
    get()._initWs();
    get().loadUsers();  // 加载用户列表
    return null;
  },

  register: async (username, password) => {
    const res = await apiRegister(username, password);
    if (res.error) return res.error;
    if (!res.token || !res.userId) return '注册失败';

    const session: Session = { userId: res.userId, username, token: res.token };
    localStorage.setItem('netchat_session', JSON.stringify(session));
    set({ session, token: res.token });
    get()._initWs();
    get().loadUsers();  // 加载用户列表
    return null;
  },

  logout: () => {
    const token = get().token;
    if (token) apiLogout(token).catch(() => {});
    unsubscribeWs?.();
    unsubscribeWs = undefined;
    connectedToken = null;
    onlineUserIds = null;
    usersRequestId++;
    wsClient.disconnect();
    localStorage.removeItem('netchat_session');
    set({ session: null, token: null, users: [], conversations: new Map(), activeChat: null, loadingUsers: false });
  },

  selectUser: async (userId) => {
    set({ activeChat: userId });
    const conv = get().conversations.get(userId);
    if (conv) {
      conv.unread = 0;
      const newConvs = new Map(get().conversations);
      newConvs.set(userId, { ...conv });
      set({ conversations: newConvs });
    }
    // 发送已读回执
    wsClient.send({ type: 'read', from: userId });
    // 加载消息历史
    await get().loadHistory(userId);
  },

  sendMessage: (to, msg) => {
    const session = get().session;
    if (!session) return;

    // 生成本地消息 ID
    const msgId = `msg_${Date.now()}_${Math.random().toString(36).substr(2, 6)}`;

    // 乐观添加到本地
    const record: MessageRecord = {
      id: msgId,
      msgId,
      fromUserId: session.userId,
      username: session.username,
      content: msg,
      time: Date.now(),
      isSelf: true,
      status: 'sending',
    };

    const convs = new Map(get().conversations);
    const existing = convs.get(to);
    if (existing) {
      convs.set(to, { ...existing, messages: [...existing.messages, record] });
    } else {
      convs.set(to, {
        userId: to,
        username: '',
        messages: [record],
        unread: 0,
      });
    }
    set({ conversations: convs });

    // 发送到服务端
    wsClient.send({ type: 'chat', to, msg, id: msgId });
  },

  loadUsers: async () => {
    const token = get().token;
    if (!token) return;
    const requestId = ++usersRequestId;
    const before = new Map(get().users.map(user => [user.userId, user]));
    try {
      set({ loadingUsers: true });
      const users = await apiFetchUsers(token);
      if (get().token !== token || requestId !== usersRequestId) return;
      const current = new Map(get().users.map(user => [user.userId, user]));
      const merged = mergeUsers(get().users, users).map(user => {
        // HTTP 返回期间可能已收到新事件，不能用旧快照覆盖它们。
        const updated = current.get(user.userId);
        const latest = updated && updated !== before.get(user.userId) ? updated : user;
        return onlineUserIds
          ? { ...latest, online: onlineUserIds.has(user.userId) }
          : latest;
      });
      set({ users: merged });
    } catch (e) {
      console.error('Failed to load users:', e);
    } finally {
      if (get().token === token && requestId === usersRequestId) {
        set({ loadingUsers: false });
      }
    }
  },

  loadHistory: async (userId) => {
    const token = get().token;
    const session = get().session;
    if (!token || !session) return;
    try {
      set({ loadingHistory: true });
      const msgs = await apiFetchHistory(token, userId);
      // 修正 isSelf 标志
      const records: MessageRecord[] = msgs.map(m => ({
        ...m,
        isSelf: m.fromUserId === session.userId,
        username: m.fromUserId === session.userId ? session.username : (get().users.find(u => u.userId === m.fromUserId)?.username || ''),
      }));
      const convs = new Map(get().conversations);
      convs.set(userId, {
        userId,
        username: get().users.find(u => u.userId === userId)?.username || `#${userId}`,
        messages: records,
        unread: 0,
      });
      set({ conversations: convs });
    } catch (e) {
      console.error('Failed to load history:', e);
    } finally {
      set({ loadingHistory: false });
    }
  },

  _initWs: () => {
    const { token } = get();
    if (!token || (connectedToken === token && unsubscribeWs)) return;
    unsubscribeWs?.();
    wsClient.disconnect();
    unsubscribeWs = wsClient.onMessage((msg) => {
      get()._handleWsMessage(msg);
    });
    connectedToken = token;
    onlineUserIds = null;
    wsClient.connect(token);
  },

  _handleWsMessage: (msg) => {
    const state = get();
    const convs = new Map(state.conversations);

    switch (msg.type) {
      // online_list 来自 WebSocket (已经广播), users 来自 HTTP API
      case 'online_list': {
        const onlineIds = new Set(msg.users.map(user => user.userId));
        onlineUserIds = onlineIds;
        const merged = mergeUsers(state.users, msg.users).map(user => ({
          ...user,
          online: onlineIds.has(user.userId),
        }));
        set({ users: merged });
        break;
      }

      case 'user_online': {
        onlineUserIds?.add(msg.userId);
        set({
          users: mergeUsers(state.users, [{ userId: msg.userId, username: msg.username, online: true }]),
        });
        break;
      }

      case 'user_offline': {
        onlineUserIds?.delete(msg.userId);
        set({
          users: state.users.map(u =>
            u.userId === msg.userId ? { ...u, online: false } : u
          ),
        });
        break;
      }

      case 'chat': {
        const session = state.session;
        if (!session) break;

        // 自身回显必须按收件人路由，不能依赖此刻打开的会话。
        const isSelf = msg.from === session.userId;
        const convId = isSelf ? msg.to : msg.from;
        const existing = convs.get(convId);
        const index = msg.id ? (existing?.messages.findIndex(m =>
          m.msgId === msg.id && m.fromUserId === msg.from
        ) ?? -1) : -1;
        const previous = index >= 0 ? existing!.messages[index] : undefined;
        const status = isSelf
          ? (previous?.status === 'delivered' || previous?.status === 'read' ? previous.status : 'sent')
          : undefined;

        const record: MessageRecord = {
          id: previous?.id || msg.id || `${msg.time}-${msg.from}-${Math.random()}`,
          msgId: msg.id,
          fromUserId: msg.from,
          username: msg.username,
          content: msg.msg,
          time: msg.time,
          isSelf,
          status,
          file: msg.file,
        };

        const messages = existing ? [...existing.messages] : [];
        if (index >= 0) {
          messages[index] = record;
        } else {
          messages.push(record);
        }
        const unread = !isSelf && index < 0 && convId !== state.activeChat ? 1 : 0;
        convs.set(convId, {
          userId: convId,
          username: existing?.username || (isSelf
            ? state.users.find(u => u.userId === convId)?.username || ''
            : msg.username),
          messages,
          unread: (existing?.unread || 0) + unread,
        });
        // 即使错过上线通知，来信也必须有可点击的用户入口。
        let users = state.users;
        if (!isSelf && !users.some(user => user.userId === msg.from)) {
          onlineUserIds?.add(msg.from);
          users = mergeUsers(users, [{ userId: msg.from, username: msg.username, online: true }]);
        }
        set({ conversations: convs, users });
        break;
      }

      // 已送达回执
      case 'delivered': {
        const convs2 = new Map(state.conversations);
        for (const [convId, conv] of convs2) {
          const updated = conv.messages.map(m =>
            m.isSelf && m.msgId === msg.id && m.status !== 'read'
              ? { ...m, status: 'delivered' as MessageStatus } : m
          );
          if (updated !== conv.messages) {
            convs2.set(convId, { ...conv, messages: updated });
          }
        }
        set({ conversations: convs2 });
        break;
      }

      // 已读回执
      case 'read': {
        const convs3 = new Map(state.conversations);
        const conv = convs3.get(msg.from);
        if (conv) {
          const updated = conv.messages.map(m =>
            m.status === 'delivered' || m.status === 'sent'
              ? { ...m, status: 'read' as MessageStatus }
              : m
          );
          convs3.set(msg.from, { ...conv, messages: updated });
        }
        set({ conversations: convs3 });
        break;
      }

      // 好友请求
      case 'friend_request': {
        const exists = state.friendRequests.find(r => r.userId === msg.from);
        if (!exists) {
          const newReqs = [...state.friendRequests, { userId: msg.from, username: msg.username }];
          set({ friendRequests: newReqs, friendRequestsCount: newReqs.length });
        }
        break;
      }

      // 好友请求已接受
      case 'friend_accepted': {
        set({ friendRequests: state.friendRequests.filter(r => r.userId !== msg.by) });
        break;
      }

      case 'error': {
        console.error('Server error:', msg.msg);
        break;
      }
    }
  },
}));
