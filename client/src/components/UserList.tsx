import { useChatStore } from '../store/chatStore';

/**
 * UserList — 左侧在线用户列表。
 *
 * 显示所有已注册用户及其在线状态。
 * 点击用户开始聊天/切换聊天对象。
 */
export default function UserList() {
  const users = useChatStore(s => s.users);
  const activeChat = useChatStore(s => s.activeChat);
  const selectUser = useChatStore(s => s.selectUser);
  const conversations = useChatStore(s => s.conversations);
  const session = useChatStore(s => s.session);
  const logout = useChatStore(s => s.logout);
  const friendRequestsCount = useChatStore(s => s.friendRequestsCount);

  // 获取用户首字母
  function getInitial(name: string): string {
    return name.charAt(0).toUpperCase();
  }

  return (
    <div className="sidebar">
      <div className="sidebar-header">
        <h2>NetChat</h2>
        <div style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
          {friendRequestsCount > 0 && <span className="unread-badge">📋{friendRequestsCount}</span>}
          <button className="logout-btn" onClick={logout}>退出</button>
        </div>
      </div>

      <div className="sidebar-search">
        <input type="text" placeholder="搜索用户..." />
      </div>

      <div className="user-list">
        {/* 当前用户 */}
        {session && (
          <div className="user-item" style={{ opacity: 0.6 }}>
            <div className="user-avatar">{getInitial(session.username)}</div>
            <div className="user-info">
              <div className="name">{session.username} (我)</div>
              <div className="status">在线</div>
            </div>
            <div className="online-dot" />
          </div>
        )}

        {/* 在线用户列表 */}
        {users
          .filter(u => u.userId !== session?.userId)
          .map(user => {
            const conv = conversations.get(user.userId);
            const unread = conv?.unread || 0;
            return (
              <div
                key={user.userId}
                className={`user-item ${activeChat === user.userId ? 'active' : ''}`}
                onClick={() => selectUser(user.userId)}
              >
                <div className="user-avatar">{getInitial(user.username)}</div>
                <div className="user-info">
                  <div className="name">{user.username}</div>
                  <div className="status">在线</div>
                </div>
                {unread > 0 && <div className="unread-badge">{unread}</div>}
                <div className="online-dot" />
              </div>
            );
          })}
      </div>
    </div>
  );
}
