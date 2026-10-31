import axios from 'axios';
import type { AuthResponse, UserId, User, MessageRecord } from '../types';

const http = axios.create({
  baseURL: '/api',
  timeout: 10000,
  headers: { 'Content-Type': 'application/json' },
});

/** 注册 */
export async function register(username: string, password: string): Promise<AuthResponse> {
  const res = await http.post<AuthResponse>('/auth/register', { username, password });
  return res.data;
}

/** 登录 */
export async function login(username: string, password: string): Promise<AuthResponse> {
  const res = await http.post<AuthResponse>('/auth/login', { username, password });
  return res.data;
}

/** 登出 */
export async function logout(token: string): Promise<void> {
  await http.post('/auth/logout', { token });
}

/** 获取所有用户 */
export async function fetchUsers(token: string): Promise<User[]> {
  const res = await http.get<User[]>('/users', { params: { token } });
  return res.data;
}

/** 获取消息历史 */
export async function fetchHistory(token: string, withUserId: UserId, limit = 50): Promise<MessageRecord[]> {
  const res = await http.get('/messages/history', {
    params: { token, with: withUserId, limit }
  });
  // 转换后端格式为前端格式
  return (res.data || []).map((m: any) => ({
    id: `hist-${m.created_at}-${m.fromUserId}`,
    fromUserId: m.fromUserId,
    username: '',
    content: m.content,
    time: m.created_at,
    isSelf: false,  // 在 store 中根据 session 修正
  }));
}

export interface UploadResult {
  fileId: string;
  name: string;
  size: number;
  url: string;
  mime: string;
}

/** 上传文件（base64） */
export async function uploadFile(token: string, file: File): Promise<UploadResult> {
  const reader = new FileReader();
  const data = await new Promise<string>((resolve, reject) => {
    reader.onload = () => {
      const result = reader.result as string;
      // 去掉 data:...;base64, 前缀
      const base64 = result.split(',')[1];
      resolve(base64);
    };
    reader.onerror = reject;
    reader.readAsDataURL(file);
  });

  const res = await http.post('/files/upload', {
    name: file.name,
    data,
    size: file.size,
  }, { params: { token } });
  return res.data;
}

export default http;

/** 好友相关 API */
export async function addFriend(token: string, userId: UserId): Promise<any> {
  const res = await http.post('/friends/add', { userId }, { params: { token } });
  return res.data;
}

export async function acceptFriend(token: string, userId: UserId): Promise<any> {
  const res = await http.post('/friends/accept', { userId }, { params: { token } });
  return res.data;
}

export async function rejectFriend(token: string, userId: UserId): Promise<any> {
  const res = await http.post('/friends/reject', { userId }, { params: { token } });
  return res.data;
}

export async function removeFriend(token: string, userId: UserId): Promise<any> {
  const res = await http.post('/friends/remove', { userId }, { params: { token } });
  return res.data;
}

export async function getFriends(token: string): Promise<User[]> {
  const res = await http.get('/friends', { params: { token } });
  return res.data;
}

export async function getFriendRequests(token: string): Promise<User[]> {
  const res = await http.get('/friends/requests', { params: { token } });
  return res.data;
}
