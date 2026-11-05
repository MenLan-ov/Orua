'use strict';

const API_BASE = window.__API_BASE__ || 'http://127.0.0.1:8765';

/* ================= 核心对象：命令注册表 + 事件 + API ================= */
const App = {
  state: {
    sessions: [],
    currentSessionId: null,
    personas: [],
    activePersonaId: null,
    config: null,
    streaming: false,
    abortCtrl: null,
  },
  commands: new Map(),
  listeners: {},

  registerCommand(name, fn) { this.commands.set(name, fn); },
  run(name, ...args) {
    const fn = this.commands.get(name);
    if (fn) { try { return fn(...args); } catch (e) { console.error('[cmd]', name, e); toast(e.message || String(e)); } }
    else console.warn('unknown command:', name);
  },
  on(type, fn) { (this.listeners[type] ||= []).push(fn); },
  emit(type, data) { (this.listeners[type] || []).forEach(fn => { try { fn(data); } catch (e) { console.error(e); } }); },

  api: {
    async req(method, path, body) {
      const opts = { method, headers: {} };
      if (body !== undefined) {
        opts.headers['Content-Type'] = 'application/json';
        opts.body = JSON.stringify(body);
      }
      const res = await fetch(API_BASE + path, opts);
      if (!res.ok) {
        let msg = 'HTTP ' + res.status;
        try { msg = (await res.json()).message || msg; } catch (_) {}
        throw new Error(msg);
      }
      const j = await res.json();
      if (j.code !== 0) throw new Error(j.message || '请求失败');
      return j.data;
    },
    get: (p) => App.api.req('GET', p),
    post: (p, b) => App.api.req('POST', p, b),
    patch: (p, b) => App.api.req('PATCH', p, b),
    del: (p) => App.api.req('DELETE', p),
  },
};

/* ================= DOM 与工具 ================= */
const $ = (id) => document.getElementById(id);
const messagesEl = $('messages');
const inputEl = $('input');
const sessionListEl = $('session-list');
const personaRowEl = $('persona-switcher');

function escapeHtml(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
          .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}
let toastTimer = null;
function toast(msg) {
  const t = $('toast');
  t.textContent = msg;
  t.classList.remove('hidden');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.add('hidden'), 2600);
}
function setBanner(msg) {
  const b = $('banner');
  if (!msg) { b.classList.add('hidden'); return; }
  b.textContent = msg;
  b.classList.remove('hidden');
}
function renderMarkdown(text) {
  const raw = marked.parse(text, { breaks: true });
  const clean = DOMPurify.sanitize(raw);
  const div = document.createElement('div');
  div.innerHTML = clean;
  div.querySelectorAll('pre code').forEach(block => {
    try { hljs.highlightElement(block); } catch (_) {}
  });
  return div.innerHTML;
}
function applyTheme(theme) {
  document.documentElement.dataset.theme = theme || 'dark';
}
function nearBottom() {
  return messagesEl.scrollHeight - messagesEl.scrollTop - messagesEl.clientHeight < 90;
}
function scrollToBottom(force = false) {
  if (force || nearBottom()) messagesEl.scrollTop = messagesEl.scrollHeight;
}

// WebView2 不支持 window.prompt（恒返回 null），提供自绘输入弹窗
function promptText(title, def = '') {
  return new Promise((resolve) => {
    const overlay = document.createElement('div');
    overlay.className = 'modal';
    const box = document.createElement('div');
    box.className = 'modal-box';
    box.style.width = '360px';
    const h = document.createElement('h3');
    h.textContent = title;
    const input = document.createElement('input');
    input.value = def;
    const actions = document.createElement('div');
    actions.className = 'modal-actions';
    const ok = document.createElement('button');
    ok.className = 'btn-primary';
    ok.textContent = '确定';
    const cancel = document.createElement('button');
    cancel.className = 'btn-ghost';
    cancel.textContent = '取消';
    actions.append(cancel, ok);
    box.append(h, input, actions);
    overlay.appendChild(box);
    document.body.appendChild(overlay);
    const close = (val) => { overlay.remove(); resolve(val); };
    ok.addEventListener('click', () => close(input.value));
    cancel.addEventListener('click', () => close(null));
    input.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); close(input.value); }
      if (e.key === 'Escape') close(null);
    });
    overlay.addEventListener('mousedown', (e) => { if (e.target === overlay) close(null); });
    input.focus();
    input.select();
  });
}

/* ================= 打字机 ================= */
class Typewriter {
  constructor(el, onDrain) {
    this.el = el;
    this.onDrain = onDrain || null;
    this.buf = '';
    this.timer = null;
    this.finished = false;
    this.drained = false;
    this.tick = this.tick.bind(this);
  }
  push(text) {
    this.buf += text;
    if (!this.timer) this.timer = setInterval(this.tick, 24);
  }
  tick() {
    if (!this.buf) {
      if (this.finished && this.timer) {
        clearInterval(this.timer);
        this.timer = null;
        this.fireDrain();
      }
      return;
    }
    const n = Math.min(4, this.buf.length);
    this.el.textContent += this.buf.slice(0, n);
    this.buf = this.buf.slice(n);
  }
  fireDrain() {
    if (this.drained || !this.onDrain) return;
    this.drained = true;
    this.onDrain();
  }
  finish() { // 标记完成；缓冲全部排空后回调 onDrain
    this.finished = true;
    if (!this.timer) {
      this.el.textContent += this.buf;
      this.buf = '';
      this.fireDrain();
    }
  }
}

/* ================= 消息气泡 ================= */
function addBubble(role, text, { streaming = false, id = null } = {}) {
  const wrap = document.createElement('div');
  wrap.className = 'msg ' + role;
  wrap.dataset.msgId = id || '';

  const who = document.createElement('div');
  who.className = 'who';
  who.textContent = role === 'user' ? '我' : (App.state.personas.find(p => p.id === App.state.activePersonaId)?.name || 'AI');

  const bubble = document.createElement('div');
  bubble.className = 'bubble';
  const content = document.createElement('div');
  content.className = 'msg-content' + (streaming ? ' typing' : '');
  if (!streaming) content.innerHTML = renderMarkdown(text);
  bubble.appendChild(content);
  wrap.appendChild(who);
  wrap.appendChild(bubble);

  const meta = document.createElement('div');
  meta.className = 'msg-meta';
  meta.innerHTML =
    '<span class="m-time">' + new Date().toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit' }) + '</span>' +
    '<button class="m-copy" data-action="copy">复制</button>' +
    (role === 'assistant' && !streaming ? '<button class="m-regen" data-action="regenerate">重新生成</button>' : '') +
    '<button class="m-del" data-action="delete">删除</button>';
  wrap.appendChild(meta);

  const empty = messagesEl.querySelector('.empty-tip');
  if (empty) empty.remove();
  messagesEl.appendChild(wrap);
  scrollToBottom(true);
  return {
    root: wrap,
    contentEl: content,
    finalize(fullText) {
      content.classList.remove('typing');
      content.innerHTML = renderMarkdown(fullText);
      const regen = document.createElement('button');
      regen.className = 'm-regen';
      regen.dataset.action = 'regenerate';
      regen.textContent = '重新生成';
      meta.insertBefore(regen, meta.querySelector('.m-del'));
    },
    showError(msg) {
      content.classList.remove('typing');
      const note = document.createElement('div');
      note.className = 'err-note';
      note.textContent = '⚠ ' + msg;
      wrap.appendChild(note);
    },
  };
}

/* ================= SSE 消费 ================= */
async function consumeSse(path, payload, { onDelta, onDone, onError }) {
  App.state.abortCtrl = new AbortController();
  let res;
  try {
    res = await fetch(API_BASE + path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
      signal: App.state.abortCtrl.signal,
    });
  } catch (e) {
    if (e.name === 'AbortError') return;
    throw new Error('无法连接后端服务');
  }
  if (!res.ok || !res.body) {
    let msg = 'HTTP ' + res.status;
    try { msg = (await res.json()).message || msg; } catch (_) {}
    throw new Error(msg);
  }
  const reader = res.body.getReader();
  const dec = new TextDecoder();
  let buf = '';
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    buf += dec.decode(value, { stream: true });
    let idx;
    while ((idx = buf.indexOf('\n\n')) >= 0) {
      const block = buf.slice(0, idx);
      buf = buf.slice(idx + 2);
      let ev = 'message', data = '';
      for (const line of block.split('\n')) {
        const t = line.replace(/\r$/, '');
        if (t.startsWith('event:')) ev = t.slice(6).trim();
        else if (t.startsWith('data:')) data += (data ? '\n' : '') + t.slice(5).trim();
      }
      if (!data) continue;
      let j;
      try { j = JSON.parse(data); } catch (_) { continue; }
      if (ev === 'delta') onDelta(j.content || '');
      else if (ev === 'done') onDone(j);
      else if (ev === 'error') onError(j);
    }
  }
}

function setStreamingUI(on) {
  App.state.streaming = on;
  $('btn-send').classList.toggle('hidden', on);
  $('btn-stop').classList.toggle('hidden', !on);
  $('stream-hint').classList.toggle('hidden', !on);
}

/* ================= 会话管理 ================= */
async function refreshSessions(silent = false) {
  App.state.sessions = await App.api.get('/api/v1/sessions');
  if (!App.state.sessions.length) App.state.currentSessionId = null;
  else if (!App.state.sessions.some(s => s.id === App.state.currentSessionId))
    App.state.currentSessionId = App.state.sessions[0].id;
  renderSessions();
  if (!silent && App.state.currentSessionId) loadMessages(App.state.currentSessionId);
}

function renderSessions() {
  sessionListEl.innerHTML = '';
  if (!App.state.sessions.length) {
    sessionListEl.innerHTML = '<div class="empty-tip">还没有会话<br>点击"＋ 新对话"开始</div>';
    return;
  }
  for (const s of App.state.sessions) {
    const item = document.createElement('div');
    item.className = 'session-item' + (s.id === App.state.currentSessionId ? ' active' : '');
    item.innerHTML =
      '<span class="s-title"></span>' +
      '<span class="s-actions">' +
      '<button data-sact="rename" title="重命名">✎</button>' +
      '<button class="s-del" data-sact="delete" title="删除">✕</button>' +
      '</span>';
    item.querySelector('.s-title').textContent = s.title;
    item.addEventListener('click', (e) => {
      if (e.target.closest('.s-actions')) return;
      App.run('select-session', s.id);
    });
    item.querySelector('[data-sact="rename"]').addEventListener('click', () => App.run('rename-session', s.id));
    item.querySelector('[data-sact="delete"]').addEventListener('click', () => App.run('delete-session', s.id));
    sessionListEl.appendChild(item);
  }
  $('chat-title').textContent =
    App.state.sessions.find(s => s.id === App.state.currentSessionId)?.title || '新对话';
}

async function loadMessages(sessionId) {
  const msgs = await App.api.get(`/api/v1/messages?session_id=${sessionId}&limit=200`);
  messagesEl.innerHTML = '';
  if (!msgs.length) {
    messagesEl.innerHTML = '<div class="empty-tip">开始新的对话吧 ✦</div>';
    return;
  }
  // 后端按倒序返回（新在前），渲染时翻转为旧→新
  for (const m of [...msgs].reverse()) addBubble(m.role, m.content, { id: m.id });
  scrollToBottom(true);
}

/* ================= 人设 ================= */
async function refreshPersonas() {
  App.state.personas = await App.api.get('/api/v1/personas');
  App.state.activePersonaId = App.state.personas.find(p => p.is_active)?.id || null;
  renderPersonas();
}
function renderPersonas() {
  personaRowEl.innerHTML = '';
  for (const p of App.state.personas) {
    const chip = document.createElement('button');
    chip.className = 'persona-chip' + (p.id === App.state.activePersonaId ? ' active' : '');
    chip.innerHTML =
      '<span>' + escapeHtml(p.avatar) + '</span><span>' + escapeHtml(p.name) + '</span>' +
      '<span class="chip-edit" data-pact="edit">✎</span>' +
      (p.id === App.state.activePersonaId ? '' : '<span class="chip-edit" data-pact="del">✕</span>');
    chip.addEventListener('click', (e) => {
      if (e.target.closest('[data-pact="edit"]')) { App.run('open-persona-editor', p); return; }
      if (e.target.closest('[data-pact="del"]')) { App.run('delete-persona', p.id); return; }
      App.run('activate-persona', p.id);
    });
    personaRowEl.appendChild(chip);
  }
  const add = document.createElement('button');
  add.className = 'persona-chip';
  add.textContent = '＋ 新人设';
  add.addEventListener('click', () => App.run('open-persona-editor', null));
  personaRowEl.appendChild(add);
}

/* ================= 命令实现 ================= */
App.registerCommand('boot', async () => {
  try {
    App.state.config = await App.api.get('/api/v1/config');
  } catch (_) {
    App.state.config = { theme: 'dark' };
    setBanner('后端服务未连接，请重新打开程序');
    return;
  }
  applyTheme(App.state.config.theme);
  await Promise.all([refreshPersonas(), refreshSessions()]);
  const last = localStorage.getItem('last_session');
  if (last && App.state.sessions.some(s => String(s.id) === last)) {
    App.state.currentSessionId = Number(last);
    renderSessions();
    loadMessages(App.state.currentSessionId);
  }
});

App.registerCommand('backend-status', (payload) => {
  if (payload.status === 'down') setBanner('后端连接中断：' + (payload.detail || '正在重试…'));
  else setBanner('');
});

App.registerCommand('new-session', async () => {
  const s = await App.api.post('/api/v1/sessions', {});
  App.state.currentSessionId = s.id;
  localStorage.setItem('last_session', String(s.id));
  await refreshSessions();
  loadMessages(s.id);
});

App.registerCommand('select-session', async (id) => {
  App.state.currentSessionId = id;
  localStorage.setItem('last_session', String(id));
  renderSessions();
  await loadMessages(id);
});

App.registerCommand('rename-session', async (id) => {
  const cur = App.state.sessions.find(s => s.id === id);
  const title = await promptText('会话名称', cur?.title || '');
  if (title === null || !title.trim()) return;
  await App.api.patch(`/api/v1/sessions/${id}`, { title: title.trim() });
  await refreshSessions();
});

App.registerCommand('delete-session', async (id) => {
  if (!confirm('删除该会话及其全部消息？')) return;
  await App.api.del(`/api/v1/sessions/${id}`);
  await refreshSessions();
  if (App.state.currentSessionId) loadMessages(App.state.currentSessionId);
  else { messagesEl.innerHTML = '<div class="empty-tip">开始新的对话吧 ✦</div>'; }
});

App.registerCommand('send-message', async () => {
  if (App.state.streaming) return;
  const text = inputEl.value.trim();
  if (!text) return;
  const sid = App.state.currentSessionId;
  if (!sid) { toast('请先创建会话'); return; }
  if (!App.state.config || !App.state.config.model || !App.state.config.base_url) {
    toast('请先在设置中填写 API 配置');
    App.run('open-settings');
    return;
  }
  inputEl.value = '';
  addBubble('user', text);
  setStreamingUI(true);
  const bubble = addBubble('assistant', '', { streaming: true });
  let full = '';
  const tw = new Typewriter(bubble.contentEl, () => { bubble.finalize(full); scrollToBottom(); });
  let errored = false;
  try {
    await consumeSse('/api/v1/chat/stream', { session_id: sid, content: text }, {
      onDelta: (d) => { full += d; tw.push(d); scrollToBottom(); },
      onDone: () => {
        tw.finish();
        App.run('refresh-after-chat');
      },
      onError: (e) => {
        errored = true;
        tw.finish();
        bubble.showError(e.message || '生成失败');
        App.run('refresh-after-chat');
      },
    });
  } catch (e) {
    if (!errored) {
      tw.finish();
      bubble.showError(e.message);
      App.run('refresh-after-chat');
    }
  }
});

App.registerCommand('refresh-after-chat', async () => {
  setStreamingUI(false);
  await refreshSessions(true);
  if (App.state.currentSessionId) loadMessages(App.state.currentSessionId);
});

App.registerCommand('stop-stream', async () => {
  if (!App.state.streaming) return;
  const sid = App.state.currentSessionId;
  try { App.state.abortCtrl?.abort(); } catch (_) {}
  if (sid) App.api.post('/api/v1/chat/stop', { session_id: sid }).catch(() => {});
  App.run('refresh-after-chat');
});

App.registerCommand('regenerate', async () => {
  if (App.state.streaming) { toast('正在生成中…'); return; }
  const sid = App.state.currentSessionId;
  if (!sid) return;
  setStreamingUI(true);
  const bubble = addBubble('assistant', '', { streaming: true });
  let full = '';
  const tw = new Typewriter(bubble.contentEl, () => { bubble.finalize(full); scrollToBottom(); });
  try {
    await consumeSse('/api/v1/chat/regenerate', { session_id: sid }, {
      onDelta: (d) => { full += d; tw.push(d); scrollToBottom(); },
      onDone: () => { tw.finish(); App.run('refresh-after-chat'); },
      onError: (e) => { tw.finish(); bubble.showError(e.message || '生成失败'); App.run('refresh-after-chat'); },
    });
  } catch (e) {
    tw.finish();
    bubble.showError(e.message);
    App.run('refresh-after-chat');
  }
});

App.registerCommand('copy-message', async (msgId) => {
  const el = messagesEl.querySelector(`.msg[data-msg-id="${msgId}"] .msg-content`);
  if (!el) return;
  try {
    await navigator.clipboard.writeText(el.innerText);
    toast('已复制');
  } catch (_) {
    toast('复制失败');
  }
});

App.registerCommand('delete-message', async (msgId) => {
  if (!confirm('删除这条消息？')) return;
  await App.api.del(`/api/v1/messages/${msgId}`);
  messagesEl.querySelector(`.msg[data-msg-id="${msgId}"]`)?.remove();
  toast('已删除');
});

App.registerCommand('clear-session', async () => {
  const sid = App.state.currentSessionId;
  if (!sid) return;
  if (!confirm('清空当前会话的全部消息？')) return;
  await App.api.del(`/api/v1/messages?session_id=${sid}`);
  messagesEl.innerHTML = '<div class="empty-tip">开始新的对话吧 ✦</div>';
});

App.registerCommand('export-session', async () => {
  const sid = App.state.currentSessionId;
  if (!sid) { toast('请先选择会话'); return; }
  const fmt = await promptText('导出格式（md / json / txt）', 'md');
  if (!['md', 'json', 'txt'].includes(fmt)) return;
  const res = await fetch(`${API_BASE}/api/v1/export?session_id=${sid}&format=${fmt}`, { method: 'POST' });
  if (!res.ok) { toast('导出失败'); return; }
  const blob = await res.blob();
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = `chat_${sid}.${fmt}`;
  a.click();
  URL.revokeObjectURL(a.href);
});

App.registerCommand('activate-persona', async (id) => {
  const sid = App.state.currentSessionId;
  const q = sid ? `?session_id=${sid}` : '';
  await App.api.post(`/api/v1/personas/${id}/activate${q}`);
  await refreshPersonas();
  if (sid) loadMessages(sid);
});

App.registerCommand('open-persona-editor', (p) => {
  $('persona-modal-title').textContent = p ? '编辑人设' : '新建人设';
  $('pe-name').value = p?.name || '';
  $('pe-avatar').value = p?.avatar || '🌸';
  $('pe-greeting').value = p?.greeting || '';
  $('pe-prompt').value = p?.system_prompt || '';
  $('pe-temp').value = p?.temperature ?? 0.8;
  $('modal-persona').classList.remove('hidden');
  $('modal-persona').dataset.personaId = p?.id || '';
});

App.registerCommand('save-persona', async () => {
  const name = $('pe-name').value.trim();
  const promptText = $('pe-prompt').value.trim();
  if (!name || !promptText) { toast('名称与 System Prompt 不能为空'); return; }
  const body = {
    name,
    avatar: $('pe-avatar').value.trim() || '🌸',
    greeting: $('pe-greeting').value.trim(),
    system_prompt: promptText,
    temperature: Number($('pe-temp').value) || 0.8,
  };
  const id = $('modal-persona').dataset.personaId;
  if (id) body.id = Number(id);
  await App.api.post('/api/v1/personas', body);
  $('modal-persona').classList.add('hidden');
  await refreshPersonas();
  toast('人设已保存');
});

App.registerCommand('delete-persona', async (id) => {
  if (!confirm('删除这个人设？')) return;
  try {
    await App.api.del(`/api/v1/personas/${id}`);
  } catch (e) {
    toast(e.message);
    return;
  }
  await refreshPersonas();
});

App.registerCommand('open-settings', () => {
  const c = App.state.config || {};
  $('set-base-url').value = c.base_url || '';
  $('set-api-key').value = '';
  $('set-api-key').placeholder = c.api_key ? `当前 Key：${c.api_key}（留空保持不变）` : 'sk-...';
  $('set-model').value = c.model || '';
  $('set-keep-rounds').value = c.keep_rounds ?? 20;
  $('set-theme').value = c.theme || 'dark';
  $('set-proxy-enabled').checked = c.proxy_enabled === '1' || c.proxy_enabled === true;
  $('set-proxy-host').value = c.proxy_host || '';
  $('set-proxy-port').value = c.proxy_port && c.proxy_port !== '0' ? c.proxy_port : '';
  $('modal-settings').classList.remove('hidden');
});

App.registerCommand('save-settings', async () => {
  const body = {
    base_url: $('set-base-url').value.trim(),
    api_key: $('set-api-key').value.trim(),
    model: $('set-model').value.trim(),
    keep_rounds: Number($('set-keep-rounds').value) || 0,
    theme: $('set-theme').value,
    proxy_enabled: $('set-proxy-enabled').checked,
    proxy_host: $('set-proxy-host').value.trim(),
    proxy_port: Number($('set-proxy-port').value) || 0,
  };
  await App.api.post('/api/v1/config', body);
  App.state.config = await App.api.get('/api/v1/config');
  applyTheme(App.state.config.theme);
  $('modal-settings').classList.add('hidden');
  toast('设置已保存');
});

/* ================= 事件绑定 ================= */
document.addEventListener('DOMContentLoaded', () => App.run('boot'));

$('btn-send').addEventListener('click', () => App.run('send-message'));
$('btn-stop').addEventListener('click', () => App.run('stop-stream'));
$('btn-new-session').addEventListener('click', () => App.run('new-session'));
$('btn-settings').addEventListener('click', () => App.run('open-settings'));
$('btn-export').addEventListener('click', () => App.run('export-session'));
$('btn-clear').addEventListener('click', () => App.run('clear-session'));
$('btn-settings-save').addEventListener('click', () => App.run('save-settings'));
$('btn-settings-cancel').addEventListener('click', () => $('modal-settings').classList.add('hidden'));
$('btn-persona-save').addEventListener('click', () => App.run('save-persona'));
$('btn-persona-cancel').addEventListener('click', () => $('modal-persona').classList.add('hidden'));

inputEl.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) {
    e.preventDefault();
    App.run('send-message');
  }
});

messagesEl.addEventListener('click', (e) => {
  const btn = e.target.closest('[data-action]');
  if (!btn) return;
  const msgEl = btn.closest('.msg');
  const msgId = msgEl?.dataset.msgId;
  if (btn.dataset.action === 'copy' && msgId) App.run('copy-message', msgId);
  else if (btn.dataset.action === 'delete' && msgId) App.run('delete-message', msgId);
  else if (btn.dataset.action === 'regenerate') App.run('regenerate');
});

/* WPF 桥接：窗口按钮 + 后端状态 */
const wv = window.chrome && window.chrome.webview;
if (wv) {
  $('btn-min').addEventListener('click', () => wv.postMessage({ type: 'window', payload: { action: 'minimize' } }));
  $('btn-max').addEventListener('click', () => wv.postMessage({ type: 'window', payload: { action: 'maximize' } }));
  $('btn-close').addEventListener('click', () => wv.postMessage({ type: 'window', payload: { action: 'close' } }));
  const tb = $('titlebar');
  tb.addEventListener('mousedown', (e) => {
    if (e.target.closest('.tb-actions')) return;
    if (e.detail === 2) wv.postMessage({ type: 'window', payload: { action: 'maximize' } });
    else wv.postMessage({ type: 'window', payload: { action: 'drag' } });
  });
  wv.addEventListener('message', (e) => {
    try {
      const m = typeof e.data === 'string' ? JSON.parse(e.data) : e.data;
      if (m.type === 'backend-status') App.run('backend-status', m.payload);
    } catch (_) {}
  });
}
