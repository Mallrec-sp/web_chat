import { useState, FormEvent } from 'react';
import { Link, useNavigate } from 'react-router-dom';
import { useChatStore } from '../store/chatStore';

export default function RegisterPage() {
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [confirm, setConfirm] = useState('');
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(false);
  const register = useChatStore(s => s.register);
  const navigate = useNavigate();

  async function handleSubmit(e: FormEvent) {
    e.preventDefault();
    setError('');

    if (password !== confirm) {
      setError('两次密码输入不一致');
      return;
    }
    if (password.length < 4) {
      setError('密码至少 4 位');
      return;
    }

    setLoading(true);
    try {
      const err = await register(username, password);
      if (err) {
        setError(err);
      } else {
        navigate('/chat');
      }
    } catch (e: any) {
      setError(e?.response?.data?.error || '注册失败，请重试');
    }
    setLoading(false);
  }

  return (
    <div className="auth-page">
      <form className="auth-card" onSubmit={handleSubmit}>
        <h1>NetChat</h1>
        <p className="subtitle">注册新账号</p>

        {error && <div className="error-msg">{error}</div>}

        <div className="form-group">
          <label>用户名</label>
          <input
            type="text"
            value={username}
            onChange={e => setUsername(e.target.value)}
            placeholder="2-32 个字符"
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
            placeholder="至少 4 位"
            required
          />
        </div>

        <div className="form-group">
          <label>确认密码</label>
          <input
            type="password"
            value={confirm}
            onChange={e => setConfirm(e.target.value)}
            placeholder="再次输入密码"
            required
          />
        </div>

        <button type="submit" className="btn btn-primary" disabled={loading}>
          {loading ? '注册中...' : '注 册'}
        </button>

        <div className="auth-link">
          已有账号？<Link to="/login">立即登录</Link>
        </div>
      </form>
    </div>
  );
}
