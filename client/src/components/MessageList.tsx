import { useEffect, useRef } from 'react';
import type { MessageRecord } from '../types';
import { useChatStore } from '../store/chatStore';

interface Props {
  messages: MessageRecord[];
}

function formatFileSize(bytes: number): string {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
  return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
}

/**
 * MessageList — 消息列表。
 *
 * 自动滚动到底部。
 * 左侧显示对方消息（灰背景），右侧显示自己消息（紫色背景）。
 */
export default function MessageList({ messages }: Props) {
  const bottomRef = useRef<HTMLDivElement>(null);
  const session = useChatStore(s => s.session);

  // 新消息时自动滚动到底部
  useEffect(() => {
    bottomRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [messages.length]);

  function formatTime(ts: number): string {
    const d = new Date(ts);
    return `${d.getHours().toString().padStart(2, '0')}:${d.getMinutes().toString().padStart(2, '0')}`;
  }

  if (messages.length === 0) {
    return (
      <div className="message-list" style={{ justifyContent: 'center', alignItems: 'center', color: '#999' }}>
        发送第一条消息开始聊天吧 💬
      </div>
    );
  }

  return (
    <div className="message-list">
      {messages.map(msg => (
        <div
          key={msg.id}
          className={`message ${msg.isSelf ? 'self' : 'other'}`}
        >
          <div className="content">
            {msg.content}
            {msg.file && (
              <div className="file-attachment">
                <a
                  href={`/api/files/download?id=${msg.file.id}&token=${session?.token}`}
                  target="_blank"
                  rel="noreferrer"
                  className="file-link"
                  onClick={e => e.stopPropagation()}
                >
                  <span className="file-icon">📄</span>
                  <span className="file-info">
                    <span className="file-name">{msg.file.name}</span>
                    <span className="file-size">{formatFileSize(msg.file.size)}</span>
                  </span>
                </a>
              </div>
            )}
          </div>
          <div className="time">
            {formatTime(msg.time)}
            {msg.isSelf && msg.status && (
              <span className={`status-icon status-${msg.status}`}>
                {msg.status === 'sending' ? '⏳' :
                 msg.status === 'sent' ? '✓' :
                 msg.status === 'delivered' ? '✓✓' :
                 msg.status === 'read' ? '✓✓' : ''}
              </span>
            )}
          </div>
        </div>
      ))}
      <div ref={bottomRef} />
    </div>
  );
}
