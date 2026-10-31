import { BrowserRouter, Routes, Route, Navigate } from 'react-router-dom'
import { useEffect, useState } from 'react'
import LoginPage from './pages/LoginPage'
import RegisterPage from './pages/RegisterPage'
import ChatPage from './pages/ChatPage'
import { useChatStore } from './store/chatStore'
import { parseSavedSession } from './api/session'

/** 登录态保护路由：未登录跳转到 /login */
function ProtectedRoute({ children }: { children: React.ReactNode }) {
  const session = useChatStore(s => s.session);
  if (!session) return <Navigate to="/login" replace />;
  return <>{children}</>;
}

export default function App() {
  const [loaded, setLoaded] = useState(false);

  // 页面加载时恢复已保存的会话
  useEffect(() => {
    const saved = localStorage.getItem('netchat_session');
    let timer: ReturnType<typeof setTimeout> | undefined;
    if (saved) {
      const session = parseSavedSession(saved);
      if (session) {
        useChatStore.setState({ session, token: session.token });
        // 延迟初始化；清理定时器以避免 StrictMode 重复连接。
        timer = setTimeout(() => {
          useChatStore.getState()._initWs();
          useChatStore.getState().loadUsers();
        }, 100);
      } else {
        localStorage.removeItem('netchat_session');
        sessionStorage.setItem('netchat_login_notice', '登录信息格式已更新，请重新登录。');
      }
    }
    setLoaded(true);
    return () => clearTimeout(timer);
  }, []);

  if (!loaded) {
    return <div className="app-loading">Loading...</div>;
  }

  return (
    <BrowserRouter>
      <Routes>
        <Route path="/login" element={<LoginPage />} />
        <Route path="/register" element={<RegisterPage />} />
        <Route path="/chat" element={
          <ProtectedRoute>
            <ChatPage />
          </ProtectedRoute>
        } />
        <Route path="*" element={<Navigate to="/chat" replace />} />
      </Routes>
    </BrowserRouter>
  );
}
