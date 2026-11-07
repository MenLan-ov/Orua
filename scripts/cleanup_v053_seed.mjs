// 清理验证过程中写进用户真实配置的测试数据：
// 删掉 3 个 example.com 测试插件，并清掉给 http_get 塞的示例参数。
import http from 'node:http';

const PORT = Number(process.env.CDP_PORT || 9267);
function httpJson(path) {
  return new Promise((resolve, reject) => {
    http.get({ host: '127.0.0.1', port: PORT, path }, (res) => {
      let b = '';
      res.on('data', (c) => (b += c));
      res.on('end', () => { try { resolve(JSON.parse(b)); } catch (e) { reject(e); } });
    }).on('error', reject);
  });
}
class CDP {
  constructor(url) { this.url = url; this.id = 0; this.pending = new Map(); }
  connect() {
    return new Promise((resolve, reject) => {
      this.ws = new WebSocket(this.url);
      this.ws.addEventListener('open', () => resolve());
      this.ws.addEventListener('error', reject);
      this.ws.addEventListener('message', (ev) => {
        const m = JSON.parse(ev.data);
        if (m.id && this.pending.has(m.id)) {
          const { resolve: r, reject: j } = this.pending.get(m.id);
          this.pending.delete(m.id);
          m.error ? j(new Error(JSON.stringify(m.error))) : r(m.result);
        }
      });
    });
  }
  send(method, params = {}) {
    const id = ++this.id;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.ws.send(JSON.stringify({ id, method, params }));
    });
  }
  async eval(expr) {
    const r = await this.send('Runtime.evaluate', {
      expression: expr, returnByValue: true, awaitPromise: true,
    });
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.text);
    return r.result.value;
  }
}

const targets = await httpJson('/json/list');
const page = targets.find((t) => t.type === 'page' && t.url.includes('app.local'));
const cdp = new CDP(page.webSocketDebuggerUrl);
await cdp.connect();
await cdp.send('Runtime.enable');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const r = await cdp.eval(`(async () => {
  const removed = [];
  for (const p of (App.state.plugins || []).slice()) {
    // 只删本次验证造的：URL 指向 example.com 或本机 18766/8765 测试端点
    if (/example\\.com|127\\.0\\.0\\.1:18766|127\\.0\\.0\\.1:8765\\/notify/.test(p.url)) {
      try {
        await App.api.del('/api/v1/plugins/' + encodeURIComponent(p.name));
        removed.push(p.name);
      } catch (e) { removed.push('ERR:' + p.name + ':' + e.message); }
    }
  }
  // 清掉验证时给 http_get 塞的示例参数
  const botId = App.state.pluginBotId;
  if (botId) {
    try {
      await App.api.put('/api/v1/bots/' + botId + '/skills/http_get/params', { params: {} });
    } catch (e) { removed.push('params:' + e.message); }
  }
  await loadPlugins();
  return { removed, left: (App.state.plugins || []).map(p => p.name + ' → ' + p.url) };
})()`);
console.log(JSON.stringify(r, null, 2));
