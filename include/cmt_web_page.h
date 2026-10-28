#pragma once

// ============================================================================
// Webファイルマネージャの画面 (cmt_web.cpp から "/" で返す)
//   API:
//     GET  /api/list?path=<フォルダ>
//     GET  /api/download?path=<ファイル>
//     POST /api/upload?dir=<フォルダ>        (multipart、1リクエストに1ファイル)
//     POST /api/delete?path=<ファイル/空のフォルダ>
//     POST /api/rename?path=<対象>&name=<新しい名前>
//     POST /api/mkdir?path=<親フォルダ>&name=<フォルダ名>
//   まとめてのアップロード・ダウンロード・削除は、1つずつ順番に行う
// ============================================================================

static const char WEB_INDEX_HTML[] = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PicoCmt</title>
<style>
body{font-family:sans-serif;margin:0;background:#f4f5f7;color:#222}
header{background:#2b4a6f;color:#fff;padding:10px 16px;font-size:18px}
main{max-width:860px;margin:0 auto;padding:8px 16px 24px}
#path{font-family:monospace;margin:8px 0;word-break:break-all}
#path a{color:#2b4a6f;text-decoration:none}
.bar{display:flex;flex-wrap:wrap;gap:6px;margin:8px 0}
button{padding:6px 12px;border:1px solid #9aa4ad;border-radius:4px;background:#fff;cursor:pointer;font-size:14px}
button:disabled{opacity:.45;cursor:default}
#drop{border:2px dashed #9aa4ad;border-radius:6px;padding:12px;text-align:center;color:#667;margin:8px 0}
#drop.over{background:#e6eef8;border-color:#2b4a6f}
#status{font-family:monospace;white-space:pre-wrap;font-size:13px;min-height:1.3em;margin:8px 0}
#status.err{color:#b00020}
table{width:100%;border-collapse:collapse;background:#fff}
th,td{padding:6px 8px;border-bottom:1px solid #e3e5e8;text-align:left}
th.chk,td.chk{width:28px}
td.size,td.time{text-align:right;white-space:nowrap;font-family:monospace}
td.name{word-break:break-all}
td.name a{color:#2b4a6f;text-decoration:none}
td.name a.dir{font-weight:bold}
</style></head><body>
<header>PicoCmt</header>
<main>
<div id="path"></div>
<div class="bar">
<button id="btnUp">Upload</button>
<button id="btnDown" disabled>Download</button>
<button id="btnRen" disabled>Rename</button>
<button id="btnDel" disabled>Delete</button>
<button id="btnDir">New Folder</button>
<button id="btnRef">Refresh</button>
<input type="file" id="file" multiple hidden>
</div>
<div id="drop">Drop files here to upload</div>
<div id="status"></div>
<table><thead><tr><th class="chk"><input type="checkbox" id="all"></th><th>Name</th><th>Size</th><th>Modified</th></tr></thead>
<tbody id="list"></tbody></table>
</main>
<script>
let cwd = '/', entries = [], busy = false;
const $ = id => document.getElementById(id);
const join = (d, n) => d === '/' ? '/' + n : d + '/' + n;
const parent = p => p.substring(0, p.lastIndexOf('/')) || '/';
const q = s => encodeURIComponent(s);

function setStatus(text, isError) {
  const s = $('status');
  s.textContent = text;
  s.className = isError ? 'err' : '';
}

function fmtSize(n) {
  if (n < 1024) return n + ' B';
  if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1048576).toFixed(1) + ' MB';
}

// 更新日時。本体の時刻が合っていない間に書いたファイル (2000-01-01 など) は表示しない
function fmtTime(t) {
  if (!t || t < 978307200) return '-';
  const d = new Date(t * 1000);
  const p = n => String(n).padStart(2, '0');
  return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' +
         p(d.getHours()) + ':' + p(d.getMinutes());
}

function selected() {
  return [...document.querySelectorAll('.sel:checked')].map(c => entries[+c.dataset.i]);
}

function updateButtons() {
  const s = selected();
  $('btnDown').disabled = busy || !s.length || s.some(e => e.dir);
  $('btnDel').disabled = busy || !s.length;
  $('btnRen').disabled = busy || s.length !== 1;
  for (const id of ['btnUp', 'btnDir', 'btnRef', 'all']) $(id).disabled = busy;
}

async function api(method, url) {
  const r = await fetch(url, {method: method, cache: 'no-store'});
  if (!r.ok) throw new Error((await r.text()) || r.statusText);
  return r;
}

async function load(path) {
  try {
    const j = await (await api('GET', '/api/list?path=' + q(path))).json();
    cwd = j.path;
    entries = j.entries.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name));
    render();
  } catch (e) {
    setStatus('Error: ' + e.message, true);
  }
}

function link(label, cls, onclick) {
  const a = document.createElement('a');
  a.href = '#';
  a.textContent = label;
  if (cls) a.className = cls;
  a.onclick = ev => { ev.preventDefault(); if (!busy) onclick(); };
  return a;
}

function render() {
  const p = $('path');
  p.textContent = '';
  p.appendChild(link('/', '', () => load('/')));
  let acc = '';
  cwd.split('/').filter(x => x).forEach((name, i, all) => {
    acc += '/' + name;
    const target = acc;
    p.appendChild(link(name, '', () => load(target)));
    if (i < all.length - 1) p.appendChild(document.createTextNode('/'));
  });

  const tb = $('list');
  tb.textContent = '';
  const row = (chk, nameCell, size, time) => {
    const tr = document.createElement('tr');
    const c0 = document.createElement('td'); c0.className = 'chk'; if (chk) c0.appendChild(chk);
    const c1 = document.createElement('td'); c1.className = 'name'; c1.appendChild(nameCell);
    const c2 = document.createElement('td'); c2.className = 'size'; c2.textContent = size;
    const c3 = document.createElement('td'); c3.className = 'time'; c3.textContent = time;
    tr.append(c0, c1, c2, c3);
    tb.appendChild(tr);
  };
  if (cwd !== '/') row(null, link('..', 'dir', () => load(parent(cwd))), '', '');
  entries.forEach((e, i) => {
    const chk = document.createElement('input');
    chk.type = 'checkbox'; chk.className = 'sel'; chk.dataset.i = i;
    chk.onchange = updateButtons;
    const name = e.dir
      ? link(e.name + '/', 'dir', () => load(join(cwd, e.name)))
      : link(e.name, '', () => runSequential('Downloading', [e], downloadOne, false));
    row(chk, name, e.dir ? '' : fmtSize(e.size), fmtTime(e.mtime));
  });
  $('all').checked = false;
  updateButtons();
}

// items を1つずつ順番に処理する
async function runSequential(label, items, fn, reload) {
  if (busy || !items.length) return;
  busy = true;
  updateButtons();
  const errors = [];
  for (let i = 0; i < items.length; i++) {
    setStatus(label + ' (' + (i + 1) + '/' + items.length + ') ' + items[i].name);
    try {
      await fn(items[i], i, items.length);
    } catch (e) {
      errors.push(items[i].name + ': ' + e.message);
    }
  }
  busy = false;
  if (reload) await load(cwd); else updateButtons();
  const done = label + ': ' + (items.length - errors.length) + '/' + items.length + ' done';
  setStatus(errors.length ? done + '\n' + errors.join('\n') : done, errors.length > 0);
}

function uploadOne(file, i, total) {
  return new Promise((resolve, reject) => {
    const x = new XMLHttpRequest();
    x.open('POST', '/api/upload?dir=' + q(cwd));
    x.upload.onprogress = ev => {
      if (ev.lengthComputable) {
        setStatus('Uploading (' + (i + 1) + '/' + total + ') ' + file.name + ' ' +
                  Math.floor(ev.loaded * 100 / ev.total) + '%');
      }
    };
    x.onload = () => x.status === 200 ? resolve() : reject(new Error(x.responseText || x.statusText));
    x.onerror = () => reject(new Error('Network error'));
    const form = new FormData();
    form.append('file', file, file.name);
    x.send(form);
  });
}

async function downloadOne(e) {
  const r = await api('GET', '/api/download?path=' + q(join(cwd, e.name)));
  const url = URL.createObjectURL(await r.blob());
  const a = document.createElement('a');
  a.href = url;
  a.download = e.name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}

function uploadFiles(fileList) {
  const files = [...fileList];
  if (!files.length || busy) return;
  const dup = files.filter(f => entries.some(e => e.name === f.name));
  if (dup.length && !confirm('Overwrite existing files?\n' + dup.map(f => f.name).join('\n'))) return;
  runSequential('Uploading', files, uploadOne, true);
}

$('btnUp').onclick = () => $('file').click();
$('file').onchange = () => { uploadFiles($('file').files); $('file').value = ''; };
$('btnRef').onclick = () => load(cwd);
$('btnDown').onclick = () => runSequential('Downloading', selected(), downloadOne, false);
$('btnDel').onclick = () => {
  const s = selected();
  if (!confirm('Delete these items?\n' + s.map(e => e.name + (e.dir ? '/' : '')).join('\n'))) return;
  runSequential('Deleting', s, e => api('POST', '/api/delete?path=' + q(join(cwd, e.name))), true);
};
$('btnRen').onclick = () => {
  const e = selected()[0];
  const name = prompt('New name', e.name);
  if (!name || name === e.name) return;
  runSequential('Renaming', [e], () =>
    api('POST', '/api/rename?path=' + q(join(cwd, e.name)) + '&name=' + q(name)), true);
};
$('btnDir').onclick = () => {
  const name = prompt('Folder name');
  if (!name) return;
  runSequential('Creating', [{name: name}], () =>
    api('POST', '/api/mkdir?path=' + q(cwd) + '&name=' + q(name)), true);
};
$('all').onchange = () => {
  document.querySelectorAll('.sel').forEach(c => c.checked = $('all').checked);
  updateButtons();
};

const drop = $('drop');
drop.ondragover = ev => { ev.preventDefault(); drop.classList.add('over'); };
drop.ondragleave = () => drop.classList.remove('over');
drop.ondrop = ev => { ev.preventDefault(); drop.classList.remove('over'); uploadFiles(ev.dataTransfer.files); };
document.ondragover = ev => ev.preventDefault();
document.ondrop = ev => ev.preventDefault();

load('/');
</script>
</body></html>
)rawliteral";
