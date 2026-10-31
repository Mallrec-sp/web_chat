import { useState, FormEvent } from 'react';
import { Link, useNavigate } from 'react-router-dom';
import { useChatStore } from '../store/chatStore';

export default function LoginPage() {
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState(() => sessionStorage.getItem('netchat_login_notice') || '');
  const [loading, setLoading] = useState(false);
  const login = useChatStore(s => s.login);
  const navigate = useNavigate();

  async function handleSubmit(e: FormEvent) {
    e.preventDefault();
    setError('');
    setLoading(true);
    try {
      const err = await login(username, password);
      if (err) {
        setError(err);
      } else {
        sessionStorage.removeItem('netchat_login_notice');
        navigate('/chat');
      }
    } catch (e: any) {
      setError(e?.response?.data?.error || '登录失败，请重试');
    }
    setLoading(false);
  }

  return (
    <div className="auth-page">
      <form className="auth-card" onSubmit={handleSubmit}>
        <h1>NetChat</h1>
        <p className="subtitle">企业级即时通讯</p>

        {error && <div className="error-msg">{error}</div>}

        <div className="form-group">
          <label>用户名</label>
          <input
            type="text"
            value={username}
            onChange={e => setUsername(e.target.value)}
            placeholder="请输入用户名"
            autoFocus
            required
          />
        </div>

        <div className="form-group">
          <label>密码</label>
          <input
            type="password"
            value={password}
            onChange={e => setPassword(e.target.value)}
            placeholder="请输入密码"
            required
          />
        </div>

        <button type="submit" className="btn btn-primary" disabled={loading}>
          {loading ? '登录中...' : '登 录'}
        </button>

        <div className="auth-link">
          还没有账号？<Link to="/register">立即注册</Link>
        </div>
      </form>
    </div>
  );
}
