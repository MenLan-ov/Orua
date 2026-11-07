// 通过 WebView2 CDP 检查 Orua 前端实机状态（Node 24 原生 WebSocket）
import http from 'node:http';

function get(path) {
  return new Promise((resolve, reject) => {
    http.get({ host: '127.0.0.1', port: 9222, path }, (res) => {
      let d = '';
      res.on('data', (c) => (d += c));
      res.on('end', () => resolve(JSON.parse(d)));
    }).on('error', reject);
  });
}

async function main() {
  const list = await get('/json/list');
  const page = list.find((t) => t.type === 'page');
  if (!page) { console.log('NO PAGE'); return; }
  const wsUrl = page.webSocketDebuggerUrl;
  console.log('ws url:', wsUrl);

  const ws = new WebSocket(wsUrl);
  let id = 0;
  const pending = new Map();
  function send(method, params) {
    return new Promise((resolve, reject) => {
      const mid = ++id;
      pending.set(mid, { resolve, reject });
      ws.send(JSON.stringify({ id: mid, method, params }));
    });
  }
  ws.addEventListener('message', (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.id && pending.has(msg.id)) {
      const { resolve, reject } = pending.get(msg.id);
      pending.delete(msg.id);
      msg.error ? reject(msg.error) : resolve(msg.result);
    }
  });
  await new Promise((r) => ws.addEventListener('open', r));

  // 1. 前端模块级令牌变量
  const tokEval = await send('Runtime.evaluate', {
    expression: '__ORUA_TOKEN', returnByValue: true,
  });
  const token = tokEval.result.value;
  console.log('__ORUA_TOKEN length:', token ? token.length : 0);
  console.log('__ORUA_TOKEN sample:', token ? token.slice(0, 6) + '...' : '(empty)');

  // 2. API_BASE
  const baseEval = await send('Runtime.evaluate', {
    expression: 'API_BASE', returnByValue: true,
  });
  console.log('API_BASE:', baseEval.result.value);

  // 3. 前端用 X-Orua-Token 实际调 /api/v1/bots（跨域 app.local → 127.0.0.1）
  const fetchEval = await send('Runtime.evaluate', {
    expression: `fetch(API_BASE + '/api/v1/bots', { headers: Object.assign({'Content-Type':'application/json'}, __ORUA_TOKEN ? {'X-Orua-Token': __ORUA_TOKEN} : {}) }).then(async r => { const j = await r.json(); return { status: r.status, code: j.code, nBots: j.data ? j.data.bots.length : -1 }; })`,
    awaitPromise: true, returnByValue: true,
  });
  console.log('frontend fetch /bots:', JSON.stringify(fetchEval.result.value));

  // 4. 不带/错误令牌（模拟带 Origin 的攻击者）应 403
  const evilEval = await send('Runtime.evaluate', {
    expression: `fetch(API_BASE + '/api/v1/bots', { headers: {'Content-Type':'application/json', 'X-Orua-Token':'wrong'} }).then(async r => r.status)`,
    awaitPromise: true, returnByValue: true,
  });
  console.log('wrong-token fetch status:', evilEval.result.value);

  // 5. 前端脚注版本
  const verEval = await send('Runtime.evaluate', {
    expression: `document.getElementById('foot-version') ? document.getElementById('foot-version').textContent : '(none)'`,
    returnByValue: true,
  });
  console.log('foot-version:', verEval.result.value);

  ws.close();
  process.exit(0);
}

main().catch((e) => { console.error('ERR', e); process.exit(1); });