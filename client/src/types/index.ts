// ============================================
// NetChat TypeScript 类型定义
// ============================================

// ---- 用户 ----
// 用户 ID 超过 JavaScript 安全整数范围，网络与前端始终使用字符串。
export type UserId = string;

export interface User {
  userId: UserId;
  username: string;
  online?: boolean;
}

// ---- API 响应 ----
export interface AuthResponse {
  userId?: UserId;
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
  to: UserId;
  msg: string;
  id?: string;       // 客户端消息 ID（用于回执跟踪）
}

export interface PingMessage {
  type: 'ping';
}

export interface ReadReceipt {
  type: 'read';
  from: UserId;
}

// 服务端 → 客户端
export interface ChatReceived {
  type: 'chat';
  from: UserId;
  to: UserId;
  username: string;
  msg: string;
  time: number;
  id?: string;        // 消息 ID（用于回执匹配）
  file?: FileInfo;
}

export interface DeliveredReceipt {
  type: 'delivered';
  id: string;         // 被送达消息的 ID
  time: number;
}

export interface ReadReceiptReceived {
  type: 'read';
  from: UserId;
  username: string;
}

export interface OnlineListMessage {
  type: 'online_list';
  users: User[];
}

export interface UserOnlineMessage {
  type: 'user_online';
  userId: UserId;
  username: string;
}

export interface UserOfflineMessage {
  type: 'user_offline';
  userId: UserId;
}

export interface ErrorMessage {
  type: 'error';
  msg: string;
  id?: string;
}

export type OutgoingMessage = ChatMessage | PingMessage | ReadReceipt;
export type IncomingMessage = ChatReceived | DeliveredReceipt | ReadReceiptReceived | OnlineListMessage | UserOnlineMessage | UserOfflineMessage | ErrorMessage | FriendRequestReceived | FriendAcceptedReceived;

// ---- 好友系统 ----
export interface FriendRequestReceived {
  type: 'friend_request';
  from: UserId;
  username: string;
}

export interface FriendAcceptedReceived {
  type: 'friend_accepted';
  by: UserId;
  username: string;
}

// ---- 会话存储 ----
export interface Session {
  userId: UserId;
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
  fromUserId: UserId;
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
  userId: UserId;
  username: string;
  messages: MessageRecord[];
  unread: number;
}
