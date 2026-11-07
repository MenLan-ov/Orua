// 通过 WebView2 CDP 检查 Orua 前端实机状态（Node 24 原生 WebSocket）
import http from 'node:http';

const CDP_PORT = Number(process.env.CDP_PORT || 9222);

function get(path) {
  return new Promise((resolve, reject) => {
    http.get({ host: '127.0.0.1', port: CDP_PORT, path }, (res) => {
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

  // 6. 真实点击「发送」，断言聊天请求打到后端而不是虚拟主机。
  //    这一步是回归防线：consumeSse 曾用裸相对路径 fetch(url)，请求被解析成
  //    https://app.local/api/v1/chat/stream → net::ERR_FILE_NOT_FOUND → 界面
  //    只显示 "Failed to fetch"。此前所有测试都用相对路径 stub 或 curl 直打后端，
  //    从未在真实 WebView2 里点过发送，所以该 bug 从 v1 潜伏到 v5.0。
  await send('Runtime.evaluate', {
    expression: `(() => {
      const ta = document.querySelector('textarea#input, textarea');
      if (ta) { ta.value = '冒烟回归'; ta.dispatchEvent(new Event('input', {bubbles:true})); }
      const b = [...document.querySelectorAll('button')].find(x => (x.textContent||'').trim() === '发送');
      if (b) b.click();
      return b ? 'clicked' : 'no-button';
    })()`,
    returnByValue: true,
  });
  await new Promise((r) => setTimeout(r, 5000));

  const chatEval = await send('Runtime.evaluate', {
    expression: `(() => {
      const t = (document.querySelector('.chat-body, #chat-body, .messages')?.innerText) || '';
      const failed = /Failed to fetch/.test(t);
      const hasErr = /⚠|错误|失败/.test(t);
      return { hasFailedToFetch: failed, hasErrorBubble: hasErr, tail: t.slice(-200) };
    })()`,
    returnByValue: true,
  });
  console.log('chat click result:', JSON.stringify(chatEval.result.value));
  if (chatEval.result.value && chatEval.result.value.hasFailedToFetch) {
    console.log('FAIL: 聊天请求未打到后端（consumeSse 漏拼 API_BASE 类回归）');
  } else {
    console.log('PASS: 聊天请求路径正确，无 "Failed to fetch"');
  }

  ws.close();
  process.exit(0);
}

main().catch((e) => { console.error('ERR', e); process.exit(1); });