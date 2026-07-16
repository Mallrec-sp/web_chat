import { useChatStore } from '../store/chatStore';
import MessageList from './MessageList';
import MessageInput from './MessageInput';

/**
 * ChatPanel — 中间聊天面板。
 *
 * 显示当前聊天对象的对话：
 * - 顶部：对方用户名
 * - 中间：消息列表
 * - 底部：输入框
 */
export default function ChatPanel() {
  const activeChat = useChatStore(s => s.activeChat);
  const users = useChatStore(s => s.users);
  const conversations = useChatStore(s => s.conversations);

  // 获取当前聊天对象
  const targetUser = users.find(u => u.userId === activeChat);
  const conversation = activeChat ? conversations.get(activeChat) : undefined;

  const username = targetUser?.username || conversation?.username || `用户 #${activeChat}`;

  return (
    <div className="chat-main">
      <div className="chat-header">
        <div className="user-avatar" style={{ width: 36, height: 36, fontSize: 14 }}>
          {username.charAt(0).toUpperCase()}
        </div>
        <h3>{username}</h3>
      </div>

      <MessageList messages={conversation?.messages || []} />
      <MessageInput />
    </div>
  );
}
