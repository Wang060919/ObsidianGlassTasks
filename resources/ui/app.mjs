import { createMarkdown, extractTasks, noteUrl } from './tasks.mjs';

const elements = Object.fromEntries(['tasks','count','empty','loading','status','notice','notice-text','undo','refresh','open-note','menu'].map(id => [id, document.getElementById(id)]));
const bridge = window.chrome?.webview;
let settings, markdown, tasks = [], source = null, pending = null, undoTask = null, readable = false, fatal = null, sourceError = false;
let fixtureStarted = false, fixtureResult, materialError = '', paused = false;
let probeStarted = false, nativeProbe = null;
let noticeTimer = null;
const errors = [];
function send(message) { if (bridge) bridge.postMessage(message); }
function notice(message, error = false, duration = 0) {
  if (noticeTimer) { clearTimeout(noticeTimer); noticeTimer = null; }
  elements.notice.hidden = !message;
  elements['notice-text'].textContent = message;
  elements.notice.classList.toggle('error', error);
  elements.undo.hidden = !undoTask;
  fitHeight();
  if (message && duration) noticeTimer = setTimeout(() => notice(''), duration);
}
function controls() {
  document.querySelectorAll('input[type=checkbox]').forEach(input => { input.disabled = !!pending || !readable; });
  elements.undo.disabled = !!pending;
  elements['open-note'].disabled = !!pending || !settings;
}
function fitHeight() {
  if (!settings) return;
  const height = Math.ceil(24 + document.querySelector('.titlebar').offsetHeight + Math.max(elements.tasks.scrollHeight, 150) + document.querySelector('footer').offsetHeight);
  send({ type: 'resize', height });
}
function status() {
  elements.status.textContent = sourceError ? 'Note unavailable' : materialError ? 'Note synced · Glass unavailable' : paused ? 'Obsidian · Background paused' : 'Obsidian · Auto sync';
}
function render() {
  const previousFocus = document.activeElement?.dataset?.lineNumber;
  const fragment = document.createDocumentFragment();
  for (const task of tasks) {
    const row = document.createElement('li'); row.className = 'task';
    const label = document.createElement('label'); label.className = 'check-hit'; label.title = '标记完成';
    const checkbox = document.createElement('input'); checkbox.type = 'checkbox';
    checkbox.dataset.lineNumber = String(task.lineNumber);
    checkbox.setAttribute('aria-label', `完成：${task.body}`);
    checkbox.addEventListener('change', async () => {
      row.classList.add('pending');
      try {
        const result = await dispatch({ kind: 'toggle', line: task.line, lineNumber: task.lineNumber, done: true });
        undoTask = { line: result.line, lineNumber: result.lineNumber };
        notice('已完成一项'); refresh();
      } catch (error) { checkbox.checked = false; notice(error.message, true); refresh(); }
      finally { row.classList.remove('pending'); controls(); }
    });
    label.append(checkbox);
    const body = document.createElement('div'); body.className = 'task-body'; body.innerHTML = task.html;
    row.append(label, body); fragment.append(row);
  }
  elements.tasks.replaceChildren(fragment);
  elements.empty.hidden = tasks.length !== 0;
  elements.count.textContent = String(tasks.length);
  elements.loading.hidden = true;
  controls(); fitHeight();
  if (previousFocus !== undefined) {
    const inputs = [...elements.tasks.querySelectorAll('input')];
    (inputs.find(input => Number(input.dataset.lineNumber) >= Number(previousFocus)) || inputs.at(-1) || elements['open-note']).focus({ preventScroll: true });
  }
}
function receiveSource(text) {
  if (!markdown) return;
  try {
    readable = true;
    if (text !== source) { tasks = extractTasks(text, markdown); source = text; render(); }
    if (sourceError) { sourceError = false; notice('笔记已重新连接'); }
    status(); controls();
    if (settings.fixtureTest && !fixtureStarted) { fixtureStarted = true; runFixtureTest(); }
    else if (!settings.fixtureTest && !probeStarted) {
      probeStarted = true;
      dispatch({ kind: 'probe', report: { bridge: 'native' } }).then(result => {
        nativeProbe = result.report?.bridge === 'native'; report();
      }).catch(error => { nativeProbe = false; notice(error.message, true); report(); });
    }
  } catch (error) { receiveSourceError(error.message); }
}
function receiveSourceError(message) {
  readable = false; sourceError = true; elements.loading.hidden = true;
  status(); notice(message, true); controls();
}
function refresh() { send({ type: 'refresh' }); }
function dispatch(request) {
  if (pending) return Promise.reject(new Error('正在保存上一项操作，请稍候'));
  if (!bridge) return Promise.reject(new Error('请从桌面待办程序打开'));
  const id = crypto.randomUUID();
  if (new TextEncoder().encode(JSON.stringify({ ...request, id })).length > 14000) return Promise.reject(new Error('这项任务过长，请在 Obsidian 中操作'));
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      pending = null; controls(); refresh(); reject(new Error('未收到保存结果，请核对笔记后再操作'));
    }, 20000);
    pending = { id, resolve, reject, timeout }; controls();
    try { send({ type: 'action', request: { ...request, id } }); }
    catch (error) { clearTimeout(timeout); pending = null; controls(); reject(error); }
  });
}
function onActionFinished(result) {
  // A late result cannot complete a newer operation.
  if (!pending || result.id !== pending.id) return;
  const active = pending; clearTimeout(active.timeout); pending = null; controls();
  if (result.ok) active.resolve(result); else active.reject(new Error(result.message));
}
elements.undo.addEventListener('click', async () => {
  if (!undoTask) return;
  try { await dispatch({ kind: 'toggle', ...undoTask, done: false }); undoTask = null; notice('已撤销完成', false, 3000); refresh(); }
  catch (error) { notice(error.message, true); }
});
elements.refresh.addEventListener('click', refresh);
elements.menu.addEventListener('click', () => send({ type: 'menu' }));
elements['open-note'].addEventListener('click', () => dispatch({ kind: 'open', url: noteUrl(settings.vault, settings.noteFile) }).catch(error => notice(error.message, true)));
document.querySelector('.titlebar').addEventListener('pointerdown', event => {
  if (event.button === 0 && !event.target.closest('button, a, input')) send({ type: 'drag' });
});
document.addEventListener('click', event => {
  const link = event.target.closest('a'); if (!link) return;
  event.preventDefault(); if (pending) return;
  dispatch({ kind: 'open', url: link.getAttribute('href') }).catch(error => notice(error.message, true));
});
document.addEventListener('auxclick', event => { if (event.target.closest('a')) event.preventDefault(); });
document.addEventListener('contextmenu', event => { event.preventDefault(); send({ type: 'menu' }); });
document.addEventListener('dragover', event => event.preventDefault());
document.addEventListener('drop', event => event.preventDefault());
addEventListener('error', event => { errors.push(event.message); });
addEventListener('unhandledrejection', event => { errors.push(String(event.reason)); });
const motion = matchMedia('(prefers-reduced-motion: reduce)');
function preferences() { send({ type: 'preferences', reduceMotion: motion.matches }); }
motion.addEventListener('change', preferences);
function diagnostics() {
  const heading = document.querySelector('.heading');
  const title = heading.querySelector('h1');
  return {
    ready: !!settings && source !== null, fatal, tasks: tasks.length, readable,
    markdown: !!markdown, links: document.querySelectorAll('.task-body a').length,
    colorScheme: getComputedStyle(document.documentElement).colorScheme,
    textColor: getComputedStyle(document.documentElement).color,
    secondaryTextColor: getComputedStyle(elements.status).color,
    statusText: elements.status.textContent, statusStyle: getComputedStyle(elements.status).fontStyle,
    headingText: title.textContent, headingFont: getComputedStyle(title).fontFamily, headingStyle: getComputedStyle(title).fontStyle,
    headingFontLoaded: document.fonts.check('29px "Parisienne"', 'Inbox'),
    headingOverlap: heading.getBoundingClientRect().right > document.querySelector('.header-actions').getBoundingClientRect().left,
    width: innerWidth, height: innerHeight, dpr: devicePixelRatio,
    documentOverflow: document.documentElement.scrollWidth > innerWidth,
    listScrollable: document.querySelector('.scroll-area').scrollHeight > document.querySelector('.scroll-area').clientHeight,
    bridge: !!bridge, nativeProbe, errors, ...(fixtureResult ? { fixtureResult } : {})
  };
}
function report() { send({ type: 'diagnostics', report: diagnostics() }); }
async function until(predicate) {
  const end = Date.now() + 8000;
  while (!predicate()) { if (Date.now() > end) throw new Error('Fixture source update timed out'); await new Promise(resolve => setTimeout(resolve, 40)); }
}
async function runFixtureTest() {
  try {
    await document.fonts.ready;
    const original = source, count = tasks.length, first = tasks[0];
    if (!first || count < 2) throw new Error('Fixture must contain at least two tasks');
    const probe = await dispatch({ kind: 'probe', report: { bridge: 'native' } });
    if (probe.report?.bridge !== 'native') throw new Error('Native bridge probe failed');
    const result = await dispatch({ kind: 'toggle', line: first.line, lineNumber: first.lineNumber, done: true });
    await until(() => tasks.length === count - 1);
    await dispatch({ kind: 'toggle', line: result.line, lineNumber: result.lineNumber, done: false });
    await until(() => source === original && tasks.length === count);
    const details = diagnostics();
    if (details.documentOverflow || errors.length || !details.links) throw new Error('Fixture Markdown layout failed');
    const checkStatus = expected => {
      status();
      if (elements.status.textContent !== expected || elements.status.getBoundingClientRect().right > elements.refresh.getBoundingClientRect().left) throw new Error('Status text or layout failed');
    };
    paused = true; checkStatus('Obsidian · Background paused');
    materialError = 'fixture'; checkStatus('Note synced · Glass unavailable');
    sourceError = true; checkStatus('Note unavailable');
    paused = false; materialError = ''; sourceError = false; checkStatus('Obsidian · Auto sync');
    fixtureResult = { ok: true, probe: true, completedTaskDisappeared: true, undoRestoredSource: true, statusCases: true, tasks: count, links: details.links };
  } catch (error) { fixtureResult = { ok: false, error: error.message }; }
  report();
}
if (bridge) {
  bridge.addEventListener('message', event => {
    const message = event.data;
    switch (message.type) {
      case 'init': settings = message; markdown = createMarkdown(window.markdownit, settings.vault, settings.noteFile); preferences(); break;
      case 'source': receiveSource(message.text); break;
      case 'source-error': receiveSourceError(message.message); break;
      case 'action-result': onActionFinished(message.result); break;
      case 'material': materialError = message.error; paused = message.paused; status(); break;
      case 'notice': notice(message.message); break;
      case 'diagnose': report(); break;
    }
  });
  send({ type: 'ready' });
} else { fatal = '请从桌面待办程序打开'; notice(fatal, true); controls(); }
window.GlassTasks = { diagnostics };
