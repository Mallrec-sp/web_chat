import { useChatStore } from '../store/chatStore';
import UserList from '../components/UserList';
import ChatPanel from '../components/ChatPanel';

/**
 * ChatPage — 主聊天页面。
 *
 * 布局：左侧用户列表 | 右侧聊天面板
 */
export default function ChatPage() {
  const activeChat = useChatStore(s => s.activeChat);

  return (
    <div className="chat-page">
      <UserList />
      {activeChat ? (
        <ChatPanel />
      ) : (
        <div className="chat-main">
          <div className="chat-placeholder">
            <div className="icon">💬</div>
            <p>选择一个用户开始聊天</p>
          </div>
        </div>
      )}
    </div>
  );
}
