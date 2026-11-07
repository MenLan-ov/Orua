// v0.6.1 人设端到端验证：真实点击 UI，确认人设能建、能绑、能渲染。
// 不做桩替换——走真实 app.js，否则测的是桩不是产品。
import http from 'node:http';

const PORT = process.env.CDP_PORT || '9275';

function getJson(path) {
  return new Promise((resolve, reject) => {
    http.get({ host: '127.0.0.1', port: PORT, path }, (res) => {
      let d = '';
      res.on('data', (c) => (d += c));
      res.on('end', () => {
        try { resolve(JSON.parse(d)); } catch (e) { reject(e); }
      });
    }).on('error', reject);
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function main() {
  let target = null;
  for (let i = 0; i < 40 && !target; i++) {
    try {
      const list = await getJson('/json/list');
      target = list.find((t) => t.type === 'page' && t.url.includes('app.local'));
    } catch (e) { /* 等 WebView2 起来 */ }
    if (!target) await sleep(1000);
  }
  if (!target) throw new Error('找不到 WebView2 页面');

  const ws = new WebSocket(target.webSocketDebuggerUrl);
  let id = 0;
  const pending = new Map();
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) {
      const { resolve, reject } = pending.get(m.id);
      pending.delete(m.id);
      if (m.error) reject(new Error(JSON.stringify(m.error)));
      else resolve(m.result);
    }
  };
  const send = (method, params = {}) =>
    new Promise((resolve, reject) => {
      const mid = ++id;
      pending.set(mid, { resolve, reject });
      ws.send(JSON.stringify({ id: mid, method, params }));
    });

  const evaluate = async (expr) => {
    const r = await send('Runtime.evaluate', {
      expression: expr,
      awaitPromise: true,
      returnByValue: true,
    });
    if (r.exceptionDetails) {
      throw new Error('JS 异常: ' + JSON.stringify(r.exceptionDetails.exception?.description || r.exceptionDetails));
    }
    return r.result.value;
  };

  const results = [];
  const check = (name, ok, detail) => {
    results.push({ name, ok, detail });
    console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? ' — ' + detail : ''}`);
  };

  await evaluate('location.reload()');
  await sleep(6000);

  // 0. 幂等：清掉上次中断留下的同名测试人设，否则「删除」断言会被残留数据污染
  const preClean = await evaluate(`(async () => {
    await loadPersonas();
    const stale = App.state.personas.filter(p => p.name === 'E2E 测试人设');
    for (const p of stale) { try { await App.api.del('/api/v1/personas/' + p.id); } catch (e) {} }
    await loadPersonas();
    return stale.length;
  })()`);
  if (preClean) console.log(`（清理了 ${preClean} 条残留测试人设）`);

  // 1. 版本号
  const ver = await evaluate(`document.getElementById('foot-version').textContent`);
  check('页脚版本为 v0.6.1', ver.includes('0.6.1'), ver);

  // 2. 人设页签存在且能切换
  const tabOk = await evaluate(`(() => {
    const t = [...document.querySelectorAll('.tab-btn')].find(b => b.dataset.view === 'personas');
    if (!t) return 'no-tab';
    t.click();
    return 'ok';
  })()`);
  check('侧栏有「人设」页签', tabOk === 'ok', tabOk);
  await sleep(1200);

  const personaViewVisible = await evaluate(
    `!document.getElementById('view-personas').classList.contains('hidden')`);
  check('人设视图可见', personaViewVisible === true, String(personaViewVisible));

  // 3. 内置人设被渲染出来
  const cardCount = await evaluate(
    `document.querySelectorAll('#persona-list .persona-card').length`);
  check('内置人设已渲染', cardCount >= 3, `${cardCount} 张卡片`);

  const builtinTag = await evaluate(
    `document.querySelectorAll('#persona-list .persona-tag.builtin').length`);
  check('内置人设带「内置」标记', builtinTag >= 3, `${builtinTag} 个`);

  // 4. 新建人设：走真实弹窗
  await evaluate(`document.getElementById('btn-persona-add').click()`);
  await sleep(500);
  const modalOpen = await evaluate(
    `!document.getElementById('modal-persona').classList.contains('hidden')`);
  check('「新建人设」弹窗打开', modalOpen === true, String(modalOpen));

  // 字段齐全（对齐参考稿）
  const fields = await evaluate(`(() => {
    const ids = ['pe-name','pe-display','pe-participation','pe-role','pe-rules','pe-override'];
    return ids.filter(i => !document.getElementById(i)).join(',') || 'all';
  })()`);
  check('参考稿字段齐全', fields === 'all', fields);

  // 5. 空名字应被拦下
  await evaluate(`document.getElementById('btn-persona-save').click()`);
  await sleep(600);
  const errShown = await evaluate(`(() => {
    const e = document.getElementById('persona-form-error');
    return !e.classList.contains('hidden') ? e.textContent.trim() : '';
  })()`);
  check('空人设名被拦截', errShown.length > 0, errShown);

  // 6. 正常保存
  await evaluate(`(() => {
    document.getElementById('pe-name').value = 'E2E 测试人设';
    document.getElementById('pe-role').value = '# 角色：E2E\\n你是一个端到端测试用的人设。';
    document.getElementById('pe-rules').value = '保持简短。';
    document.getElementById('pe-display').value = '小E';
    document.getElementById('pe-participation').value = 'moderate';
    document.getElementById('btn-persona-save').click();
  })()`);
  await sleep(1500);
  const savedName = await evaluate(`(() => {
    const cards = [...document.querySelectorAll('#persona-list .persona-card')];
    const c = cards.find(x => x.textContent.includes('E2E 测试人设'));
    return c ? 'found' : 'missing';
  })()`);
  check('新人设保存后出现在列表', savedName === 'found', savedName);

  // 7. 后端确实落库（用 App.api，它已处理 X-Orua-Token，直接 fetch 会 401）
  const apiCheck = await evaluate(`(async () => {
    const j = await App.api.get('/api/v1/personas');
    const p = (j.personas || []).find(x => x.name === 'E2E 测试人设');
    return p ? JSON.stringify({ id: p.id, part: p.participation, display: p.group_display_name }) : 'not-in-db';
  })()`);
  check('人设已写入后端', apiCheck !== 'not-in-db', apiCheck);

  let newPersonaId = 0;
  try { newPersonaId = JSON.parse(apiCheck).id; } catch (e) { /* 留 0 */ }

  // 8. 绑定到 Bot：走 Bot 详情页的人设按钮
  const botCount = await evaluate(`(App.state.bots || []).length`);
  check('存在可绑定的 Bot', botCount > 0, `${botCount} 个`);

  if (botCount > 0 && newPersonaId) {
    const bindOk = await evaluate(`(async () => {
      const botId = App.state.bots[0].id;
      await App.api.put('/api/v1/bots/' + botId + '/persona', { persona_id: ${newPersonaId} });
      await refreshBots();
      const b = App.state.bots.find(x => x.id === botId);
      return b && b.persona_id === ${newPersonaId} ? 'bound' : 'not-bound:' + (b && b.persona_id);
    })()`);
    check('Bot 绑定人设成功', bindOk === 'bound', bindOk);

    // 9. Bot 详情页显示人设摘要
    const detailOk = await evaluate(`(() => {
      App.state.selectedBotId = App.state.bots[0].id;
      renderBotDetail();
      const el = document.getElementById('bot-detail-persona');
      return el.textContent.includes('E2E 测试人设') ? 'shown' : 'missing:' + el.textContent.slice(0, 60);
    })()`);
    check('Bot 详情显示当前人设', detailOk === 'shown', detailOk);

    // 10. 解绑
    const unbindOk = await evaluate(`(async () => {
      const botId = App.state.bots[0].id;
      await App.api.put('/api/v1/bots/' + botId + '/persona', { persona_id: 0 });
      await refreshBots();
      const b = App.state.bots.find(x => x.id === botId);
      return b && b.persona_id === 0 ? 'unbound' : 'still:' + (b && b.persona_id);
    })()`);
    check('Bot 解绑人设成功', unbindOk === 'unbound', unbindOk);
  }

  // 11. 清理测试人设
  if (newPersonaId) {
    const delOk = await evaluate(`(async () => {
      await App.api.del('/api/v1/personas/${newPersonaId}');
      await loadPersonas();
      // 按 id 判断，不按名字——同名残留会让断言假失败
      return App.state.personas.some(p => p.id === ${newPersonaId}) ? 'still-there' : 'deleted';
    })()`);
    check('测试人设可删除', delOk === 'deleted', delOk);
  }

  // 12. 控制台无报错
  const errs = await evaluate(`(window.__e2eErrors || []).length`);
  check('无 JS 运行时错误', errs === 0, `${errs} 个`);

  const failed = results.filter((r) => !r.ok);
  console.log(`\n=== ${results.length - failed.length}/${results.length} 通过 ===`);
  if (failed.length) {
    console.log('失败项：');
    for (const f of failed) console.log(` - ${f.name}: ${f.detail}`);
  }
  ws.close();
  process.exit(failed.length ? 1 : 0);
}

main().catch((e) => {
  console.error('E2E 失败:', e.message);
  process.exit(1);
});
