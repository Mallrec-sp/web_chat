import type { Session } from '../types';

/** 旧版数字 ID 已可能丢失精度，只能重新登录获取准确的字符串 ID。 */
export function parseSavedSession(saved: string): Session | null {
  try {
    const session = JSON.parse(saved);
    if (!session || typeof session.userId !== 'string' ||
        !/^[1-9][0-9]*$/.test(session.userId) || session.userId.length > 19 ||
        BigInt(session.userId) > 9223372036854775807n ||
        typeof session.username !== 'string' || !session.username ||
        typeof session.token !== 'string' || !session.token) return null;
    return { userId: session.userId, username: session.username, token: session.token };
  } catch {
    return null;
  }
}
