import type { IncomingMessage, OutgoingMessage } from '../types';

type MessageHandler = (msg: IncomingMessage) => void;

/**
 * WebSocket 客户端封装。
 *
 * 职责：
 *   1. 连接管理（自动重连、心跳）
 *   2. 消息发送（JSON 编码 + 队列）
 *   3. 消息接收（JSON 解码 + 分发）
 *
 * 讲解点：为什么自己封装而不是用原生 WebSocket？
 *   原生 WebSocket 太底层了——没有自动重连、没有心跳、
 *   连接断开后消息丢失。封装后我们在上层统一处理这些问题。
 */
export class WsClient {
  private ws: WebSocket | null = null;
  private url = '';
  private handlers: MessageHandler[] = [];
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private destroyed = false;

  /** 连接 WebSocket */
  connect(token: string) {
    if (this.ws) this.disconnect();

    // 判断当前环境：开发模式用 localhost，生产模式用当前域名
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const host = window.location.host;
    this.url = `${protocol}//${host}/ws?token=${token}`;

    this.destroyed = false;
    this.createConnection();
  }

  private createConnection() {
    if (this.destroyed) return;

    try {
      this.ws = new WebSocket(this.url);
    } catch (e) {
      console.error('WebSocket connection error:', e);
      this.scheduleReconnect();
      return;
    }

    this.ws.onopen = () => {
      console.log('WebSocket connected');
      this.startPing();
    };

    this.ws.onmessage = (event) => {
      try {
        const msg = JSON.parse(event.data) as IncomingMessage;
        this.handlers.forEach(h => h(msg));
      } catch (e) {
        console.error('Failed to parse WS message:', e);
      }
    };

    this.ws.onclose = () => {
      console.log('WebSocket closed');
      this.stopPing();
      if (!this.destroyed) {
        this.scheduleReconnect();
      }
    };

    this.ws.onerror = (err) => {
      console.error('WebSocket error:', err);
    };
  }

  /** 注册消息处理器 */
  onMessage(handler: MessageHandler) {
    this.handlers.push(handler);
    return () => {
      this.handlers = this.handlers.filter(h => h !== handler);
    };
  }

  /** 发送消息 */
  send(msg: OutgoingMessage) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(msg));
    } else {
      console.warn('Cannot send, WebSocket not connected');
    }
  }

  /** 断开连接 */
  disconnect() {
    this.destroyed = true;
    this.stopPing();
    this.clearReconnect();
    if (this.ws) {
      this.ws.onclose = null;
      this.ws.close();
      this.ws = null;
    }
  }

  /** 重新连接 */
  private scheduleReconnect() {
    this.clearReconnect();
    this.reconnectTimer = setTimeout(() => {
      console.log('Reconnecting WebSocket...');
      this.createConnection();
    }, 3000);
  }

  private clearReconnect() {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
  }

  /** 心跳：每 30 秒发一次 ping */
  private startPing() {
    this.pingTimer = setInterval(() => {
      this.send({ type: 'ping' });
    }, 30000);
  }

  private stopPing() {
    if (this.pingTimer) {
      clearInterval(this.pingTimer);
      this.pingTimer = null;
    }
  }
}

/** 全局 WebSocket 客户端实例 */
export const wsClient = new WsClient();
