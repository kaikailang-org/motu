import { createEditor } from './vendor/codemirror.js';

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
const el = (tag, attrs = {}, ...kids) => {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'class') e.className = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (v !== undefined && v !== null && v !== false) e.setAttribute(k, v === true ? '' : v);
  }
  for (const k of kids.flat()) if (k !== null && k !== undefined) e.append(k);
  return e;
};

const MANUSCRIPT = 'manuscript', RESEARCH = 'research', TRASH = 'trash';
const S = {
  root: null, book: null, sel: null, view: 'editor', scrivenings: false, split: null,
  inspector: true, words: {}, manuscript: 0, sessionStart: null, hits: null,
  expanded: new Set([MANUSCRIPT, RESEARCH]), docs: new Map(), timers: new Map(), editors: [], gen: 0,
};

const call = (cmd, args = {}) => motu.invoke(cmd, { root: S.root, ...args });
const words = (t) => (t.match(/[\p{L}\p{N}][\p{L}\p{N}'’-]*/gu) || []).length;
const node = (id) => S.book.nodes.find((n) => n.id === id);
const kids = (id) => S.book.nodes.filter((n) => n.parent === id);
const descendants = (id) => kids(id).flatMap((n) => [n, ...descendants(n.id)]);
const docsUnder = (id) => descendants(id).filter((n) => n.kind === 'doc');
const isFolder = (n) => n && n.kind === 'folder';
const ancestors = (id) => { const out = []; let n = node(id); while (n && n.parent) { out.push(n.parent); n = node(n.parent); } return out; };
const sessionKey = () => `book.session.${S.root}`;
const sessionTarget = () => Number(localStorage.getItem(sessionKey()) || 1000);

/* ---------- documents: cache and autosave ---------- */

async function loadDoc(id) {
  if (!S.docs.has(id)) {
    const d = await call('read', { id });
    S.docs.set(id, { meta: d.meta, body: d.body, notes: d.notes });
  }
  return S.docs.get(id);
}

function schedule(id, what = 'doc') {
  const key = `${what}:${id}`;
  clearTimeout(S.timers.get(key));
  S.timers.set(key, setTimeout(() => flushOne(key), 700));
  status('Editing…');
}

async function flushOne(key) {
  if (!S.timers.has(key)) return;
  clearTimeout(S.timers.get(key));
  S.timers.delete(key);
  const [what, id] = key.split(':');
  const d = S.docs.get(id);
  if (!d) return;
  if (what === 'notes') { await call('save-notes', { id, text: d.notes }); status('Saved'); return; }
  const n = node(id);
  if (n) d.meta.title = n.title;
  const r = await call('save', { id, meta: d.meta, body: d.body });
  if (typeof r === 'number') { S.words[id] = r; refreshTargets(); }
  else { S.book = r; renderBinder(); }
  status('Saved');
}

async function flushAll() {
  await Promise.all(Array.from(S.timers.keys()).map(flushOne));
}

function status(text) { const t = $('#targets'); if (t) t.dataset.status = text; refreshTargets(); }

/* ---------- welcome ---------- */

async function showWelcome() {
  $('#workspace').classList.add('hidden');
  $('#welcome').classList.remove('hidden');
  const recent = await motu.invoke('recent');
  $('#recent-title').classList.toggle('hidden', recent.length === 0);
  $('#recent').replaceChildren(...recent.map((p) =>
    el('li', { onclick: () => openProject(() => motu.invoke('open', { path: p })) },
      el('div', {}, p.split('/').pop().replace(/\.book$/, '')), el('div', { class: 'muted' }, p))));
}

async function openProject(how) {
  const r = await how();
  if (!r) return;
  S.root = r.path; S.book = r.book; S.docs.clear(); S.sel = null; S.split = null; S.hits = null;
  $('#welcome').classList.add('hidden');
  $('#workspace').classList.remove('hidden');
  await refreshStats();
  S.sessionStart = S.manuscript;
  const first = docsUnder(MANUSCRIPT)[0];
  await select(first ? first.id : MANUSCRIPT);
}

/* ---------- binder ---------- */

const ICON = { folder: '▸', doc: '¶', research: '◫' };

function renderBinder() {
  const visible = S.hits ? new Set(S.hits.flatMap((id) => [id, ...ancestors(id)])) : null;
  const branch = (parent) => el('ul', {}, ...kids(parent)
    .filter((n) => !visible || visible.has(n.id))
    .map((n) => el('li', {}, nodeRow(n), isFolder(n) && (S.expanded.has(n.id) || visible) ? branch(n.id) : null)));
  $('#tree').replaceChildren(branch(''));
}

function nodeRow(n) {
  const folder = isFolder(n);
  const row = el('div', {
    class: `node${n.id === S.sel ? ' sel' : ''}${S.hits && S.hits.includes(n.id) ? ' hit' : ''}`,
    draggable: n.parent !== '' ? 'true' : false, 'data-id': n.id,
    onclick: (e) => { if (e.altKey && n.kind === 'doc') openSplit(n.id); else select(n.id); },
    ondblclick: () => n.parent !== '' && startRename(row, n),
  },
    el('span', { class: 'twist', onclick: (e) => { e.stopPropagation(); toggle(n.id); } }, folder ? (S.expanded.has(n.id) ? '▾' : '▸') : ''),
    el('span', { class: 'icon' }, folder ? '' : ICON[n.kind] || ''),
    el('span', { class: 'title' }, n.title || 'Untitled'),
    n.kind === 'doc' && S.words[n.id] !== undefined ? el('span', { class: 'count' }, String(S.words[n.id])) : null);
  wireDrag(row, n);
  return row;
}

function toggle(id) { S.expanded.has(id) ? S.expanded.delete(id) : S.expanded.add(id); renderBinder(); }

function startRename(row, n) {
  const t = $('.title', row);
  t.contentEditable = 'true'; t.focus();
  document.getSelection().selectAllChildren(t);
  const done = async (keep) => {
    t.contentEditable = 'false';
    const title = t.textContent.trim();
    if (keep && title && title !== n.title) {
      await flushAll();
      S.book = await call('rename', { id: n.id, title });
      const d = S.docs.get(n.id); if (d) d.meta.title = title;
      renderAll();
    } else renderBinder();
  };
  t.onkeydown = (e) => { if (e.key === 'Enter') { e.preventDefault(); done(true); } if (e.key === 'Escape') done(false); };
  t.onblur = () => done(true);
}

let dragId = null;
function wireDrag(row, n) {
  row.addEventListener('dragstart', (e) => { dragId = n.id; e.dataTransfer.effectAllowed = 'move'; e.dataTransfer.setData('text/plain', n.id); });
  row.addEventListener('dragend', () => { dragId = null; clearDrop(); });
  row.addEventListener('dragover', (e) => {
    if (!dragId || dragId === n.id) return;
    e.preventDefault();
    clearDrop();
    row.classList.add(`drop-${dropPlace(e, row, n)}`);
  });
  row.addEventListener('dragleave', clearDrop);
  row.addEventListener('drop', async (e) => {
    if (!dragId) return;
    e.preventDefault();
    const place = dropPlace(e, row, n);
    const id = dragId; clearDrop();
    let parent, index;
    if (place === 'into') { parent = n.id; index = 1e6; S.expanded.add(n.id); }
    else {
      parent = n.parent;
      const siblings = kids(parent).filter((x) => x.id !== id);
      index = siblings.findIndex((x) => x.id === n.id) + (place === 'after' ? 1 : 0);
    }
    if (!parent) return;
    try { S.book = await call('move', { id, parent, index }); renderAll(); } catch (err) { status(err.message); }
  });
}

function dropPlace(e, row, n) {
  const r = row.getBoundingClientRect(), y = (e.clientY - r.top) / r.height;
  if (isFolder(n) && (n.parent === '' || (y > 0.25 && y < 0.75))) return 'into';
  return y < 0.5 ? 'before' : 'after';
}
function clearDrop() { $$('.drop-into,.drop-before,.drop-after').forEach((x) => x.classList.remove('drop-into', 'drop-before', 'drop-after')); }

async function addNode(kind) {
  const sel = node(S.sel) || node(MANUSCRIPT);
  const parent = isFolder(sel) && sel.id !== TRASH ? sel.id : (sel.parent || MANUSCRIPT);
  const index = isFolder(sel) ? 1e6 : kids(parent).findIndex((x) => x.id === sel.id) + 1;
  const r = await call('add', { parent, kind, title: kind === 'folder' ? 'New Folder' : 'Untitled', index });
  S.book = r.book; S.expanded.add(parent);
  await select(r.id);
  const row = $(`.node[data-id="${r.id}"]`);
  if (row) startRename(row, node(r.id));
}

async function trashSelected() {
  const n = node(S.sel);
  if (!n || n.parent === '') return;
  if (n.parent === TRASH || ancestors(n.id).includes(TRASH)) return;
  await flushAll();
  S.book = await call('move', { id: n.id, parent: TRASH, index: 1e6 });
  select(n.parent);
}

/* ---------- selection and panes ---------- */

async function select(id) {
  await flushAll();
  S.sel = id;
  await renderAll();
}

async function renderAll() {
  renderBinder();
  refreshTargets();
  await Promise.all([renderMain(), renderInspector()]);
}

function destroyEditors() { S.editors.forEach((e) => e.destroy()); S.editors = []; }

async function renderMain() {
  S.gen++;
  destroyEditors();
  const main = $('#main');
  main.replaceChildren();
  const panes = [pane(main, S.sel, true)];
  if (S.split && node(S.split)) panes.push(pane(main, S.split, false));
  await Promise.all(panes);
}

async function pane(main, id, primary) {
  const n = node(id);
  const p = el('section', { class: 'pane' });
  main.append(p);
  if (!n) { p.append(el('div', { class: 'empty' }, 'Nothing selected')); return; }
  const header = el('header', {}, el('h2', {}, n.title || 'Untitled'),
    !primary ? el('button', { onclick: () => { S.split = null; renderMain(); } }, 'Close split') : null);
  const scroll = el('div', { class: 'scroll' });
  p.append(header, scroll);
  const folder = isFolder(n);
  if (primary && S.view === 'corkboard') await corkboard(scroll, folder ? n : node(n.parent));
  else if (primary && S.view === 'outliner') await outliner(scroll, folder ? n : node(n.parent));
  else if (n.kind === 'research') research(scroll, n);
  else if (folder || (primary && S.scrivenings)) await scrivenings(scroll, folder ? n : node(n.parent));
  else await editorFor(scroll, n);
}

async function editorFor(parent, n, withTitle = false) {
  const box = el('div', { class: 'doc' });
  if (withTitle) box.append(el('div', { class: 'doc-title' }, n.title || 'Untitled'));
  parent.append(box);
  const gen = S.gen;
  const d = await loadDoc(n.id);
  if (gen !== S.gen) return null;
  const ed = createEditor(box, {
    doc: d.body, placeholderText: 'Write…',
    onChange: (text) => { d.body = text; S.words[n.id] = words(text); schedule(n.id); refreshTargets(); },
  });
  S.editors.push(ed);
  if (!withTitle) setTimeout(() => ed.focus(), 0);
  return ed;
}

async function scrivenings(parent, folder) {
  const docs = docsUnder(folder.id);
  if (docs.length === 0) { parent.append(el('div', { class: 'empty' }, 'This folder has no documents yet')); return; }
  for (const n of docs) await editorFor(parent, n, true);
}

function research(parent, n) {
  const url = `motu://files/${n.file}`;
  const ext = n.file.split('.').pop().toLowerCase();
  const box = el('div', { class: 'research' });
  if (['png', 'jpg', 'jpeg', 'gif', 'webp', 'svg'].includes(ext)) box.append(el('img', { src: url, alt: n.title }));
  else if (ext === 'pdf') box.append(el('iframe', { src: url }));
  else box.append(el('div', { class: 'muted' }, 'No preview for this file'));
  box.append(el('div', {}, el('button', { onclick: () => motu.invoke('open-file', { path: `${S.root}/${n.file}` }) }, 'Open in its application'),
    ' ', el('button', { onclick: () => motu.invoke('reveal', { path: `${S.root}/${n.file}` }) }, 'Show in folder')));
  parent.append(box);
}

async function openSplit(id) { S.split = id; await renderMain(); }

/* ---------- corkboard and outliner ---------- */

async function corkboard(parent, folder) {
  if (!folder) return;
  const grid = el('div', { class: 'cork' });
  parent.append(grid);
  for (const n of kids(folder.id)) {
    const d = n.kind === 'doc' ? await loadDoc(n.id) : null;
    const syn = el('textarea', { placeholder: 'Synopsis', oninput: (e) => { d.meta.synopsis = e.target.value; schedule(n.id); } });
    syn.value = d ? d.meta.synopsis : '';
    if (!d) syn.disabled = true;
    const card = el('div', { class: `card label-${d ? d.meta.label : 'none'}`, draggable: 'true', 'data-id': n.id,
      ondblclick: () => { S.view = 'editor'; syncViewButtons(); select(n.id); } },
      el('div', { class: 't' }, n.title || 'Untitled'), syn,
      el('div', { class: 'foot' }, el('span', {}, d ? d.meta.status : n.kind), el('span', {}, d ? `${S.words[n.id] ?? words(d.body)} w` : '')));
    card.addEventListener('dragstart', (e) => { dragId = n.id; card.classList.add('dragging'); e.dataTransfer.setData('text/plain', n.id); });
    card.addEventListener('dragend', () => { card.classList.remove('dragging'); dragId = null; });
    card.addEventListener('dragover', (e) => { if (dragId && dragId !== n.id) e.preventDefault(); });
    card.addEventListener('drop', async (e) => {
      e.preventDefault();
      const id = dragId; if (!id || id === n.id) return;
      const siblings = kids(folder.id).filter((x) => x.id !== id);
      const index = siblings.findIndex((x) => x.id === n.id);
      S.book = await call('move', { id, parent: folder.id, index });
      renderAll();
    });
    grid.append(card);
  }
  if (!grid.children.length) parent.append(el('div', { class: 'empty' }, 'No cards here'));
}

async function outliner(parent, folder) {
  if (!folder) return;
  const rows = [];
  for (const n of descendants(folder.id)) {
    const d = n.kind === 'doc' ? await loadDoc(n.id) : null;
    const w = d ? (S.words[n.id] ?? words(d.body)) : '';
    const t = d ? d.meta.target : 0;
    rows.push(el('tr', { ondblclick: () => { S.view = 'editor'; syncViewButtons(); select(n.id); } },
      el('td', {}, ' '.repeat(2 * (ancestors(n.id).length - ancestors(folder.id).length - 1)) + (n.title || 'Untitled')),
      el('td', { class: 'syn' }, d ? d.meta.synopsis : ''),
      el('td', {}, d ? d.meta.label : ''), el('td', {}, d ? d.meta.status : ''),
      el('td', {}, String(w)),
      el('td', {}, t ? el('progress', { value: Math.min(w, t), max: t }) : '')));
  }
  parent.append(el('table', { class: 'outline' },
    el('thead', {}, el('tr', {}, ...['Title', 'Synopsis', 'Label', 'Status', 'Words', 'Target'].map((h) => el('th', {}, h)))),
    el('tbody', {}, ...rows)));
}

/* ---------- inspector ---------- */

async function renderInspector() {
  const box = $('#inspector');
  const n = node(S.sel);
  if (!n || n.kind !== 'doc') { box.replaceChildren(el('div', { class: 'empty' }, n ? (n.kind === 'research' ? 'A research file' : 'A folder') : 'Select a document')); return; }
  const d = await loadDoc(n.id);
  if (S.sel !== n.id) return;
  const field = (label, input) => el('label', {}, label, input);
  const select_ = (opts, value, on) => {
    const s = el('select', { onchange: (e) => on(e.target.value) }, ...opts.map((o) => el('option', { value: o }, o)));
    s.value = value || opts[0]; return s;
  };
  const touch = () => schedule(n.id);
  const syn = el('textarea', { oninput: (e) => { d.meta.synopsis = e.target.value; touch(); } }); syn.value = d.meta.synopsis;
  const notes = el('textarea', { id: 'notes', oninput: (e) => { d.notes = e.target.value; schedule(n.id, 'notes'); } }); notes.value = d.notes;
  const kw = el('input', { value: d.meta.keywords.join(', '), oninput: (e) => { d.meta.keywords = e.target.value.split(',').map((x) => x.trim()).filter(Boolean); touch(); } });
  const target = el('input', { type: 'number', min: 0, step: 100, value: d.meta.target || '', oninput: (e) => { d.meta.target = Number(e.target.value) || 0; touch(); } });
  const snaps = el('ul', { id: 'snapshots' });
  box.replaceChildren(
    field('Synopsis', syn),
    el('div', { class: 'row' },
      field('Label', select_(S.book.labels, d.meta.label, (v) => { d.meta.label = v; touch(); })),
      field('Status', select_(S.book.statuses, d.meta.status, (v) => { d.meta.status = v; touch(); }))),
    field('Keywords', kw),
    field('Target (words)', target),
    field('Notes', notes),
    el('div', {}, el('div', { class: 'row' }, el('strong', {}, 'Snapshots'), el('button', { onclick: takeSnapshot }, 'Take snapshot')), snaps),
    el('div', {}, el('button', { onclick: () => openSplit(n.id) }, 'Open in split')));
  renderSnapshots(n.id, snaps);
}

async function renderSnapshots(id, list) {
  const names = await call('snapshots', { id });
  list.replaceChildren(...names.map((name) => el('li', {}, el('span', {}, name.replace('T', ' ').replace(/-(\d\d)-(\d\d)Z/, ':$1:$2')),
    el('button', { onclick: () => viewSnapshot(id, name) }, 'View'))));
  if (!names.length) list.append(el('li', { class: 'muted' }, 'None yet'));
}

async function takeSnapshot() {
  const n = node(S.sel); if (!n || n.kind !== 'doc') return;
  await flushAll();
  await call('snapshot', { id: n.id });
  renderSnapshots(n.id, $('#snapshots'));
  status('Snapshot taken');
}

async function viewSnapshot(id, name) {
  const body = await call('snapshot-body', { id, name });
  $('#sn-title').textContent = `Snapshot ${name}`;
  $('#sn-body').textContent = body;
  const dlg = $('#d-snapshot');
  $('#sn-close').onclick = () => dlg.close();
  $('#sn-restore').onclick = async () => {
    await flushAll();
    await call('restore', { id, name });
    S.docs.delete(id);
    dlg.close();
    renderAll();
  };
  dlg.showModal();
}

/* ---------- targets, search, composition ---------- */

async function refreshStats() {
  const st = await call('stats');
  S.words = st.words; S.manuscript = st.manuscript;
  refreshTargets();
}

function refreshTargets() {
  const t = $('#targets'); if (!t || !S.book) return;
  const manuscriptIds = new Set(docsUnder(MANUSCRIPT).map((n) => n.id));
  S.manuscript = Object.entries(S.words).filter(([id]) => manuscriptIds.has(id)).reduce((a, [, w]) => a + w, 0);
  const session = Math.max(0, S.manuscript - (S.sessionStart ?? S.manuscript));
  const goal = S.book.target, sgoal = sessionTarget();
  t.replaceChildren(
    `${S.manuscript.toLocaleString()} words`,
    goal ? el('span', {}, ' ', el('progress', { value: Math.min(S.manuscript, goal), max: goal }), ` of ${goal.toLocaleString()}`) : '',
    ` · session ${session.toLocaleString()}/${sgoal.toLocaleString()}`,
    t.dataset.status ? ` · ${t.dataset.status}` : '');
  const cb = $('#cb-words'); if (cb) cb.textContent = `${S.manuscript.toLocaleString()} words · session ${session}`;
}

let searchTimer = null;
function onSearch() {
  clearTimeout(searchTimer);
  searchTimer = setTimeout(async () => {
    const q = $('#t-search').value.trim();
    if (q.length < 2) { S.hits = null; renderBinder(); return; }
    await flushAll();
    S.hits = await call('search', { query: q });
    renderBinder();
  }, 250);
}

async function setComposing(on) {
  document.body.classList.toggle('composing', on);
  await motu.invoke('fullscreen', { on });
  const n = node(S.sel);
  if (on && n && isFolder(n) && !S.scrivenings) S.scrivenings = true;
  renderMain();
}

/* ---------- compile ---------- */

function openCompile() {
  const dlg = $('#d-compile');
  $('#c-status').textContent = ''; $('#compile-progress').classList.add('hidden');
  $('#c-cancel').onclick = () => dlg.close();
  $('#c-go').onclick = async () => {
    await flushAll();
    const format = $('#c-format').value;
    const bar = $('#compile-progress'); bar.classList.remove('hidden'); bar.value = 0;
    $('#c-status').textContent = 'Compiling…';
    try {
      const out = await call('compile', { format, titles: $('#c-titles').checked, title_page: $('#c-title-page').checked });
      $('#c-status').textContent = out ? `Wrote ${out}` : 'Cancelled';
      if (out) motu.invoke('reveal', { path: out });
    } catch (e) { $('#c-status').textContent = `Failed: ${e.message}`; }
  };
  dlg.showModal();
}
motu.on('compile-progress', (d) => { const bar = $('#compile-progress'); bar.max = d.total; bar.value = d.done; });

/* ---------- settings ---------- */

function openSettings() {
  const dlg = $('#d-settings'), b = S.book;
  $('#s-title').value = b.title; $('#s-author').value = b.author; $('#s-language').value = b.language;
  $('#s-target').value = b.target || ''; $('#s-session').value = sessionTarget();
  $('#s-cancel').onclick = () => dlg.close();
  $('#s-save').onclick = async () => {
    localStorage.setItem(sessionKey(), String(Number($('#s-session').value) || 0));
    S.book = await call('save-book', { book: { title: $('#s-title').value, author: $('#s-author').value, language: $('#s-language').value, target: Number($('#s-target').value) || 0 } });
    dlg.close(); renderAll();
  };
  dlg.showModal();
}

/* ---------- commands from menus, toolbar and keys ---------- */

function syncViewButtons() {
  $$('[data-view]').forEach((b) => b.classList.toggle('on', b.dataset.view === S.view));
  $('#t-scrivenings').classList.toggle('on', S.scrivenings);
  $('#t-split').classList.toggle('on', !!S.split);
  $('#t-inspector').classList.toggle('on', S.inspector);
}

const ACTIONS = {
  'new-project': () => openProject(() => motu.invoke('pick-create', {})),
  'open-project': () => openProject(() => motu.invoke('pick-open', {})),
  'new-doc': () => S.root && addNode('doc'),
  'new-folder': () => S.root && addNode('folder'),
  'snapshot': () => S.root && takeSnapshot(),
  'compile': () => S.root && openCompile(),
  'view-editor': () => { S.view = 'editor'; syncViewButtons(); renderMain(); },
  'view-corkboard': () => { S.view = 'corkboard'; syncViewButtons(); renderMain(); },
  'view-outliner': () => { S.view = 'outliner'; syncViewButtons(); renderMain(); },
  'scrivenings': () => { S.scrivenings = !S.scrivenings; syncViewButtons(); renderMain(); },
  'split': () => { S.split = S.split ? null : S.sel; syncViewButtons(); renderMain(); },
  'composition': () => S.root && setComposing(!document.body.classList.contains('composing')),
  'find': () => $('#t-search').focus(),
  'inspector': () => { S.inspector = !S.inspector; $('#body').classList.toggle('no-inspector', !S.inspector); syncViewButtons(); },
  'targets': () => S.root && openSettings(),
  'book-settings': () => S.root && openSettings(),
  'empty-trash': async () => { if (!S.root) return; S.book = await call('empty-trash'); renderAll(); },
  'close-window': () => closeSafely(),
};

motu.on('menu', (id) => (ACTIONS[id] || (() => {}))());
motu.on('close-requested', () => closeSafely());
motu.on('files-dropped', async (paths) => {
  if (!S.root) return;
  S.book = await call('import', { paths });
  S.expanded.add(RESEARCH);
  renderAll();
});

async function closeSafely() {
  await flushAll();
  await motu.invoke('close-window');
}

$('#w-new').onclick = ACTIONS['new-project'];
$('#w-open').onclick = ACTIONS['open-project'];
$$('[data-view]').forEach((b) => (b.onclick = ACTIONS[`view-${b.dataset.view}`]));
$('#t-scrivenings').onclick = ACTIONS.scrivenings;
$('#t-split').onclick = ACTIONS.split;
$('#t-compose').onclick = ACTIONS.composition;
$('#t-inspector').onclick = ACTIONS.inspector;
$('#t-compile').onclick = ACTIONS.compile;
$('#t-search').oninput = onSearch;
$('#b-doc').onclick = ACTIONS['new-doc'];
$('#b-folder').onclick = ACTIONS['new-folder'];
$('#b-trash').onclick = trashSelected;
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape' && document.body.classList.contains('composing')) setComposing(false);
});

/* ---------- start, or run the self-test ---------- */

if (location.hash.startsWith('#selftest=')) selftest(decodeURIComponent(location.hash.slice(10)));
else if (location.hash.startsWith('#open=')) openProject(() => motu.invoke('open', { path: decodeURIComponent(location.hash.slice(6)) }));
else if (location.hash.startsWith('#shots=')) shots(decodeURIComponent(location.hash.slice(7)));
else showWelcome();

async function selftest(dir) {
  const out = [];
  const check = (name, ok, detail = '') => out.push(`${ok ? 'ok' : 'FAIL'} ${name}${detail ? ` (${detail})` : ''}`);
  window.onerror = (m) => motu.invoke('motu.report', `JS ERROR ${m}`);
  try {
    await openProject(() => motu.invoke('create', { path: `${dir}/Night Ferry`, title: 'Night Ferry', author: 'Ana' }));
    check('create and open', S.root.endsWith('Night Ferry.book') && docsUnder(MANUSCRIPT).length === 1);
    const first = docsUnder(MANUSCRIPT)[0].id;
    await select(first);
    S.editors[0].value = 'The ferry came in after dark.[^1] The lights failed.\n\n[^1]: The last ferry.';
    const cached = S.docs.get(first).body;
    await flushAll();
    const saved = (await call('read', { id: first })).body;
    check('editor saves through autosave', saved.startsWith('The ferry came in'), `editors=${S.editors.length} cached=${JSON.stringify(cached.slice(0, 20))} saved=${JSON.stringify(saved.slice(0, 20))} timers=${S.timers.size}`);
    const d = S.docs.get(first); d.meta.synopsis = 'Ana arrives.'; d.meta.label = 'draft'; schedule(first); await flushAll();
    check('inspector metadata saves', (await call('read', { id: first })).meta.label === 'draft');
    S.book = await call('rename', { id: first, title: 'The Harbour' });
    check('rename', node(first).title === 'The Harbour');
    const r = await call('add', { parent: MANUSCRIPT, kind: 'doc', title: 'Dawn', index: 1e6 }); S.book = r.book;
    S.docs.set(r.id, { meta: (await call('read', { id: r.id })).meta, body: 'The lights come back at dawn.', notes: 'Check the tide tables.' });
    schedule(r.id); schedule(r.id, 'notes'); await flushAll();
    S.book = await call('move', { id: r.id, parent: MANUSCRIPT, index: 0 });
    check('move reorders', docsUnder(MANUSCRIPT)[0].id === r.id);
    const hits = await call('search', { query: 'lights' });
    check('search', hits.length === 2, `${hits.length} first=${JSON.stringify((await call('read', { id: first })).body.slice(0, 30))} dawn=${JSON.stringify((await call('read', { id: r.id })).body.slice(0, 30))}`);
    const tide = await call('search', { query: 'tide' });
    check('search finds notes', tide.length === 1 && tide[0] === r.id);
    await refreshStats();
    check('word counts', S.manuscript === 18, S.manuscript);
    await call('snapshot', { id: first });
    const before = (await call('snapshots', { id: first }))[0];
    S.docs.get(first).body = 'A different text.'; schedule(first); await flushAll();
    await call('restore', { id: first, name: before }); S.docs.delete(first);
    check('snapshot restore', (await call('read', { id: first })).body.startsWith('The ferry came in'));
    S.view = 'corkboard'; await select(MANUSCRIPT);
    check('corkboard', $$('.card').length === 2, $$('.card').length);
    S.view = 'outliner'; await renderMain();
    check('outliner', $$('table.outline tbody tr').length === 2);
    S.view = 'editor'; S.scrivenings = true; await select(first);
    check('scrivenings', $$('.cm-editor').length === 2, $$('.cm-editor').length);
    await motu.invoke('motu.snapshot', { out: `${dir}/shot.png` });
    S.book = await call('import', { paths: [`${dir}/shot.png`] });
    const pic = kids(RESEARCH).find((n) => n.kind === 'research');
    check('import research', !!pic && pic.file.startsWith('research/'));
    if (pic) {
      S.view = 'editor'; await select(pic.id);
      const img = $('.research img');
      if (img && !img.complete) await new Promise((res) => { img.onload = img.onerror = res; });
      check('research image loads', !!img && img.naturalWidth > 0, img ? img.naturalWidth : 'no img');
    }
    for (const format of ['md', 'html', 'epub', 'docx', 'pdf']) {
      try {
        const path = await call('compile', { format, titles: true, title_page: true, out: `${dir}/out.${format}` });
        check(`compile ${format}`, typeof path === 'string', path);
      } catch (e) { check(`compile ${format}`, false, e.message); }
    }
  } catch (e) {
    check('self-test', false, e.message);
  }
  await motu.invoke('motu.report', out.join('\n'));
}

async function shots(arg) {
  window.onerror = (m) => motu.invoke('motu.report', `JS ERROR ${m}`);
  window.onunhandledrejection = (e) => motu.invoke('motu.report', `REJECTED ${e.reason}`);
  const [path, dir] = arg.split('|');
  const pause = () => new Promise((r) => setTimeout(r, 400));
  await openProject(() => motu.invoke('open', { path }));
  const shot = async (name) => { await pause(); await motu.invoke('motu.snapshot', { out: `${dir}/${name}.png` }); };
  S.expanded.add('p1'); S.expanded.add('p2');
  await select('c1'); await shot('editor');
  S.view = 'corkboard'; syncViewButtons(); await select('p1'); await shot('corkboard');
  S.view = 'outliner'; syncViewButtons(); await select(MANUSCRIPT); await shot('outliner');
  S.view = 'editor'; syncViewButtons(); S.split = 'c2'; await select('c1'); await shot('split');
  S.split = null; document.body.classList.add('composing'); await renderMain(); await shot('composition');
  document.body.classList.remove('composing');
  await motu.invoke('motu.report', 'shots done');
}
