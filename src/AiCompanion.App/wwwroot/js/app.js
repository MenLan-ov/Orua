/* Orua v2 — 前端逻辑（三视图 / 机器人 / 群聊 SSE） */
'use strict';

const API_BASE = (window.__API_BASE__ || 'http://127.0.0.1:8765').replace(/\/$/, '');

function botAvatarHtml(b, cls = 'avatar-img') {
  if (b && b.avatar_url) return `<img class="${cls}" src="${API_BASE}/api/v1/bots/${b.id}/avatar">`;
  return `<span class="${cls}">${escapeHtml(b?.avatar || '🤖')}</span>`;
}

const $ = (id) => document.getElementById(id);
const inputEl = $('input');
const messagesEl = $('messages');

function escapeHtml(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) =>
    ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

let toastTimer = null;
function toast(msg) {
  const el = $('toast');
  el.textContent = msg;
  el.classList.remove('hidden');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.add('hidden'), 2200);
}

function setBanner(msg, ok = false) {
  const el = $('banner');
  if (!msg) { el.classList.add('hidden'); return; }
  el.textContent = msg;
  el.className = 'banner' + (ok ? ' banner-ok' : ' banner-err');
}

function applyTheme(theme) {
  document.documentElement.dataset.theme = theme || 'dark';
}

function renderMarkdown(text) {
  try {
    const html = marked.parse(text, { breaks: true });
    return DOMPurify.sanitize(html);
  } catch (_) {
    return escapeHtml(text);
  }
}

function highlightCode(el) {
  el.querySelectorAll('pre code').forEach((b) => { try { hljs.highlightElement(b); } catch (_) {} });
}

function scrollToBottom() { messagesEl.scrollTop = messagesEl.scrollHeight; }
function scrollGroupToBottom() {
  const el = $('group-messages');
  el.scrollTop = el.scrollHeight;
}

// WebView2 无 window.prompt：自绘弹窗
function promptText(label, def = '') {
  return new Promise((resolve) => {
    const overlay = document.createElement('div');
    overlay.className = 'modal';
    overlay.innerHTML = `
      <div class="modal-box">
        <h3>${escapeHtml(label)}</h3>
        <input id="pt-input" value="${escapeHtml(def)}">
        <div class="modal-actions">
          <button id="pt-cancel" class="btn-ghost">取消</button>
          <button id="pt-ok" class="btn-primary">确定</button>
        </div>
      </div>`;
    document.body.appendChild(overlay);
    const input = overlay.querySelector('#pt-input');
    input.focus(); input.select();
    const close = (v) => { overlay.remove(); resolve(v); };
    overlay.querySelector('#pt-ok').onclick = () => close(input.value.trim());
    overlay.querySelector('#pt-cancel').onclick = () => close(null);
    input.onkeydown = (e) => {
      if (e.key === 'Enter') { e.preventDefault(); close(input.value.trim()); }
      if (e.key === 'Escape') close(null);
    };
  });
}

class Typewriter {
  constructor(el, onDrain) {
    this.el = el;
    this.buf = '';
    this.timer = null;
    this.onDrain = onDrain;
  }
  push(text) { this.buf += text; this.flush(); }
  flush() {
    if (this.timer || !this.buf) return;
    this.timer = setInterval(() => {
      if (!this.buf) {
        clearInterval(this.timer);
        this.timer = null;
        if (this.onDrain) this.onDrain();
        return;
      }
      this.el.textContent += this.buf.slice(0, 4);
      this.buf = this.buf.slice(4);
    }, 24);
  }
  finish() {
    if (this.buf) this.el.textContent += this.buf;
    this.buf = '';
    if (this.timer) { clearInterval(this.timer); this.timer = null; }
    if (this.onDrain) this.onDrain();
  }
}

async function consumeSse(url, body, handlers) {
  const ctrl = new AbortController();
  App.state.abortCtrl = ctrl;
  try {
    const res = await fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
      signal: ctrl.signal,
    });
    if (!res.ok || !res.body) {
      const err = await res.json().catch(() => ({}));
      throw new Error(err.message || `HTTP ${res.status}`);
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
          if (line.startsWith('event: ')) ev = line.slice(7).trim();
          else if (line.startsWith('data: ')) data += line.slice(6);
        }
        if (!data) continue;
        let obj;
        try { obj = JSON.parse(data); } catch (_) { continue; }
        if (ev === 'delta') handlers.onDelta && handlers.onDelta(obj.content || '');
        else if (ev === 'done') handlers.onDone && handlers.onDone(obj);
        else if (ev === 'error') {
          const e = new Error(obj.message || '生成失败');
          e.code = obj.code;
          handlers.onError && handlers.onError(e);
        }
      }
    }
  } catch (e) {
    if (e.name === 'AbortError') return;
    handlers.onError && handlers.onError(e);
  }
}

/* ================= App 核心 ================= */
const App = {
  state: {
    config: null,
    pinned: [],
    reads: {},
    sessions: [],
    bots: [],
    groups: [],
    defaultBotId: 0,
    selectedBotId: null,
    currentSessionId: null,
    currentGroupId: null,
    currentGroup: null,
    view: 'messages',
    streaming: false,
    abortCtrl: null,
    search: '',
    groupAbort: null,
    groupReconnectTimer: null,
    groupBackoff: 1000,
    groupStatus: 'idle',
    groupTurn: 0,
    typers: new Map(),
    gmsgSeen: new Set(),
    botAvatarFile: null,
    botAvatarRemove: false,
    memoryBotId: null,
    memories: [],
    skills: [],
    botSkills: {},
    pluginBotId: null,
    detached: false,
    detachedTarget: null,
  },
  commands: new Map(),
  registerCommand(name, fn) { this.commands.set(name, fn); },
  run(cmd, ...args) {
    const fn = this.commands.get(cmd);
    if (!fn) return Promise.reject(new Error('unknown command: ' + cmd));
    return Promise.resolve(fn(...args)).catch((e) => {
      console.error(cmd, e);
      toast(e.message || '操作失败');
    });
  },
  api: {
    async req(method, path, body) {
      const opts = { method, headers: { 'Content-Type': 'application/json' } };
      if (body !== undefined) opts.body = JSON.stringify(body);
      const res = await fetch(API_BASE + path, opts);
      let json = null;
      try { json = await res.json(); } catch (_) {}
      if (!res.ok || (json && json.code !== 0)) {
        const e = new Error((json && json.message) || `HTTP ${res.status}`);
        e.code = json ? json.code : res.status;
        throw e;
      }
      return json ? json.data : null;
    },
    get: (p) => App.api.req('GET', p),
    post: (p, b) => App.api.req('POST', p, b),
    patch: (p, b) => App.api.req('PATCH', p, b),
    put: (p, b) => App.api.req('PUT', p, b),
    del: (p) => App.api.req('DELETE', p),
  },
};

/* ================= 视图切换 ================= */
async function switchView(view) {
  if (App.state.detached && view === 'plugins') return;
  App.state.view = view;
  // 群聊室作为「聊天」视图内的子视图：当前群打开时以 view-groups 覆盖内容区
  const inGroupRoom = view === 'messages' && App.state.currentGroupId !== null;
  $('view-messages').classList.toggle('hidden', view !== 'messages' || inGroupRoom);
  $('view-bots').classList.toggle('hidden', view !== 'bots');
  $('view-groups').classList.toggle('hidden', view !== 'groups' && !inGroupRoom);
  $('view-memory').classList.toggle('hidden', view !== 'memory');
  $('view-plugins').classList.toggle('hidden', view !== 'plugins');
  $('btn-new-session').classList.toggle('hidden', view !== 'messages');
  $('btn-new-bot').classList.toggle('hidden', view !== 'bots');
  $('btn-new-group').classList.toggle('hidden', view !== 'messages');
  $('session-list').classList.toggle('hidden', view !== 'messages');
  $('bot-list').classList.toggle('hidden', view !== 'bots');
  $('group-list').classList.toggle('hidden', view !== 'groups');
  document.querySelectorAll('.tab-btn').forEach((b) =>
    b.classList.toggle('active', b.dataset.view === view));
  if (view === 'groups') {
    if (App.state.currentGroupId) App.run('open-group-room', App.state.currentGroupId);
  } else if (!inGroupRoom) {
    App.run('close-group-room');
  }
  if (view === 'messages') renderChatList();
  if (view === 'memory') { renderMemoryBots(); loadMemories(); }
  if (view === 'plugins') {
    if (!App.state.pluginBotId && App.state.bots.length) App.state.pluginBotId = App.state.bots[0].id;
    try {
      await loadSkills();
      if (App.state.pluginBotId) await loadBotSkills(App.state.pluginBotId);
    } catch (e) {
      toast(e.message || '技能加载失败');
    }
    renderBotSkills();
  }
}

/* ================= 单聊气泡 ================= */
function metaBar(role, msgId) {
  let btns = '<button data-action="copy" title="复制">📋</button>';
  if (role === 'assistant') btns += '<button data-action="regenerate" title="重新生成">🔄</button>';
  if (msgId) btns += '<button data-action="delete" title="删除">🗑</button>';
  return `<div class="msg-meta">${btns}</div>`;
}

function botNameForSession() {
  const s = App.state.sessions.find((x) => x.id === App.state.currentSessionId);
  if (s && s.bot_id > 0) {
    const b = App.state.bots.find((x) => x.id === s.bot_id);
    if (b) return b.name;
  }
  return 'AI';
}

function addBubble(role, text, opts = {}) {
  const bubble = document.createElement('div');
  bubble.className = 'msg ' + (role === 'user' ? 'msg-user' : 'msg-ai');
  if (opts.id) bubble.dataset.msgId = opts.id;
  const who = role === 'user' ? '我' : botNameForSession();
  bubble.innerHTML = `
    <div class="msg-who">${escapeHtml(who)}</div>
    <div class="msg-content">${opts.streaming ? '' : renderMarkdown(text)}</div>
    ${opts.streaming ? '' : metaBar(role, opts.id)}`;
  messagesEl.appendChild(bubble);
  const contentEl = bubble.querySelector('.msg-content');
  if (!opts.streaming) highlightCode(contentEl);
  return {
    el: bubble,
    contentEl,
    finalize(full) {
      contentEl.innerHTML = renderMarkdown(full);
      highlightCode(contentEl);
      if (!bubble.querySelector('.msg-meta')) bubble.insertAdjacentHTML('beforeend', metaBar(role));
      scrollToBottom();
    },
    showError(msg) {
      contentEl.innerHTML = `<span class="msg-error">⚠ ${escapeHtml(msg)}</span>`;
    },
  };
}

function setStreamingUI(streaming) {
  App.state.streaming = streaming;
  $('btn-send').classList.toggle('hidden', streaming);
  $('btn-stop').classList.toggle('hidden', !streaming);
  $('stream-hint').classList.toggle('hidden', !streaming);
}

/* ================= 会话 ================= */
async function refreshSessions() {
  App.state.sessions = await App.api.get('/api/v1/sessions');
}

/* ================= 聊天列表（Bot/群合并，QQ 式） ================= */
function isPinned(kind, id) {
  return App.state.pinned.some((p) => p.kind === kind && p.id === id);
}
function readWatermark(kind, id) {
  return App.state.reads[`${kind}:${id}`] || '';
}
async function defaultSessionFor(bot) {
  const mine = App.state.sessions.filter((s) => s.bot_id === bot.id);
  mine.sort((a, b) => (b.updated_at || '').localeCompare(a.updated_at || ''));
  if (mine.length) return mine[0];
  const s = await App.api.post('/api/v1/sessions', { bot_id: bot.id });
  await refreshSessions();
  return App.state.sessions.find((x) => x.id === s.id);
}
async function chatListPreview(kind, id) {
  try {
    if (kind === 'bot') {
      const bot = App.state.bots.find((b) => b.id === id);
      if (!bot) return { text: '', time: '' };
      const mine = App.state.sessions.filter((s) => s.bot_id === bot.id);
      mine.sort((a, b) => (b.updated_at || '').localeCompare(a.updated_at || ''));
      if (!mine.length) return { text: bot.greeting || '', time: '' };
      const msgs = await App.api.get(`/api/v1/messages?session_id=${mine[0].id}&limit=1`);
      const last = msgs[0];
      return { text: last ? (last.role === 'user' ? last.content : last.content) : '', time: last ? last.created_at : '' };
    }
    const g = await App.api.get(`/api/v1/groups/${id}`);
    const msgs = await App.api.get(`/api/v1/groups/${id}/messages?limit=1`);
    const last = msgs[0];
    return { text: last ? (last.bot_id === 0 ? last.content : `${last.name || 'Bot'}：${last.content}`) : '', time: last ? last.created_at : '' };
  } catch (_) {
    return { text: '', time: '' };
  }
}
function fmtListTime(iso) {
  if (!iso) return '';
  const d = new Date(iso);
  if (isNaN(d)) return '';
  const now = new Date();
  if (d.toDateString() === now.toDateString())
    return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
  return `${d.getMonth() + 1}/${d.getDate()}`;
}
async function renderChatList() {
  const el = $('session-list');
  const q = App.state.search.trim().toLowerCase();
  const items = [];
  for (const b of App.state.bots) {
    const pv = await chatListPreview('bot', b.id);
    const wm = readWatermark('bot', b.id);
    const unread = wm && pv.time && pv.time > wm;
    items.push({ kind: 'bot', id: b.id, name: b.name, avatar: b.avatar, avatar_url: b.avatar_url,
                 preview: pv.text, time: pv.time, unread, bot: b });
  }
  for (const g of App.state.groups) {
    const pv = await chatListPreview('group', g.id);
    const wm = readWatermark('group', g.id);
    const unread = wm && pv.time && pv.time > wm;
    items.push({ kind: 'group', id: g.id, name: g.name, avatar: '👥', preview: pv.text,
                 time: pv.time, unread, group: g });
  }
  const filtered = items.filter((it) => !q || it.name.toLowerCase().includes(q));
  filtered.sort((a, b) => {
    const pa = isPinned(a.kind, a.id) ? 1 : 0;
    const pb = isPinned(b.kind, b.id) ? 1 : 0;
    if (pa !== pb) return pb - pa;
    return (b.time || '').localeCompare(a.time || '');
  });
  el.innerHTML = '';
  for (const it of filtered) {
    const item = document.createElement('div');
    item.className = 'list-item' + (it.kind === 'bot' && it.id === App.state.selectedBotId ? '' : '');
    const av = it.kind === 'bot' ? botAvatarHtml(it.bot) : `<span class="list-item-avatar">👥</span>`;
    item.innerHTML = `
      <div class="list-item-avatar-wrap">${av}</div>
      <div class="list-item-body">
        <div class="list-item-title-row">
          ${isPinned(it.kind, it.id) ? '<span class="pin-icon">📌</span>' : ''}
          <span>${escapeHtml(it.name)}</span>
          ${it.kind === 'group' && it.group && it.group.status === 'running' ? '<span class="badge-running">进行中</span>' : ''}
        </div>
        <div class="list-item-preview">${escapeHtml(it.preview)}</div>
      </div>
      <div class="list-item-side">
        <span class="list-item-time">${fmtListTime(it.time)}</span>
        ${it.unread ? '<span class="unread-dot"></span>' : ''}
      </div>`;
    item.onclick = async () => {
      if (it.kind === 'bot') {
        const s = await defaultSessionFor(it.bot);
        if (s) App.run('select-session', s.id);
        markRead('bot', it.id);
      } else {
        App.run('open-group-room', it.id);
        markRead('group', it.id);
      }
      switchView('messages');
    };
    item.oncontextmenu = (e) => {
      e.preventDefault();
      App.run('open-context-menu', it.kind, it.id, e.clientX, e.clientY);
    };
    el.appendChild(item);
  }
}
async function markRead(kind, id) {
  App.state.reads[`${kind}:${id}`] = new Date().toISOString();
  await saveReads();
}
async function saveReads() {
  await App.api.post('/api/v1/config', { chat_reads: JSON.stringify(App.state.reads) });
}
async function savePinned() {
  await App.api.post('/api/v1/config', { chat_pinned: JSON.stringify(App.state.pinned) });
}

function updateChatSub(sid) {
  const s = App.state.sessions.find((x) => x.id === sid);
  if (!s) { $('chat-sub').textContent = ''; return; }
  if (s.bot_id > 0) {
    const b = App.state.bots.find((x) => x.id === s.bot_id);
    $('chat-sub').textContent = b ? `与 ${b.name} 对话` : '与默认 Bot 对话';
  } else {
    $('chat-sub').textContent = '与默认 Bot 对话';
  }
}

async function loadMessages(sid) {
  const msgs = await App.api.get(`/api/v1/messages?session_id=${sid}&limit=100`);
  messagesEl.innerHTML = '';
  for (const m of [...msgs].reverse()) {
    if (m.role === 'user') addBubble('user', m.content, { id: m.id });
    else if (m.role === 'assistant') addBubble('assistant', m.content, { id: m.id });
    else {
      const note = document.createElement('div');
      note.className = 'sysnote';
      note.textContent = m.content;
      messagesEl.appendChild(note);
    }
  }
  scrollToBottom();
}

App.registerCommand('select-session', async (id) => {
  App.run('close-group-room');
  App.state.currentSessionId = id;
  const s = App.state.sessions.find((x) => x.id === id);
  $('chat-title').textContent = s ? s.title : '新对话';
  updateChatSub(id);
  await loadMessages(id);
  $('chat-empty').classList.add('hidden');
  $('messages').classList.remove('hidden');
  $('composer').classList.remove('hidden');
  $('btn-switch-bot').classList.remove('hidden');
});

App.registerCommand('rename-session', async (id) => {
  const s = App.state.sessions.find((x) => x.id === id);
  if (!s) return;
  const title = await promptText('重命名会话', s.title);
  if (!title) return;
  await App.api.patch(`/api/v1/sessions/${id}`, { title });
  await refreshSessions();
  if (App.state.currentSessionId === id) $('chat-title').textContent = title;
});

App.registerCommand('delete-session', async (id) => {
  if (!confirm('删除这个会话及其全部消息？')) return;
  await App.api.del(`/api/v1/sessions/${id}`);
  if (App.state.currentSessionId === id) {
    App.state.currentSessionId = null;
    messagesEl.innerHTML = '';
    $('chat-title').textContent = '新对话';
    $('chat-sub').textContent = '';
    $('messages').classList.add('hidden');
    $('composer').classList.add('hidden');
    $('chat-empty').classList.remove('hidden');
  }
  await refreshSessions();
  renderChatList();
});

App.registerCommand('new-session', () => {
  const box = $('ns-bots');
  box.innerHTML = '';
  const enabled = App.state.bots.filter((b) => b.is_enabled);
  if (!enabled.length) { toast('没有可用的 Bot，请先在「Bot」页创建或启用'); return; }
  for (const b of enabled) {
    const label = document.createElement('label');
    label.className = 'ns-bot' + (b.id === App.state.defaultBotId ? ' checked' : '');
    label.innerHTML = `<input type="radio" name="ns-bot" value="${b.id}" ${b.id === App.state.defaultBotId ? 'checked' : ''}>
      <span class="member-avatar">${botAvatarHtml(b)}</span>
      <span class="member-name">${escapeHtml(b.name)}</span>`;
    box.appendChild(label);
  }
  $('modal-new-session').dataset.mode = 'create';
  $('modal-new-session').classList.remove('hidden');
});

App.registerCommand('switch-session-bot', () => {
  if (!App.state.currentSessionId) return;
  App.run('new-session');
  $('modal-new-session').dataset.mode = 'switch';
});

App.registerCommand('save-new-session', async () => {
  const checked = $('ns-bots').querySelector('input:checked');
  if (!checked) { toast('请选择一个 Bot'); return; }
  const botId = Number(checked.value);
  const mode = $('modal-new-session').dataset.mode;
  $('modal-new-session').classList.add('hidden');
  if (mode === 'switch') {
    const sid = App.state.currentSessionId;
    if (!sid) return;
    await App.api.patch(`/api/v1/sessions/${sid}`, { bot_id: botId });
    updateChatSub(sid);
    toast('已切换 Bot');
  } else {
    const s = await App.api.post('/api/v1/sessions', { bot_id: botId });
    await refreshSessions();
    renderChatList();
    App.run('select-session', s.id);
  }
});

/* ================= 单聊交互 ================= */
App.registerCommand('send-message', async () => {
  const sid = App.state.currentSessionId;
  const content = inputEl.value.trim();
  if (!sid || !content || App.state.streaming) return;
  inputEl.value = '';
  setStreamingUI(true);
  addBubble('user', content);
  scrollToBottom();
  const bubble = addBubble('assistant', '', { streaming: true });
  let full = '';
  const tw = new Typewriter(bubble.contentEl, () => { bubble.finalize(full); });
  try {
    await consumeSse('/api/v1/chat/stream', { session_id: sid, content }, {
      onDelta: (d) => { full += d; tw.push(d); scrollToBottom(); },
      onDone: () => { tw.finish(); App.run('refresh-after-chat'); },
      onError: (e) => {
        tw.finish();
        bubble.showError(e.message || '生成失败');
        App.run('refresh-after-chat');
      },
    });
  } catch (e) {
    tw.finish();
    bubble.showError(e.message);
    App.run('refresh-after-chat');
  }
});

App.registerCommand('refresh-after-chat', async () => {
  setStreamingUI(false);
  await refreshSessions(true);
  if (App.state.currentSessionId) loadMessages(App.state.currentSessionId);
  renderChatList();
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
  const tw = new Typewriter(bubble.contentEl, () => { bubble.finalize(full); });
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

App.registerCommand('copy-message', async (msgEl) => {
  const content = msgEl.querySelector('.msg-content');
  if (!content) return;
  try {
    await navigator.clipboard.writeText(content.innerText);
    toast('已复制');
  } catch (_) { toast('复制失败'); }
});

App.registerCommand('delete-message', async (msgId) => {
  if (!confirm('删除这条消息？')) return;
  await App.api.del(`/api/v1/messages/${msgId}`);
  messagesEl.querySelector(`.msg[data-msg-id="${msgId}"]`)?.remove();
  toast('已删除');
});

App.registerCommand('clear-session', async (sid) => {
  sid = sid || App.state.currentSessionId;
  if (!sid) return;
  if (!confirm('清空该会话的全部消息？')) return;
  await App.api.del(`/api/v1/messages?session_id=${sid}`);
  if (sid !== App.state.currentSessionId) { toast('已清空该会话消息'); return; }
  messagesEl.innerHTML = '<div class="sysnote">开始新的对话吧 ✦</div>';
});

App.registerCommand('export-session', async (sid) => {
  sid = sid || App.state.currentSessionId;
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

/* ================= 机器人 ================= */
async function refreshBots() {
  const data = await App.api.get('/api/v1/bots');
  App.state.bots = data.bots;
  App.state.defaultBotId = data.default_bot_id;
  renderBots();
}

function renderBots() {
  const el = $('bot-list');
  const q = App.state.search.trim().toLowerCase();
  const list = App.state.bots.filter((b) => !q || b.name.toLowerCase().includes(q));
  el.innerHTML = '';
  $('bots-empty').classList.toggle('hidden', list.length > 0);
  for (const b of list) {
    const item = document.createElement('div');
    item.className = 'list-item' + (b.id === App.state.selectedBotId ? ' active' : '');
    item.innerHTML = `
      <div class="list-item-avatar">${botAvatarHtml(b)}</div>
      <div class="list-item-body">
        <div class="list-item-title">${escapeHtml(b.name)}${b.id === App.state.defaultBotId ? '<span class="tag">默认</span>' : ''}</div>
        <div class="list-item-sub">${b.is_enabled ? '' : '已停用 · '}${escapeHtml(b.api_base_url || '默认 API')}</div>
      </div>
      <label class="switch" title="启用/停用">
        <input type="checkbox" data-bot-toggle="${b.id}" ${b.is_enabled ? 'checked' : ''}>
        <span class="slider"></span>
      </label>`;
    item.onclick = (e) => {
      if (e.target.closest('.switch')) return;
      App.run('select-bot', b.id);
    };
    el.appendChild(item);
  }
  renderBotDetail();
}

App.registerCommand('select-bot', (id) => {
  App.state.selectedBotId = id;
  renderBots();
});

function renderBotDetail() {
  const b = App.state.bots.find((x) => x.id === App.state.selectedBotId);
  const detail = $('bot-detail');
  if (!b) { detail.classList.add('hidden'); return; }
  detail.classList.remove('hidden');
  $('btn-bot-delete').classList.toggle('hidden', b.id === App.state.defaultBotId);
  $('bot-detail-avatar').innerHTML = botAvatarHtml(b, 'bot-detail-avatar');
  $('bot-detail-name').textContent = b.name + (b.id === App.state.defaultBotId ? '（默认）' : '');
  $('bot-detail-sub').textContent = b.is_enabled ? '已启用' : '已停用';
  $('bot-detail-prompt').textContent = b.system_prompt;
  $('bot-detail-meta').innerHTML =
    `Temperature：${b.temperature}<br>` +
    `API：${escapeHtml(b.api_base_url || '继承默认配置')} · 模型：${escapeHtml(b.api_model || '继承默认配置')}<br>` +
    `Key：${b.api_key ? escapeHtml(b.api_key) : '继承默认配置'}`;
  $('bot-detail-greeting').textContent = b.greeting || '（无）';
}

// PUT /bots/{id}/skills 是整表替换：勾选未与服务端现状对齐时保存不得提交，否则会清空既有技能
let botSkillsAligned = false;

App.registerCommand('open-bot-editor', async (b) => {
  $('bot-modal-title').textContent = b ? '编辑 Bot' : '新建 Bot';
  $('be-name').value = b?.name || '';
  $('be-avatar').value = b?.avatar || '🤖';
  $('be-greeting').value = b?.greeting || '';
  $('be-prompt').value = b?.system_prompt || '';
  $('be-temp').value = b?.temperature ?? 0.8;
  $('be-max-tokens').value = b?.max_tokens ?? 2048;
  $('be-api-type').value = b?.api_type || 'openai';
  $('be-avatar-file').value = '';
  App.state.botAvatarFile = null;
  App.state.botAvatarRemove = false;
  const prev = $('be-avatar-preview');
  prev.classList.toggle('hidden', !(b?.avatar_url));
  if (b?.avatar_url) $('be-avatar-img').src = `${API_BASE}/api/v1/bots/${b.id}/avatar`;
  $('be-api-base').value = b?.api_base_url || '';
  $('be-api-key').value = '';
  $('be-api-key').placeholder = b?.api_key ? `当前 Key：${b.api_key}（留空保持不变）` : '留空 = 使用默认 Key';
  $('be-api-model').value = b?.api_model || '';
  $('be-key-clear').checked = false;
  $('modal-bot').dataset.botId = b?.id || '';
  $('be-skills').innerHTML = '';
  botSkillsAligned = false;
  $('modal-bot').classList.remove('hidden');
  try {
    if (!App.state.skills.length) await loadSkills();
    if (b) await loadBotSkills(b.id);
    renderBotEditorSkills(b);
  } catch (e) {
    toast(e.message || '技能加载失败');
  }
});

function renderBotEditorSkills(b) {
  const skEl = $('be-skills');
  skEl.innerHTML = '';
  const saved = App.state.botSkills[b?.id] || [];
  for (const s of App.state.skills) {
    const on = saved.some((x) => x.skill_id === s.id && x.enabled);
    const lab = document.createElement('label');
    lab.className = 'member-check';
    lab.innerHTML = `<input type="checkbox" data-beskill="${escapeHtml(s.id)}" ${on ? 'checked' : ''}>
      <span>${escapeHtml(s.name)}</span>`;
    skEl.appendChild(lab);
  }
  botSkillsAligned = true;
}

App.registerCommand('save-bot', async () => {
  const name = $('be-name').value.trim();
  const prompt = $('be-prompt').value.trim();
  if (!name || !prompt) { toast('名称与 System Prompt 不能为空'); return; }
  const body = {
    name,
    avatar: $('be-avatar').value.trim() || '🤖',
    greeting: $('be-greeting').value.trim(),
    system_prompt: prompt,
    temperature: Number($('be-temp').value) || 0.8,
    max_tokens: Number($('be-max-tokens').value) || 2048,
    api_type: $('be-api-type').value,
    api_base_url: $('be-api-base').value.trim(),
    api_model: $('be-api-model').value.trim(),
  };
  if ($('be-key-clear').checked) body.api_key = '';
  else if ($('be-api-key').value.trim()) body.api_key = $('be-api-key').value.trim();
  const id = $('modal-bot').dataset.botId;
  let saved;
  if (id) saved = await App.api.patch(`/api/v1/bots/${id}`, body);
  else saved = await App.api.post('/api/v1/bots', body);
  const savedId = saved.id;
  if (savedId && botSkillsAligned) {
    if (!id) botSkillsLoaded.add(savedId); // 新建 Bot 的服务端技能集必为空，可直接提交
    await submitBotSkills(savedId, () => [...$('be-skills').querySelectorAll('input:checked')]
      .map((c) => ({ skill_id: c.dataset.beskill, enabled: true })));
  }
  if (App.state.botAvatarRemove && savedId) {
    // 移除图片：PATCH 清空 avatar_path（Task 5 已支持直通），回到 emoji 头像
    await App.api.patch(`/api/v1/bots/${savedId}`, { avatar_path: '' });
  } else if (App.state.botAvatarFile && savedId) {
    await App.api.post(`/api/v1/bots/${savedId}/avatar`, { image_base64: App.state.botAvatarFile });
  }
  $('modal-bot').classList.add('hidden');
  await refreshBots();
  renderChatList();
  toast('Bot 已保存');
});

App.registerCommand('delete-bot', async (id) => {
  const b = App.state.bots.find((x) => x.id === id);
  if (!confirm(`删除 Bot「${b?.name || ''}」？`)) return;
  try {
    await App.api.del(`/api/v1/bots/${id}`);
  } catch (e) {
    toast(e.code === 1001 ? '默认 Bot 不可删除' : e.message);
    return;
  }
  await refreshBots();
  renderChatList();
});

/* ================= 群聊 ================= */
async function refreshGroups() {
  App.state.groups = await App.api.get('/api/v1/groups');
  renderGroups();
}

function renderGroups() {
  const el = $('group-list');
  const q = App.state.search.trim().toLowerCase();
  const list = App.state.groups.filter((g) => !q || g.name.toLowerCase().includes(q));
  el.innerHTML = '';
  $('group-empty').classList.toggle('hidden', list.length > 0 || !!App.state.currentGroupId);
  for (const g of list) {
    const item = document.createElement('div');
    item.className = 'list-item' + (g.id === App.state.currentGroupId ? ' active' : '');
    const statusLabel = { running: '进行中', paused: '已暂停' }[g.status];
    item.innerHTML = `<div class="list-item-body">
        <div class="list-item-title">${escapeHtml(g.name)}</div>
        <div class="list-item-sub">${g.member_count} 位成员${statusLabel ? ' · ' + statusLabel : ''}</div>
      </div>`;
    item.onclick = () => App.run('select-group', g.id);
    const del = document.createElement('button');
    del.className = 'list-item-btn';
    del.textContent = '🗑';
    del.title = '删除群';
    del.onclick = (e) => { e.stopPropagation(); App.run('delete-group', g.id); };
    item.appendChild(del);
    el.appendChild(item);
  }
}

App.registerCommand('select-group', (id) => {
  App.run('open-group-room', id);
});

function addGroupBubble(botId, name, avatar, text, opts = {}) {
  const el = $('group-messages');
  const bubble = document.createElement('div');
  bubble.className = 'gmsg' + (botId === 0 ? ' gmsg-user' : '');
  if (opts.id) bubble.dataset.gmsgId = opts.id;
  const av = opts.avatarUrl
    ? `<img class="avatar-img" src="${API_BASE}/api/v1/bots/${botId}/avatar">`
    : `<span class="gmsg-avatar">${escapeHtml(avatar)}</span>`;
  bubble.innerHTML = `
    <div class="gmsg-head">${av}
      <span class="gmsg-name">${escapeHtml(name)}</span>
      ${opts.id && !opts.streaming ? '<div class="gmsg-meta"><button data-action="copy" title="复制">📋</button></div>' : ''}
    </div>
    <div class="gmsg-content">${opts.streaming ? '' : renderMarkdown(text)}</div>`;
  el.appendChild(bubble);
  const contentEl = bubble.querySelector('.gmsg-content');
  if (!opts.streaming) highlightCode(contentEl);
  return {
    el: bubble,
    contentEl,
    finalize(full) {
      contentEl.innerHTML = renderMarkdown(full);
      highlightCode(contentEl);
      scrollGroupToBottom();
    },
    showError(msg) {
      contentEl.innerHTML = `<span class="msg-error">⚠ ${escapeHtml(msg)}</span>`;
    },
  };
}

// 按 gmsg id 去重（SSE 与 REST 重建可能重复投递同一条）
function addGroupBubbleChecked(botId, name, avatar, text, opts = {}) {
  if (opts.id && App.state.gmsgSeen.has(opts.id)) return null;
  if (opts.id) App.state.gmsgSeen.add(opts.id);
  return addGroupBubble(botId, name, avatar, text, { ...opts, avatarUrl: opts.avatarUrl });
}

function addGroupNote(text) {
  const note = document.createElement('div');
  note.className = 'sysnote';
  note.textContent = text;
  $('group-messages').appendChild(note);
  scrollGroupToBottom();
}

App.registerCommand('open-group-room', async (gid) => {
  closeGroupStream();
  App.state.typers.clear();
  App.state.gmsgSeen.clear();
  App.state.currentGroupId = gid;
  App.state.groupStatus = 'idle';
  App.state.groupTurn = 0;
  App.state.groupBackoff = 1000;
  let g;
  try {
    g = await App.api.get(`/api/v1/groups/${gid}`);
  } catch (e) {
    App.state.currentGroupId = null;
    toast(e.message);
    return;
  }
  App.state.currentGroup = g;
  $('group-empty').classList.add('hidden');
  $('group-room').classList.remove('hidden');
  $('group-title').textContent = g.name;
  $('group-sub').textContent = g.topic ? `话题：${g.topic}` : '';
  $('group-topic').value = '';
  App.state.groupStatus = g.status;
  App.state.groupTurn = 0;
  updateGroupControls();
  await reconcileGroupMessages();
  openGroupStream(gid);
});

App.registerCommand('close-group-room', () => {
  if (App.state.currentGroupId === null) return;
  closeGroupStream();
  App.state.typers.clear();
  App.state.currentGroupId = null;
  App.state.currentGroup = null;
  $('group-room').classList.add('hidden');
  $('group-empty').classList.remove('hidden');
  renderGroups();
});

function closeGroupStream() {
  try { App.state.groupAbort?.abort(); } catch (_) {}
  App.state.groupAbort = null;
  clearTimeout(App.state.groupReconnectTimer);
  App.state.groupReconnectTimer = null;
}

// 以 REST 为准全量重建消息区（turn_end 时引擎已落库，此时无在途打字机）
async function reconcileGroupMessages() {
  const gid = App.state.currentGroupId;
  if (!gid) return;
  try {
    const msgs = await App.api.get(`/api/v1/groups/${gid}/messages?limit=200`);
    const el = $('group-messages');
    el.innerHTML = '';
    App.state.gmsgSeen.clear();
    for (const m of [...msgs].reverse()) {
      addGroupBubbleChecked(m.bot_id, m.name, m.avatar, m.content, { id: m.id, avatarUrl: m.avatar_url });
    }
    scrollGroupToBottom();
  } catch (_) {}
}

function updateGroupControls() {
  const st = App.state.groupStatus;
  $('btn-group-start').classList.toggle('hidden', st !== 'idle');
  $('btn-group-pause').classList.toggle('hidden', st !== 'running');
  $('btn-group-resume').classList.toggle('hidden', st !== 'paused');
  $('btn-group-stop').classList.toggle('hidden', st === 'idle');
  $('group-turn').classList.toggle('hidden', st === 'idle');
  if (st !== 'idle') $('group-turn').textContent = `第 ${App.state.groupTurn} 轮`;
}

function ensureTyper(obj) {
  const key = obj.bot_id;
  let t = App.state.typers.get(key);
  if (!t) {
    const b = addGroupBubble(key, obj.name || 'Bot', obj.avatar || '🤖', '', { streaming: true, avatarUrl: obj.avatar_url });
    const tw = new Typewriter(b.contentEl, () => {});
    t = { bubble: b, tw, full: '' };
    App.state.typers.set(key, t);
  }
  return t;
}

function finishTyper(botId) {
  const t = App.state.typers.get(botId);
  if (!t) return;
  App.state.typers.delete(botId);
  t.tw.finish();
  t.bubble.finalize(t.full);
  reconcileGroupMessages();
}

function handleGroupEvent(ev, obj) {
  if (ev === 'status') {
    const prev = App.state.groupStatus;
    App.state.groupStatus = obj.state;
    App.state.groupTurn = obj.turn || App.state.groupTurn;
    updateGroupControls();
    if (obj.state === 'idle' && prev !== 'idle') addGroupNote('群聊已停止');
  } else if (ev === 'turn_start') {
    App.state.groupTurn = obj.turn || App.state.groupTurn;
    updateGroupControls();
    ensureTyper(obj);
  } else if (ev === 'delta') {
    const t = ensureTyper(obj);
    t.full += obj.text || '';
    t.tw.push(obj.text || '');
    scrollGroupToBottom();
  } else if (ev === 'turn_end') {
    finishTyper(obj.bot_id);
  } else if (ev === 'interject') {
    addGroupBubbleChecked(0, '用户', '🙋', obj.content, { id: obj.id });
    scrollGroupToBottom();
  } else if (ev === 'error') {
    finishTyper(obj.bot_id);
    addGroupNote(`⚠ ${obj.name || 'Bot'} 回复失败：${obj.message || ''}`);
  }
}

async function openGroupStream(gid) {
  closeGroupStream();
  const ctrl = new AbortController();
  App.state.groupAbort = ctrl;
  try {
    const res = await fetch(`${API_BASE}/api/v1/groups/${gid}/stream`, { signal: ctrl.signal });
    if (!res.ok || !res.body) {
      const err = await res.json().catch(() => ({}));
      throw new Error(err.message || `HTTP ${res.status}`);
    }
    App.state.groupBackoff = 1000;
    setBanner('');
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
          if (line.startsWith('event: ')) ev = line.slice(7).trim();
          else if (line.startsWith('data: ')) data += line.slice(6);
        }
        if (!data) continue;
        let obj;
        try { obj = JSON.parse(data); } catch (_) { continue; }
        handleGroupEvent(ev, obj);
      }
    }
  } catch (e) {
    if (e.name === 'AbortError') return;
  }
  scheduleGroupReconnect(gid);
}

// 断开退避重连：1s/2s/4s（封顶）
function scheduleGroupReconnect(gid) {
  if (App.state.currentGroupId !== gid) return;
  const delay = App.state.groupBackoff;
  App.state.groupBackoff = Math.min(App.state.groupBackoff * 2, 4000);
  setBanner(`群聊连接断开，${Math.round(delay / 1000)}s 后重连…`);
  clearTimeout(App.state.groupReconnectTimer);
  App.state.groupReconnectTimer = setTimeout(() => {
    if (App.state.currentGroupId === gid) openGroupStream(gid);
  }, delay);
}

App.registerCommand('group-start', async () => {
  const gid = App.state.currentGroupId;
  if (!gid) return;
  const topic = $('group-topic').value.trim();
  try {
    const r = await App.api.post(`/api/v1/groups/${gid}/start`, topic ? { topic } : {});
    App.state.groupStatus = r.state;
    updateGroupControls();
    if (topic) {
      App.state.currentGroup.topic = topic;
      $('group-sub').textContent = `话题：${topic}`;
      $('group-topic').value = '';
    }
  } catch (e) { toast(e.message); }
});

App.registerCommand('group-pause', async () => {
  const gid = App.state.currentGroupId;
  if (!gid) return;
  try {
    const r = await App.api.post(`/api/v1/groups/${gid}/pause`, {});
    App.state.groupStatus = r.state;
    updateGroupControls();
  } catch (e) { toast(e.message); }
});

App.registerCommand('group-resume', async () => {
  const gid = App.state.currentGroupId;
  if (!gid) return;
  try {
    const r = await App.api.post(`/api/v1/groups/${gid}/resume`, {});
    App.state.groupStatus = r.state;
    updateGroupControls();
  } catch (e) { toast(e.message); }
});

App.registerCommand('group-stop', async () => {
  const gid = App.state.currentGroupId;
  if (!gid) return;
  try {
    const r = await App.api.post(`/api/v1/groups/${gid}/stop`, {});
    App.state.groupStatus = r.state;
    updateGroupControls();
  } catch (e) { toast(e.message); }
});

App.registerCommand('group-interject', async () => {
  const gid = App.state.currentGroupId;
  const input = $('group-interject-input');
  const content = input.value.trim();
  if (!gid || !content) return;
  try {
    await App.api.post(`/api/v1/groups/${gid}/interject`, { content });
    input.value = '';
    // 有在途打字机（活跃轮次）时跳过全量重建：reconcile 的 innerHTML='' 会抹掉流式气泡；
    // 插话消息由 SSE interject 事件即时渲染（gmsgSeen 去重），全量重建推迟到 turn_end reconcile
    if (App.state.typers.size === 0) {
      await reconcileGroupMessages();  // 无在途打字机：保持原重建；SSE 事件到达时由 gmsgSeen 去重
    }
  } catch (e) { toast(e.message); }
});

App.registerCommand('show-group-members', () => {
  const g = App.state.currentGroup;
  if (!g) return;
  $('gm-title').textContent = `${g.name} · ${(g.members || []).length} 位成员`;
  const box = $('gm-list');
  box.innerHTML = '';
  for (const m of g.members || []) {
    const row = document.createElement('div');
    row.className = 'gm-row';
    row.innerHTML = `<span class="gmsg-avatar">${botAvatarHtml({ id: m.bot_id, avatar: m.avatar, avatar_url: m.avatar_url })}</span>
      <span class="gm-name">${escapeHtml(m.name)}</span>
      ${m.is_enabled ? '' : '<span class="tag">已停用</span>'}`;
    box.appendChild(row);
  }
  $('modal-group-members').classList.remove('hidden');
});

App.registerCommand('open-group-editor', async (g) => {
  if (g && !g.members) {
    try { g = await App.api.get(`/api/v1/groups/${g.id}`); } catch (_) {}
  }
  $('group-modal-title').textContent = g ? '群聊设置' : '新建群聊';
  $('ge-name').value = g?.name || '';
  $('ge-topic').value = g?.topic || '';
  const box = $('ge-members');
  box.innerHTML = '';
  const memberIds = new Set((g?.members || []).map((m) => m.bot_id));
  for (const b of App.state.bots) {
    const label = document.createElement('label');
    label.className = 'member-check' + (b.is_enabled ? '' : ' disabled');
    label.innerHTML = `<input type="checkbox" value="${b.id}" ${memberIds.has(b.id) ? 'checked' : ''} ${b.is_enabled ? '' : 'disabled'}>
      <span class="member-avatar">${botAvatarHtml(b)}</span>
      <span class="member-name">${escapeHtml(b.name)}${b.is_enabled ? '' : '（已停用）'}</span>`;
    box.appendChild(label);
  }
  $('modal-group').dataset.groupId = g?.id || '';
  $('modal-group').classList.remove('hidden');
});

App.registerCommand('save-group', async () => {
  const name = $('ge-name').value.trim();
  if (!name) { toast('群名称不能为空'); return; }
  const ids = [...$('ge-members').querySelectorAll('input:checked')].map((c) => Number(c.value));
  if (ids.length < 2 || ids.length > 10) { toast('请选择 2-10 个 Bot'); return; }
  const id = $('modal-group').dataset.groupId;
  const body = { name, topic: $('ge-topic').value.trim(), bot_ids: ids };
  if (id) {
    const cur = App.state.currentGroup;
    if (cur && cur.status !== 'idle') delete body.bot_ids;  // 运行中仅改 name/topic
  }
  try {
    if (id) await App.api.patch(`/api/v1/groups/${id}`, body);
    else await App.api.post('/api/v1/groups', body);
  } catch (e) {
    toast(e.code === 1002 ? '群聊进行中，先停止再修改成员' : e.message);
    return;
  }
  $('modal-group').classList.add('hidden');
  await refreshGroups();
  renderChatList();
  if (App.state.currentGroupId) App.run('open-group-room', App.state.currentGroupId);
  toast('群已保存');
});

App.registerCommand('delete-group', async (id) => {
  const g = App.state.groups.find((x) => x.id === id);
  if (!confirm(`删除群「${g?.name || ''}」？将同时清除群内消息。`)) return;
  await App.api.del(`/api/v1/groups/${id}`);
  if (App.state.currentGroupId === id) App.run('close-group-room');
  await refreshGroups();
  renderChatList();
});

/* ================= 右键菜单 ================= */
// 菜单打开时的固定定位矩形：ctxItemEl 点击会先 closeContextMenus()（display:none 后
// getBoundingClientRect 归零），历史会话子菜单需要以此矩形定位
let ctxMenuRect = null;
function closeContextMenus() {
  $('ctx-menu').classList.add('hidden');
  $('ctx-menu').innerHTML = '';
  const sub = document.getElementById('ctx-sub');
  if (sub) sub.remove();
}
function ctxItemEl(label, onClick, opts = {}) {
  const el = document.createElement('div');
  el.className = 'ctx-item' + (opts.danger ? ' danger' : '');
  el.innerHTML = `<span>${opts.icon || '·'}</span><span>${escapeHtml(label)}</span>`;
  el.onclick = (ev) => { ev.stopPropagation(); closeContextMenus(); onClick(); };
  return el;
}
function ctxSepEl() {
  const el = document.createElement('div');
  el.className = 'ctx-sep';
  return el;
}
App.registerCommand('open-context-menu', (kind, id, x, y) => {
  closeContextMenus();
  const menu = $('ctx-menu');
  menu.classList.remove('hidden');
  menu.style.left = Math.min(x, window.innerWidth - 220) + 'px';
  menu.style.top = Math.min(y, window.innerHeight - 320) + 'px';
  ctxMenuRect = menu.getBoundingClientRect();

  const togglePin = async () => {
    const i = App.state.pinned.findIndex((p) => p.kind === kind && p.id === id);
    if (i >= 0) App.state.pinned.splice(i, 1);
    else App.state.pinned.push({ kind, id });
    await savePinned();
    renderChatList();
  };
  const markUnread = async () => {
    App.state.reads[`${kind}:${id}`] = '1970-01-01T00:00:00Z';
    await saveReads();
    renderChatList();
  };
  const openDetached = () => {
    if (kind === 'bot') {
      const b = App.state.bots.find((x) => x.id === id);
      if (!b) return;
      defaultSessionFor(b).then((s) => {
        if (s) winMsg('opendetach', { target: `session:${s.id}`, name: b.name });
      });
    } else {
      const g = App.state.groups.find((x) => x.id === id);
      winMsg('opendetach', { target: `group:${id}`, name: (g && g.name) || '' });
    }
  };

  menu.appendChild(ctxItemEl('置顶/取消置顶', togglePin, { icon: '📌' }));
  menu.appendChild(ctxItemEl('标记未读', markUnread, { icon: '🔴' }));

  if (kind === 'bot') {
    const b = App.state.bots.find((x) => x.id === id);
    menu.appendChild(ctxSepEl());
    // 历史会话子菜单
    const sessions = App.state.sessions.filter((s) => s.bot_id === id);
    sessions.sort((a, c) => (c.updated_at || '').localeCompare(a.updated_at || ''));
    const histItem = ctxItemEl('历史会话…', () => {
      const sub = document.createElement('div');
      sub.id = 'ctx-sub';
      sub.className = 'ctx-sub';
      const base = ctxMenuRect || { right: 2, top: 0 };
      sub.style.left = (base.right + 2) + 'px';
      sub.style.top = base.top + 'px';
      for (const s of sessions) {
        const wrap = document.createElement('div');
        wrap.className = 'ctx-sess';
        // 第一行：打开该会话（含非默认会话）
        const head = document.createElement('div');
        head.className = 'ctx-item';
        head.innerHTML = `<span>💬</span><span>${escapeHtml(s.title)}</span>` +
          (s.id === App.state.currentSessionId ? '<span class="tag">当前</span>' : '');
        head.onclick = (ev) => {
          ev.stopPropagation();
          closeContextMenus();
          App.run('select-session', s.id);
        };
        // 第二行：五个动作（默认 = touch 该会话刷新 updated_at）
        const bar = document.createElement('div');
        bar.className = 'ctx-sess-actions';
        const miniBtn = (label, onClick, danger) => {
          const b = document.createElement('button');
          b.type = 'button';
          b.className = 'ctx-mini-btn' + (danger ? ' danger' : '');
          b.textContent = label;
          b.onclick = (ev) => {
            ev.stopPropagation();
            closeContextMenus();
            onClick();
          };
          return b;
        };
        bar.appendChild(miniBtn('默认', async () => {
          await App.api.patch(`/api/v1/sessions/${s.id}`, { title: s.title });
          await refreshSessions();
          renderChatList();
        }));
        // rename/delete 命令内部已刷新会话列表，不重复渲染
        bar.appendChild(miniBtn('重命名', () => App.run('rename-session', s.id)));
        bar.appendChild(miniBtn('清空', () => App.run('clear-session', s.id)));
        bar.appendChild(miniBtn('导出', () => App.run('export-session', s.id)));
        bar.appendChild(miniBtn('删除', () => App.run('delete-session', s.id), true));
        wrap.appendChild(head);
        wrap.appendChild(bar);
        sub.appendChild(wrap);
      }
      if (!sessions.length) {
        const empty = document.createElement('div');
        empty.className = 'ctx-item';
        empty.textContent = '暂无会话（点击 Bot 项自动创建）';
        sub.appendChild(empty);
      }
      document.body.appendChild(sub);
    }, { icon: '💬' });
    menu.appendChild(histItem);
    menu.appendChild(ctxItemEl('编辑 Bot', () => App.run('open-bot-editor', b), { icon: '✏️' }));
    menu.appendChild(ctxItemEl('记忆库', () => { App.state.memoryBotId = id; switchView('memory'); }, { icon: '🧠' }));
    menu.appendChild(ctxItemEl('打开独立聊天窗口', openDetached, { icon: '🪟' }));
    menu.appendChild(ctxSepEl());
    menu.appendChild(ctxItemEl('删除 Bot', () => App.run('delete-bot', id), { icon: '🗑', danger: true }));
  } else {
    menu.appendChild(ctxSepEl());
    menu.appendChild(ctxItemEl('群设置', () => App.run('open-group-editor', App.state.groups.find((g) => g.id === id)), { icon: '⚙' }));
    menu.appendChild(ctxItemEl('打开独立聊天窗口', openDetached, { icon: '🪟' }));
    menu.appendChild(ctxSepEl());
    menu.appendChild(ctxItemEl('删除群', () => App.run('delete-group', id), { icon: '🗑', danger: true }));
  }
});
document.addEventListener('click', (e) => {
  if (!e.target.closest('.ctx-menu') && !e.target.closest('.ctx-sub')) closeContextMenus();
});
document.addEventListener('contextmenu', (e) => {
  if (!e.target.closest('#session-list')) closeContextMenus();
});

/* ================= 记忆库 ================= */
function renderMemoryBots() {
  const el = $('memory-bot-list');
  el.innerHTML = '';
  const bots = App.state.bots || [];
  if (!App.state.memoryBotId && bots.length) App.state.memoryBotId = bots[0].id;
  for (const b of bots) {
    const item = document.createElement('div');
    item.className = 'list-item' + (b.id === App.state.memoryBotId ? ' active' : '');
    item.innerHTML = `<div class="list-item-avatar">${botAvatarHtml(b)}</div>
      <div class="list-item-body"><div class="list-item-title">${escapeHtml(b.name)}</div></div>`;
    item.onclick = () => { App.state.memoryBotId = b.id; renderMemoryBots(); loadMemories(); };
    el.appendChild(item);
  }
}

async function loadMemories(q = '') {
  const id = App.state.memoryBotId;
  const el = $('mem-list');
  el.innerHTML = '';
  if (!id) { $('mem-empty').classList.remove('hidden'); return; }
  const res = await App.api.get(`/api/v1/bots/${id}/memories${q ? `?q=${encodeURIComponent(q)}` : ''}`);
  App.state.memories = res.memories || [];
  $('mem-empty').classList.toggle('hidden', App.state.memories.length > 0);
  for (const m of App.state.memories) {
    const card = document.createElement('div');
    card.className = 'mem-item';
    const grp = App.state.groups.find((g) => g.id === m.group_id);
    const scopeLabel = m.scope === 'group' ? `群聊 · ${escapeHtml(grp?.name || '已删除群')}` : '私聊';
    const tags = (() => { try { return JSON.parse(m.tags || '[]'); } catch (_) { return []; } })();
    const srcLink = m.source_session_id
      ? `<button class="btn-ghost" data-src="${m.source_session_id}">来源</button>` : '';
    card.innerHTML = `
      <div class="mem-content">${escapeHtml(m.content)}</div>
      <div class="mem-meta">
        <span class="mem-scope">${scopeLabel}</span>
        ${tags.map((t) => `<span>#${escapeHtml(t)}</span>`).join(' ')}
        <span>${escapeHtml(m.updated_at || '')}</span>
        <span class="mem-actions">
          ${srcLink}
          <button class="btn-ghost" data-edit="${m.id}">编辑</button>
          <button class="btn-ghost" data-del="${m.id}">删除</button>
        </span>
      </div>`;
    card.querySelector('[data-edit]')?.addEventListener('click', () => openMemoryEditor(m));
    card.querySelector('[data-del]')?.addEventListener('click', async () => {
      if (!confirm('删除这条记忆？')) return;
      await App.api.del(`/api/v1/bots/${id}/memories/${m.id}`);
      toast('记忆已删除');
      loadMemories($('mem-search').value.trim());
    });
    card.querySelector('[data-src]')?.addEventListener('click', () => {
      App.run('select-session', Number(card.querySelector('[data-src]').dataset.src));
      switchView('messages');
    });
    el.appendChild(card);
  }
}

let editingMemoryId = null;
function openMemoryEditor(m) {
  editingMemoryId = m.id;
  $('me-content').value = m.content;
  $('modal-memory').classList.remove('hidden');
}

/* ================= 插件 ================= */
// 已确认加载过服务端技能的 Bot：未确认（首次进入/加载失败）时勾选行禁用，避免按空缓存整表覆盖真实技能
const botSkillsLoaded = new Set();
// PUT /bots/{id}/skills 是整表替换，且并发请求的完成顺序不保证：按 Bot 记录「链在飞 + 最后一次待提交意图 + 最后一次服务端确认集」
const skillWrites = new Map();
function skillWriteState(botId) {
  let st = skillWrites.get(botId);
  if (!st) { st = { running: false, pending: null, trusted: [] }; skillWrites.set(botId, st); }
  return st;
}
function newSkillJob(readSkills) {
  let resolve;
  let reject;
  const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
  return {
    read: readSkills,
    promise,
    done: () => resolve(),
    fail: (e) => reject(e),
    follow(next) { next.then(resolve, reject); },
  };
}

async function loadSkills() {
  const res = await App.api.get('/api/v1/skills');
  App.state.skills = res.skills || [];
  renderSkillCards();
}
function renderSkillCards() {
  const el = $('skills-list');
  el.innerHTML = '';
  for (const s of App.state.skills) {
    const card = document.createElement('div');
    card.className = 'skill-card';
    card.innerHTML = `<h4>${escapeHtml(s.name)}</h4><p>${escapeHtml(s.description)}</p>`;
    el.appendChild(card);
  }
}
async function loadBotSkills(botId) {
  botSkillsLoaded.delete(botId);
  try {
    const res = await App.api.get(`/api/v1/bots/${botId}/skills`);
    App.state.botSkills[botId] = res.skills || [];
    skillWriteState(botId).trusted = App.state.botSkills[botId];
    botSkillsLoaded.add(botId);
  } finally {
    renderBotSkills();
  }
}
function submitBotSkills(botId, readSkills) {
  if (!botSkillsLoaded.has(botId)) return Promise.reject(new Error('技能状态未确认，已取消保存'));
  const st = skillWriteState(botId);
  const job = newSkillJob(readSkills);
  // 同一 Bot 只保留最后一次待提交意图，且提交时刻才读当前状态，新 payload 已含旧意图 → 旧调用方跟随新结果
  if (st.pending) st.pending.follow(job.promise);
  st.pending = job;
  if (!st.running) {
    st.running = true; // 置位在调用之前：链可能同步跑完并在 finally 里复位
    runSkillSubmits(botId, st);
  }
  return job.promise;
}
async function runSkillSubmits(botId, st) {
  try {
    while (st.pending) {
      const job = st.pending;
      st.pending = null;
      try {
        const skills = job.read();
        await App.api.put(`/api/v1/bots/${botId}/skills`, { skills });
        st.trusted = skills;
        botSkillsLoaded.add(botId);
        // 已排队后续提交时不回写，避免用旧快照覆盖更新的用户意图
        if (!st.pending) App.state.botSkills[botId] = skills;
        job.done();
      } catch (e) {
        botSkillsLoaded.delete(botId);
        // 写失败 = 服务端未变，回滚乐观勾选（同 Bot 已有更新意图时不回滚，让最后一次操作胜出）
        if (!st.pending) App.state.botSkills[botId] = st.trusted;
        job.fail(e);
      }
    }
  } finally {
    st.running = false;
  }
}
function renderBotSkills() {
  const sel = $('skill-bot-select');
  sel.innerHTML = '';
  for (const b of App.state.bots) {
    const o = document.createElement('option');
    o.value = b.id;
    o.textContent = b.name;
    if (b.id === App.state.pluginBotId) o.selected = true;
    sel.appendChild(o);
  }
  const id = App.state.pluginBotId;
  const el = $('bot-skills-list');
  el.innerHTML = '';
  if (!id) return;
  const loaded = botSkillsLoaded.has(id);
  const enabled = new Map((App.state.botSkills[id] || []).map((s) => [s.skill_id, s.enabled]));
  for (const s of App.state.skills) {
    const row = document.createElement('label');
    row.className = 'bot-skill-row';
    const on = enabled.get(s.id) ?? false;
    row.innerHTML = `<span>${escapeHtml(s.name)}</span>
      <input type="checkbox" data-skill="${escapeHtml(s.id)}" ${on ? 'checked' : ''}${loaded ? '' : ' disabled'}>`;
    if (loaded) {
      row.querySelector('input').addEventListener('change', async (e) => {
        const next = (App.state.botSkills[id] || []).filter((x) => x.skill_id !== s.id);
        if (e.target.checked) next.push({ skill_id: s.id, enabled: true });
        App.state.botSkills[id] = next;
        try {
          await submitBotSkills(id, () => (App.state.botSkills[id] || [])
            .filter((x) => x.enabled).map((x) => ({ skill_id: x.skill_id, enabled: true })));
          toast('技能已保存');
        } catch (err) {
          toast(err.message || '技能保存失败');
        }
        renderBotSkills();
      });
    }
    el.appendChild(row);
  }
}

/* ================= 设置 ================= */
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

/* ================= 启动 ================= */
function backendOnline(online) {
  $('status-dot').className = 'status-dot ' + (online ? 'ok' : 'err');
  $('status-text').textContent = online ? '后端已连接' : '后端未连接';
}

App.registerCommand('backend-status', (p) => {
  backendOnline(p && p.status === 'up');
});

/* 提醒到期提示：仅主窗口轮询，toast 走 textContent 渲染，故直接传原文不做转义 */
const REMINDER_POLL_MS = 15000;
// toast() 是单元素、后写覆盖前写：一轮最多逐条提示这么多条，其余合并成一条计数提示，
// 否则一次 tick 里多条到期只有最后一条看得见，前面的却已被标记完成（静默丢失）。
const REMINDER_TOAST_MAX = 2;
const remindersSurfaced = new Set();
let reminderPolling = false;
let reminderTimer = null;

// 呈现并回执：只有文案真正落到 toast 元素上才算「已提示」，调用方据此才记账/标完成
function surfaceToast(msg) {
  toast(msg);
  const el = $('toast');
  return !!el && el.textContent === msg;
}

async function reminderTick() {
  if (reminderPolling) return;
  reminderPolling = true;
  try {
    const res = await App.api.get('/api/v1/reminders?due=1');
    // 本轮新到期的按 due_at 升序（服务端已排序，这里再排一次保证「最早的先占 toast 位」）
    const fresh = (((res && res.reminders) || []).filter((r) => !remindersSurfaced.has(r.id)))
      .sort((a, b) => String(a.due_at || '').localeCompare(String(b.due_at || '')));
    if (!fresh.length) return;
    const head = fresh.slice(0, REMINDER_TOAST_MAX);
    const rest = fresh.slice(REMINDER_TOAST_MAX);
    const shown = [];
    for (const r of head) {
      const bot = App.state.bots.find((b) => b.id === r.bot_id);
      if (surfaceToast(bot ? `${bot.name}提醒你：${r.content}` : `提醒你：${r.content}`)) {
        remindersSurfaced.add(r.id);
        shown.push(r.id);
      }
    }
    // 剩余条合并为一条计数提示：合并 toast 覆盖了它们，才视为已呈现
    if (rest.length && surfaceToast(`另有 ${rest.length} 条到期提醒`)) {
      for (const r of rest) {
        remindersSurfaced.add(r.id);
        shown.push(r.id);
      }
    }
    // 先记账再标 done：POST 失败也只是本会话不再重复提示，绝不死循环刷屏
    for (const id of shown) {
      try { await App.api.post(`/api/v1/reminders/${id}/done`); } catch (_) {}
    }
  } catch (_) {
  } finally {
    reminderPolling = false;
  }
}
function startReminderPoll() {
  if (App.state.detached || reminderTimer) return;
  reminderTimer = setInterval(reminderTick, REMINDER_POLL_MS);
}

App.registerCommand('boot', async () => {
  const params = new URLSearchParams(location.search);
  App.state.detached = params.get('window') === 'detached';
  if (App.state.detached) {
    $('sidebar').classList.add('hidden');
    $('titlebar').classList.add('hidden');
    const target = params.get('target') || (window.__DETACH_TARGET__ || '');
    if (target.startsWith('session:')) App.state.detachedTarget = { kind: 'session', id: Number(target.slice(8)) };
    else if (target.startsWith('group:')) App.state.detachedTarget = { kind: 'group', id: Number(target.slice(6)) };
    // 独立窗口轮询：3s 拉最新消息（单聊）
    let seenKey = null;
    let polling = false;
    setInterval(async () => {
      if (App.state.detachedTarget?.kind !== 'session' || !App.state.currentSessionId) return;
      if (App.state.streaming || polling) return;
      const sid = App.state.currentSessionId;
      polling = true;
      try {
        const msgs = await App.api.get(`/api/v1/messages?session_id=${sid}&limit=100`);
        if (App.state.currentSessionId !== sid) return;
        const first = msgs[0] || {};
        const key = `${msgs.length}:${first.id || 0}:${(first.content || '').length}`;
        if (key === seenKey) return;
        // boot 已画过一遍：首 tick 以 DOM 实际渲染的最新消息对齐服务端快照，一致才记基线，间隙新落库的消息不会被跳过
        if (seenKey === null) {
          const ids = messagesEl.querySelectorAll('.msg[data-msg-id]');
          const lastId = ids.length ? Number(ids[ids.length - 1].dataset.msgId) : 0;
          if (lastId && `${msgs.length}:${lastId}:${(first.content || '').length}` === key) { seenKey = key; return; }
        }
        const stick = messagesEl.scrollHeight - messagesEl.scrollTop - messagesEl.clientHeight < 40;
        const top = messagesEl.scrollTop;
        await loadMessages(sid);
        seenKey = key;
        if (!stick) messagesEl.scrollTop = top;
      } catch (_) {
      } finally {
        polling = false;
      }
    }, 3000);
  }
  try {
    App.state.config = await App.api.get('/api/v1/config');
    applyTheme(App.state.config.theme);
    try { App.state.pinned = JSON.parse(App.state.config.chat_pinned || '[]'); } catch (_) { App.state.pinned = []; }
    try { App.state.reads = JSON.parse(App.state.config.chat_reads || '{}'); } catch (_) { App.state.reads = {}; }
  } catch (_) {}
  try {
    await Promise.all([refreshBots(), refreshSessions(), refreshGroups(),
                       App.state.detached ? null : loadSkills()]);
    await renderChatList();
    backendOnline(true);
  } catch (_) {
    backendOnline(false);
  }
  if (!App.state.detached) {
    startReminderPoll();
    const last = App.state.sessions[0];
    if (last) App.run('select-session', last.id);
    else {
      $('messages').classList.add('hidden');
      $('composer').classList.add('hidden');
      $('chat-empty').classList.remove('hidden');
    }
  }
  if (App.state.detached) {
    if (App.state.detachedTarget?.kind === 'session') {
      App.run('select-session', App.state.detachedTarget.id);
    } else if (App.state.detachedTarget?.kind === 'group') {
      App.run('open-group-room', App.state.detachedTarget.id);
      switchView('messages');
    }
  }
});

/* ================= 事件绑定 ================= */
document.addEventListener('DOMContentLoaded', () => App.run('boot'));

/* WPF 桥接（T1 契约） */
const wv = window.chrome && window.chrome.webview;
function winMsg(action, data) {
  if (!wv) return;
  wv.postMessage({ type: 'window', payload: { action, ...(data || {}) } });
}
$('titlebar').addEventListener('pointerdown', (e) => {
  if (e.button !== 0 || e.target.closest('.tb-actions')) return;
  winMsg('drag');
});
$('titlebar').addEventListener('dblclick', (e) => {
  if (e.target.closest('.tb-actions')) return;
  winMsg('maxdbl');
});
$('titlebar').addEventListener('contextmenu', (e) => {
  if (e.target.closest('.tb-actions')) return;
  e.preventDefault();
  winMsg('sysmenu');
});
if (wv) {
  $('btn-min').addEventListener('click', () => winMsg('minimize'));
  $('btn-max').addEventListener('click', () => winMsg('maximize'));
  $('btn-close').addEventListener('click', () => winMsg('close'));
  wv.addEventListener('message', (e) => {
    try {
      const m = typeof e.data === 'string' ? JSON.parse(e.data) : e.data;
      if (m.type === 'backend-status') App.run('backend-status', m.payload);
    } catch (_) {}
  });
}
$('btn-quit').addEventListener('click', () => winMsg('close'));

document.querySelectorAll('.tab-btn').forEach((b) =>
  b.addEventListener('click', () => switchView(b.dataset.view)));

$('search-input').addEventListener('input', (e) => {
  App.state.search = e.target.value;
  if (App.state.view === 'messages') renderChatList();
  else if (App.state.view === 'bots') renderBots();
  else renderGroups();
});

$('btn-send').addEventListener('click', () => App.run('send-message'));
$('btn-stop').addEventListener('click', () => App.run('stop-stream'));
$('btn-new-session').addEventListener('click', () => App.run('new-session'));
$('btn-new-bot').addEventListener('click', () => App.run('open-bot-editor', null));
$('btn-new-group').addEventListener('click', () => App.run('open-group-editor', null));
$('btn-settings').addEventListener('click', () => App.run('open-settings'));
$('btn-export').addEventListener('click', () => App.run('export-session'));
$('btn-clear').addEventListener('click', () => App.run('clear-session'));
$('btn-switch-bot').addEventListener('click', () => App.run('switch-session-bot'));
$('btn-bot-edit').addEventListener('click', () => {
  const b = App.state.bots.find((x) => x.id === App.state.selectedBotId);
  if (b) App.run('open-bot-editor', b);
});
$('btn-bot-delete').addEventListener('click', () => {
  const b = App.state.bots.find((x) => x.id === App.state.selectedBotId);
  if (b) App.run('delete-bot', b.id);
});
$('btn-settings-save').addEventListener('click', () => App.run('save-settings'));
$('btn-settings-cancel').addEventListener('click', () => $('modal-settings').classList.add('hidden'));
$('btn-bot-save').addEventListener('click', () => App.run('save-bot'));
$('btn-bot-cancel').addEventListener('click', () => $('modal-bot').classList.add('hidden'));
$('btn-group-save').addEventListener('click', () => App.run('save-group'));
$('btn-group-cancel').addEventListener('click', () => $('modal-group').classList.add('hidden'));
$('btn-ns-save').addEventListener('click', () => App.run('save-new-session'));
$('btn-ns-cancel').addEventListener('click', () => $('modal-new-session').classList.add('hidden'));
$('btn-gm-close').addEventListener('click', () => $('modal-group-members').classList.add('hidden'));
$('btn-group-members').addEventListener('click', () => App.run('show-group-members'));
$('btn-group-rename').addEventListener('click', () => App.run('open-group-editor', App.state.currentGroup));
$('btn-group-start').addEventListener('click', () => App.run('group-start'));
$('btn-group-pause').addEventListener('click', () => App.run('group-pause'));
$('btn-group-resume').addEventListener('click', () => App.run('group-resume'));
$('btn-group-stop').addEventListener('click', () => App.run('group-stop'));
$('btn-group-interject').addEventListener('click', () => App.run('group-interject'));

$('btn-mem-search').addEventListener('click', () => loadMemories($('mem-search').value.trim()));
$('mem-search').addEventListener('keydown', (e) => { if (e.key === 'Enter') loadMemories($('mem-search').value.trim()); });
$('btn-mem-refresh').addEventListener('click', () => loadMemories($('mem-search').value.trim()));

$('skill-bot-select').addEventListener('change', async (e) => {
  App.state.pluginBotId = Number(e.target.value);
  try {
    await loadBotSkills(App.state.pluginBotId);
  } catch (err) {
    toast(err.message || '技能加载失败');
  }
});
$('btn-skill-refresh').addEventListener('click', async () => {
  try {
    await loadSkills();
    if (App.state.pluginBotId) await loadBotSkills(App.state.pluginBotId);
  } catch (e) {
    toast(e.message || '技能加载失败');
  }
});
$('btn-memory-cancel').addEventListener('click', () => $('modal-memory').classList.add('hidden'));
$('btn-memory-save').addEventListener('click', async () => {
  if (!editingMemoryId) return;
  const content = $('me-content').value.trim();
  if (!content) { toast('记忆内容不能为空'); return; }
  await App.api.put(`/api/v1/bots/${App.state.memoryBotId}/memories/${editingMemoryId}`, { content });
  $('modal-memory').classList.add('hidden');
  toast('记忆已更新');
  loadMemories($('mem-search').value.trim());
});

inputEl.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) {
    e.preventDefault();
    App.run('send-message');
  }
});

$('group-interject-input').addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && !e.isComposing) {
    e.preventDefault();
    App.run('group-interject');
  }
});

messagesEl.addEventListener('click', (e) => {
  const btn = e.target.closest('[data-action]');
  if (!btn) return;
  const msgEl = btn.closest('.msg');
  const msgId = msgEl?.dataset.msgId;
  if (btn.dataset.action === 'copy' && msgEl) App.run('copy-message', msgEl);
  else if (btn.dataset.action === 'delete' && msgId) App.run('delete-message', msgId);
  else if (btn.dataset.action === 'regenerate') App.run('regenerate');
});

$('group-messages').addEventListener('click', async (e) => {
  const btn = e.target.closest('[data-action="copy"]');
  if (!btn) return;
  const content = btn.closest('.gmsg').querySelector('.gmsg-content');
  if (!content) return;
  try {
    await navigator.clipboard.writeText(content.innerText);
    toast('已复制');
  } catch (_) { toast('复制失败'); }
});

$('bot-list').addEventListener('change', async (e) => {
  const t = e.target.closest('[data-bot-toggle]');
  if (!t) return;
  const id = Number(t.dataset.botToggle);
  try {
    await App.api.post(`/api/v1/bots/${id}/enable`, { enabled: t.checked });
    const b = App.state.bots.find((x) => x.id === id);
    if (b) b.is_enabled = t.checked;
    renderBots();
    toast(t.checked ? '已启用' : '已停用');
  } catch (err) {
    t.checked = !t.checked;
    toast(err.message);
  }
});

$('be-avatar-file').addEventListener('change', async () => {
  const f = $('be-avatar-file').files[0];
  if (!f) return;
  if (f.size > 256 * 1024) { toast('图片超过 256KB'); return; }
  const img = new Image();
  img.onload = () => {
    const c = document.createElement('canvas');
    c.width = c.height = 128;
    const g = c.getContext('2d');
    g.drawImage(img, 0, 0, 128, 128);
    App.state.botAvatarFile = c.toDataURL('image/png').split(',')[1];
    App.state.botAvatarRemove = false;
    $('be-avatar-img').src = c.toDataURL('image/png');
    $('be-avatar-preview').classList.remove('hidden');
  };
  img.src = URL.createObjectURL(f);
});
$('btn-avatar-remove').addEventListener('click', () => {
  App.state.botAvatarFile = null;
  App.state.botAvatarRemove = true;
  $('be-avatar-preview').classList.add('hidden');
  $('be-avatar-file').value = '';
});
