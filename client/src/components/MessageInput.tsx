import { useState, KeyboardEvent, useRef } from 'react';
import { useChatStore } from '../store/chatStore';
import { uploadFile } from '../api/http';

export default function MessageInput() {
  const [text, setText] = useState('');
  const [uploading, setUploading] = useState(false);
  const activeChat = useChatStore(s => s.activeChat);
  const token = useChatStore(s => s.token);
  const sendMessage = useChatStore(s => s.sendMessage);
  const fileInputRef = useRef<HTMLInputElement>(null);

  function handleSend() {
    const trimmed = text.trim();
    if (!trimmed || !activeChat) return;
    sendMessage(activeChat, trimmed);
    setText('');
  }

  function handleKeyDown(e: KeyboardEvent) {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      handleSend();
    }
  }

  async function handleFileSelect(e: React.ChangeEvent<HTMLInputElement>) {
    const file = e.target.files?.[0];
    if (!file || !activeChat || !token) return;

    setUploading(true);
    try {
      const result = await uploadFile(token, file);
      // 发送文件消息
      const fileInfo = {
        id: result.fileId,
        name: result.name,
        size: result.size,
        url: result.url,
        mime: result.mime,
      };
      // 用 WebSocket 发送包含文件附件的消息
      const wsClient = (await import('../api/ws')).wsClient;
      wsClient.send({
        type: 'chat',
        to: activeChat,
        msg: file.name,
        file: fileInfo,
      } as any);
    } catch (e) {
      console.error('File upload failed:', e);
    }
    setUploading(false);
    // 清空 file input 以便重复选择同一文件
    if (fileInputRef.current) fileInputRef.current.value = '';
  }

  return (
    <div className="message-input-area">
      <button
        className="attach-btn"
        onClick={() => fileInputRef.current?.click()}
        disabled={!activeChat || uploading}
        title="上传文件"
      >
        📎
      </button>
      <input
        ref={fileInputRef}
        type="file"
        style={{ display: 'none' }}
        onChange={handleFileSelect}
      />
      <input
        type="text"
        value={text}
        onChange={e => setText(e.target.value)}
        onKeyDown={handleKeyDown}
        placeholder={uploading ? '上传中...' : '输入消息，按 Enter 发送'}
        disabled={!activeChat || uploading}
      />
      <button
        className="send-btn"
        onClick={handleSend}
        disabled={!text.trim() || !activeChat || uploading}
      >
        ➤
      </button>
    </div>
  );
}
