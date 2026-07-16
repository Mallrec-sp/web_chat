import { create } from 'zustand';
import type { User, Session, MessageRecord, Conversation, IncomingMessage, MessageStatus } from '../types';
import { login as apiLogin, register as apiRegister, logout as apiLogout, fetchUsers as apiFetchUsers, fetchHistory as apiFetchHistory } from '../api/http';
import { wsClient } from '../api/ws';

interface ChatState {
  session: Session | null;
  token: string | null;
  users: User[];
  conversations: Map<number, Conversation>;
  activeChat: number | null;
  loadingUsers: boolean;
  loadingHistory: boolean;
  friendRequests: { userId: number; username: string }[];
  friendRequestsCount: number;

  login: (username: string, password: string) => Promise<string | null>;
  register: (username: string, password: string) => Promise<string | null>;
  logout: () => void;
  selectUser: (userId: number) => void;
  sendMessage: (to: number, msg: string) => void;
  loadUsers: () => Promise<void>;
  loadHistory: (userId: number) => Promise<void>;

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
    wsClient.disconnect();
    localStorage.removeItem('netchat_session');
    set({ session: null, token: null, users: [], conversations: new Map(), activeChat: null });
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
    try {
      set({ loadingUsers: true });
      const users = await apiFetchUsers(token);
      set({ users });
    } catch (e) {
      console.error('Failed to load users:', e);
    } finally {
      set({ loadingUsers: false });
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
    if (!token) return;
    wsClient.disconnect();
    wsClient.connect(token);
    wsClient.onMessage((msg) => {
      get()._handleWsMessage(msg);
    });
  },

  _handleWsMessage: (msg) => {
    const state = get();
    const convs = new Map(state.conversations);

    switch (msg.type) {
      // online_list 来自 WebSocket (已经广播), users 来自 HTTP API
      case 'online_list': {
        // 把在线状态合并到已有 users 中
        const onlineIds = new Set(msg.users.map((u: any) => u.userId));
        const merged = state.users.map(u => ({
          ...u,
          online: onlineIds.has(u.userId),
        }));
        set({ users: merged });
        break;
      }

      case 'user_online': {
        set({
          users: state.users.map(u =>
            u.userId === msg.userId ? { ...u, online: true } : u
          ),
        });
        break;
      }

      case 'user_offline': {
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

        // 检查是否是我们自己发出的消息（回声确认）
        const isSelf = msg.from === session.userId;
        let status: MessageStatus | undefined = isSelf ? 'sent' : undefined;

        const record: MessageRecord = {
          id: msg.id || `${msg.time}-${msg.from}-${Math.random()}`,
          msgId: msg.id,
          fromUserId: msg.from,
          username: msg.username,
          content: msg.msg,
          time: msg.time,
          isSelf,
          status,
        };

        const convId = msg.from === session.userId ? (state.activeChat || msg.from) : msg.from;

        const existing = convs.get(convId);
        if (existing) {
          convs.set(convId, {
            ...existing,
            messages: [...existing.messages, record],
            unread: convId !== state.activeChat ? existing.unread + 1 : existing.unread,
          });
        } else {
          convs.set(convId, {
            userId: convId,
            username: msg.username,
            messages: [record],
            unread: convId !== state.activeChat ? 1 : 0,
          });
        }
        set({ conversations: convs });
        break;
      }

      // 已送达回执
      case 'delivered': {
        const convs2 = new Map(state.conversations);
        for (const [convId, conv] of convs2) {
          const updated = conv.messages.map(m =>
            m.msgId === msg.id ? { ...m, status: 'delivered' as MessageStatus } : m
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
