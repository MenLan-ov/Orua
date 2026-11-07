// 造两个自制插件，用来验证齿轮弹窗、上移、停用态、生效 Bot 显示
import http from 'node:http';

const PORT = Number(process.env.CDP_PORT || 9262);

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
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.text + ' :: ' +
      (r.exceptionDetails.exception?.description || ''));
    return r.result.value;
  }
}

const targets = await httpJson('/json/list');
const page = targets.find((t) => t.type === 'page' && t.url.includes('app.local'));
const cdp = new CDP(page.webSocketDebuggerUrl);
await cdp.connect();
await cdp.send('Runtime.enable');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const token = await cdp.eval(`localStorage.getItem('orua_token') || ''`);
console.log('token:', token ? token.slice(0, 8) + '…' : '(空)');

// 直接用页面里的 App.api（自带 X-Orua-Token），避免手工拼 header
const r = await cdp.eval(`(async () => {
  const mk = async (name, url, events, bot_ids, enabled) => {
    try {
      await App.api.post('/api/v1/plugins', { name, url, events, bot_ids, enabled });
      return 'ok';
    } catch (e) { return 'ERR ' + e.message; }
  };
  const a = await mk('聊天完成通知', 'https://example.com/hook/chat', ['chat.completed'], [], true);
  const b = await mk('错误告警', 'http://127.0.0.1:8765/notify', ['chat.error','session.created'], [1], true);
  const c = await mk('归档任务（停用）', 'https://example.com/hook/archive', ['session.deleted'], [], false);
  await loadPlugins();
  return [a, b, c, App.state.plugins.length];
})()`);
console.log('created:', JSON.stringify(r));

const info = await cdp.eval(`(() => {
  const cards = [...document.querySelectorAll('#plugin-groups .plugin-card')];
  return {
    total: cards.length,
    badge: document.getElementById('plugins-badge').textContent,
    disabled: cards.filter(c => c.classList.contains('disabled')).length,
    gears: document.querySelectorAll('[data-plugin-gear]').length,
    ups: document.querySelectorAll('[data-plugin-up]').length,
    firstUpDisabled: document.querySelector('[data-plugin-up]')?.disabled,
  };
})()`);
console.log('page:', JSON.stringify(info, null, 2));
