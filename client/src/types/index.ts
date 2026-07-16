// ============================================
// NetChat TypeScript 类型定义
// ============================================

// ---- 用户 ----
export interface User {
  userId: number;
  username: string;
  online?: boolean;
}

// ---- API 响应 ----
export interface AuthResponse {
  userId?: number;
  token?: string;
  error?: string;
}

export interface ApiError {
  error: string;
}

// ---- WebSocket 消息 ----
// 客户端 → 服务端
export interface ChatMessage {
  type: 'chat';
  to: number;
  msg: string;
  id?: string;       // 客户端消息 ID（用于回执跟踪）
}

export interface PingMessage {
  type: 'ping';
}

export interface ReadReceipt {
  type: 'read';
  from: number;
}

// 服务端 → 客户端
export interface ChatReceived {
  type: 'chat';
  from: number;
  username: string;
  msg: string;
  time: number;
  id?: string;        // 消息 ID（用于回执匹配）
}

export interface DeliveredReceipt {
  type: 'delivered';
  id: string;         // 被送达消息的 ID
  time: number;
}

export interface ReadReceiptReceived {
  type: 'read';
  from: number;
  username: string;
}

export interface OnlineListMessage {
  type: 'online_list';
  users: User[];
}

export interface UserOnlineMessage {
  type: 'user_online';
  userId: number;
  username: string;
}

export interface UserOfflineMessage {
  type: 'user_offline';
  userId: number;
}

export interface ErrorMessage {
  type: 'error';
  msg: string;
}

export type OutgoingMessage = ChatMessage | PingMessage | ReadReceipt;
export type IncomingMessage = ChatReceived | DeliveredReceipt | ReadReceiptReceived | OnlineListMessage | UserOnlineMessage | UserOfflineMessage | ErrorMessage | FriendRequestReceived | FriendAcceptedReceived;

// ---- 好友系统 ----
export interface FriendRequestReceived {
  type: 'friend_request';
  from: number;
  username: string;
}

export interface FriendAcceptedReceived {
  type: 'friend_accepted';
  by: number;
  username: string;
}

// ---- 会话存储 ----
export interface Session {
  userId: number;
  username: string;
  token: string;
}

// ---- 本地消息记录 ----
export type MessageStatus = 'sending' | 'sent' | 'delivered' | 'read';

export interface FileInfo {
  id: string;
  name: string;
  size: number;
  url: string;
  mime?: string;
}

export interface MessageRecord {
  id: string;
  fromUserId: number;
  username: string;
  content: string;
  time: number;
  isSelf: boolean;
  status?: MessageStatus;
  msgId?: string;  // 用于回执跟踪
  file?: FileInfo;  // 文件附件
}

// ---- 聊天会话 ----
export interface Conversation {
  userId: number;
  username: string;
  messages: MessageRecord[];
  unread: number;
}
