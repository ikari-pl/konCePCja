/* konCePCja - Amstrad CPC Emulator
   M4 Board — Embedded web assets

   Single-page app matching the real M4 Board's four pages:
   Files, Roms, Control, Settings — plus the same REST endpoints.

   Compatible with: cpcxfer, M4 Board Android app, web browsers.
*/

#pragma once

#include <cstddef>
#include <cstdint>

// ── index.html ──────────────────────────────────────────

static const char m4_web_index_html_src[] = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>konCePCja M4 Board</title>
<link rel="stylesheet" href="/stylesheet.css">
</head>
<body>
<div class="topbar">
  <h1>konCePCja M4 Board</h1>
  <nav>
    <a href="#" onclick="showPage('files')" class="btn" id="nav-files">Files</a>
    <a href="#" onclick="showPage('roms')" class="btn" id="nav-roms">Roms</a>
    <a href="#" onclick="showPage('control')" class="btn" id="nav-control">Control</a>
    <a href="#" onclick="showPage('settings')" class="btn" id="nav-settings">Settings</a>
  </nav>
  <span id="status-led" class="led led-off"></span>
</div>
<div id="toast-container"></div>

<!-- ═══ FILES PAGE ═══ -->
<div id="page-files" class="page">
<div class="container">
  <div class="toolbar">
    <span id="current-dir">/</span>
    <div class="toolbar-right">
      <button onclick="doCdOnCpc()" class="btn btn-sm btn-accent" title="Set CPC working directory to match">CD on CPC</button>
      <button onclick="goBack()" class="btn btn-sm">Back</button>
      <button onclick="refresh()" class="btn btn-sm">Refresh</button>
      <button onclick="showMkdir()" class="btn btn-sm">New Folder</button>
    </div>
  </div>

  <div id="mkdir-form" style="display:none" class="inline-form">
    <input type="text" id="mkdir-name" placeholder="Folder name"
      onkeydown="if(event.key==='Enter')doMkdir()">
    <button onclick="doMkdir()" class="btn btn-sm">Create</button>
    <button onclick="hideMkdir()" class="btn btn-sm">Cancel</button>
  </div>

  <div id="file-list" class="file-list">
    <div class="loading">Loading...</div>
  </div>

  <div id="upload-area" class="upload-area">
    <p>Drop files here to upload, or <label for="file-input" class="link">browse</label></p>
    <input type="file" id="file-input" multiple style="display:none" onchange="uploadFiles(this.files)">
    <div id="upload-status"></div>
  </div>
</div>
</div>

<!-- ═══ ROMS PAGE ═══ -->
<div id="page-roms" class="page" style="display:none">
<div class="container">
  <div class="section">
    <h2>ROM Slots</h2>
    <table class="rom-table" id="rom-table">
      <thead><tr><th>Slot</th><th>Status</th><th>Identified As</th><th>File</th></tr></thead>
      <tbody id="rom-tbody"><tr><td colspan="4">Loading...</td></tr></tbody>
    </table>
  </div>
</div>
</div>

<!-- ═══ CONTROL PAGE ═══ -->
<div id="page-control" class="page" style="display:none">
<div class="container">
  <div class="section">
    <h2>CPC Control</h2>
    <div class="control-grid">
      <button onclick="ctrlAction('cres')" class="btn btn-lg">CPC Reset</button>
      <button onclick="ctrlAction('chlt')" class="btn btn-lg">Pause / Resume</button>
      <button onclick="ctrlAction('mres')" class="btn btn-lg">M4 Reset</button>
      <button onclick="ctrlAction('cnmi')" class="btn btn-lg">Hack Menu (NMI)</button>
    </div>
  </div>

  <div class="section">
    <h2>Remote Run</h2>
    <div class="inline-form">
      <input type="text" id="run-cmd" placeholder='e.g. run"game' onkeydown="if(event.key==='Enter')doRunCmd()">
      <button onclick="doRunCmd()" class="btn">Run</button>
    </div>
  </div>

  <div class="section">
    <h2>Change Directory (on CPC)</h2>
    <div class="inline-form">
      <input type="text" id="cd-path" placeholder="e.g. /games/" onkeydown="if(event.key==='Enter')doCdCmd()">
      <button onclick="doCdCmd()" class="btn">CD</button>
    </div>
  </div>
</div>
</div>

<!-- ═══ SETTINGS PAGE ═══ -->
<div id="page-settings" class="page" style="display:none">
<div class="container">
  <div class="section">
    <h2>Live Preview</h2>
    <div class="preview-container">
      <img id="preview-img" alt="CPC Screen" class="preview-img">
    </div>
    <div style="margin-top:8px">
      <label><input type="checkbox" id="preview-toggle" onchange="togglePreview(this.checked)"> Auto-refresh (~5 fps)</label>
    </div>
  </div>

  <div class="section">
    <h2>Status</h2>
    <table class="info-table" id="status-table">
      <tr><td>Loading...</td></tr>
    </table>
  </div>

  <div class="section">
    <h2>About</h2>
    <table class="info-table">
      <tr><td>Emulator</td><td id="info-version">konCePCja</td></tr>
      <tr><td>HTTP Server</td><td id="info-http"></td></tr>
      <tr><td>Network</td><td>Emulated (host bridged)</td></tr>
    </table>
    <p class="note">WiFi/NTP/SSID settings are not applicable in emulation.<br>
    Network, ROM, and peripheral settings are managed via the emulator's Options dialog.</p>
  </div>
</div>
</div>

<script>
// ── Page navigation ──
var activePage = 'files';
function showPage(name) {
  document.querySelectorAll('.page').forEach(function(p) { p.style.display = 'none'; });
  document.querySelectorAll('.topbar .btn').forEach(function(b) { b.classList.remove('active'); });
  var el = document.getElementById('page-' + name);
  if (el) el.style.display = '';
  var nav = document.getElementById('nav-' + name);
  if (nav) nav.classList.add('active');
  activePage = name;
  if (name === 'settings') refreshStatus();
  if (name === 'roms') loadRoms();
}

// ── Files page ──
var currDir = '/';
var dirHistory = [];

function refresh() { loadDir(currDir); }

function goBack() {
  if (dirHistory.length > 0) {
    loadDir(dirHistory.pop(), true);
  } else if (currDir !== '/') {
    var parent = currDir.substring(0, currDir.lastIndexOf('/', currDir.length - 2));
    if (!parent) parent = '/';
    else parent += '/';
    loadDir(parent);
  }
}

function doCdOnCpc() {
  fetch('/config.cgi?cd=' + encodeURIComponent(currDir))
    .then(function() { showToast('CPC directory set to ' + currDir); });
}

function loadDir(path, skipHistory) {
  if (!skipHistory && path !== currDir) dirHistory.push(currDir);
  var xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function() {
    if (this.readyState !== 4) return;
    if (this.status === 200) {
      var lines = this.responseText.split('\n');
      currDir = lines[0];
      lines.splice(0, 1);
      document.getElementById('current-dir').textContent = currDir;
      renderFiles(lines.filter(function(l) { return l.length > 0; }));
    }
  };
  xhr.open('GET', '/config.cgi?ls=' + encodeURIComponent(path), true);
  xhr.send();
}

function renderFiles(lines) {
  var dirs = [], dsks = [], files = [];
  for (var i = 0; i < lines.length; i++) {
    var parts = lines[i].split(',');
    if (parts.length < 2) continue;
    var entry = { name: parts[0], type: parseInt(parts[1]), size: parseInt(parts[2] || '0') };
    if (entry.type === 0) dirs.push(entry);
    else if (entry.name.match(/\.(dsk|cpr)$/i)) dsks.push(entry);
    else files.push(entry);
  }
  dirs.sort(function(a,b) { return a.name.localeCompare(b.name); });
  dsks.sort(function(a,b) { return a.name.localeCompare(b.name); });
  files.sort(function(a,b) { return a.name.localeCompare(b.name); });

  var html = '<table><thead><tr><th>Name</th><th>Size</th><th></th><th></th></tr></thead><tbody>';

  for (var d = 0; d < dirs.length; d++) {
    var dpath = currDir + dirs[d].name;
    html += '<tr class="dir"><td><a href="#" onclick="loadDir(\'' + escAttr(dpath) + '/\');return false">'
          + esc(dirs[d].name) + '/</a></td><td></td><td></td>'
          + '<td><a href="#" onclick="doDelete(\'' + escAttr(dpath) + '\');return false" class="del">Del</a></td></tr>';
  }
  for (var k = 0; k < dsks.length; k++) {
    var kpath = currDir + dsks[k].name;
    html += '<tr class="dsk"><td><a href="#" onclick="loadDir(\'' + escAttr(kpath) + '/\');return false">'
          + esc(dsks[k].name) + '</a></td><td>' + fmtSize(dsks[k].size) + '</td>'
          + '<td><a href="#" onclick="doRun(\'' + escAttr(kpath) + '\');return false" class="run">Run</a></td>'
          + '<td><a href="#" onclick="doDelete(\'' + escAttr(kpath) + '\');return false" class="del">Del</a></td></tr>';
  }
  for (var f = 0; f < files.length; f++) {
    var fpath = currDir + files[f].name;
    html += '<tr><td><a href="/sd' + encURI(fpath) + '">' + esc(files[f].name) + '</a></td>'
          + '<td>' + fmtSize(files[f].size) + '</td>'
          + '<td><a href="#" onclick="doRun(\'' + escAttr(fpath) + '\');return false" class="run">Run</a></td>'
          + '<td><a href="#" onclick="doDelete(\'' + escAttr(fpath) + '\');return false" class="del">Del</a></td></tr>';
  }
  html += '</tbody></table>';
  document.getElementById('file-list').innerHTML = html;
}

function doRun(path) {
  fetch('/config.cgi?run2=' + encodeURIComponent(path));
}
function doDelete(path) {
  if (!confirm('Delete ' + path + '?')) return;
  fetch('/config.cgi?rm=' + encodeURIComponent(path)).then(function() { refresh(); });
}
function showMkdir() { document.getElementById('mkdir-form').style.display = 'flex'; }
function hideMkdir() { document.getElementById('mkdir-form').style.display = 'none'; }
function doMkdir() {
  var name = document.getElementById('mkdir-name').value.trim();
  if (!name) return;
  fetch('/config.cgi?mkdir=' + encodeURIComponent(currDir + name)).then(function() {
    hideMkdir();
    document.getElementById('mkdir-name').value = '';
    refresh();
  });
}
function uploadFiles(files) {
  var status = document.getElementById('upload-status');
  for (var i = 0; i < files.length; i++) {
    var fd = new FormData();
    fd.append('file', files[i], currDir + files[i].name);
    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/', false);
    xhr.send(fd);
    status.innerHTML += '<p>' + esc(files[i].name) + ' (' + fmtSize(files[i].size) + ') uploaded</p>';
  }
  refresh();
}

// Drag and drop
var uploadArea = document.getElementById('upload-area');
uploadArea.ondragover = function(e) { e.preventDefault(); this.classList.add('hover'); };
uploadArea.ondragleave = function() { this.classList.remove('hover'); };
uploadArea.ondrop = function(e) {
  e.preventDefault(); this.classList.remove('hover');
  uploadFiles(e.dataTransfer.files);
};

// ── Control page ──
function ctrlAction(action) {
  fetch('/config.cgi?' + action + '=1').then(function(r) { return r.text(); }).then(function(t) {
    showToast(t);
  });
}
function showToast(msg) {
  var el = document.createElement('div');
  el.className = 'toast';
  el.textContent = msg;
  document.getElementById('toast-container').appendChild(el);
  setTimeout(function() { el.classList.add('show'); }, 10);
  setTimeout(function() {
    el.classList.remove('show');
    setTimeout(function() { el.remove(); }, 300);
  }, 3000);
}
function doRunCmd() {
  var cmd = document.getElementById('run-cmd').value.trim();
  if (cmd) fetch('/config.cgi?run=' + encodeURIComponent(cmd))
    .then(function(r) { return r.text(); }).then(function(t) { showToast(t); });
}
function doCdCmd() {
  var path = document.getElementById('cd-path').value.trim();
  if (path) fetch('/config.cgi?cd2=' + encodeURIComponent(path))
    .then(function(r) { return r.text(); }).then(function(t) { showToast(t); });
}

// ── Roms page ──
function loadRoms() {
  fetch('/roms.json').then(function(r) { return r.json(); }).then(function(slots) {
    var html = '';
    for (var i = 0; i < slots.length; i++) {
      var s = slots[i];
      var cls = s.loaded ? 'rom-loaded' : '';
      html += '<tr class="' + cls + '"><td>' + s.slot + '</td>'
            + '<td>' + (s.loaded ? '<span class="dot-on">Loaded</span>' : '<span class="dot-off">Empty</span>') + '</td>'
            + '<td>' + (s.name ? '<span class="rom-name">' + esc(s.name) + '</span>' : '') + '</td>'
            + '<td>' + (s.file ? esc(s.file) : '') + '</td></tr>';
    }
    document.getElementById('rom-tbody').innerHTML = html;
  }).catch(function(){});
}

// ── Live preview (WebSocket) ──
var previewWs = null;
var previewSized = false;
function togglePreview(on) {
  if (on) {
    if (previewWs) return; // already connected
    var proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    previewWs = new WebSocket(proto + '//' + location.host + '/ws/preview');
    previewWs.binaryType = 'blob';
    previewWs.onmessage = function(ev) {
      var img = document.getElementById('preview-img');
      var url = URL.createObjectURL(ev.data);
      img.onload = function() { URL.revokeObjectURL(url); };
      img.src = url;
      // Size at 50% of native resolution on first frame
      if (!previewSized) {
        fetch('/status').then(function(r) { return r.json(); }).then(function(s) {
          if (s.screen_w && s.screen_h) {
            img.style.width = Math.round(s.screen_w / 2) + 'px';
            img.style.height = Math.round(s.screen_h / 2) + 'px';
            previewSized = true;
          }
        }).catch(function(){});
      }
    };
    previewWs.onclose = function() {
      previewWs = null;
      document.getElementById('preview-toggle').checked = false;
    };
  } else {
    if (previewWs) { previewWs.close(); previewWs = null; }
  }
}

// ── Settings page ──
function refreshStatus() {
  fetch('/status').then(function(r) { return r.json(); }).then(function(s) {
    var html = '';
    html += '<tr><td>M4 Board</td><td>' + (s.enabled ? 'Enabled' : 'Disabled') + '</td></tr>';
    html += '<tr><td>SD Path</td><td>' + esc(s.sd_path || '(none)') + '</td></tr>';
    html += '<tr><td>Current Dir</td><td>' + esc(s.current_dir) + '</td></tr>';
    html += '<tr><td>Open Files</td><td>' + s.open_files + '/4</td></tr>';
    html += '<tr><td>Commands</td><td>' + s.cmd_count + '</td></tr>';
    html += '<tr><td>Network</td><td>' + (s.network ? 'On' : 'Off') + '</td></tr>';
    html += '<tr><td>CPC</td><td>' + (s.paused ? 'Paused' : 'Running') + '</td></tr>';
    document.getElementById('status-table').innerHTML = html;
    document.getElementById('info-http').textContent = s.bind_ip + ':' + s.http_port;
    document.getElementById('info-version').textContent = s.version;
  }).catch(function(){});
}

// ── Poll status LED ──
setInterval(function() {
  fetch('/status').then(function(r) { return r.json(); }).then(function(s) {
    var led = document.getElementById('status-led');
    led.className = s.enabled ? 'led led-on' : 'led led-off';
  }).catch(function(){});
}, 5000);

// ── Helpers ──
function esc(s) { var d = document.createElement('div'); d.textContent = s; return d.innerHTML; }
function escAttr(s) { return s.replace(/'/g, "\\'").replace(/"/g, '&quot;'); }
function encURI(s) { return encodeURI(s).replace(/#/g, '%23'); }
function fmtSize(b) {
  if (!b) return '';
  if (b < 1024) return b + ' B';
  if (b < 1048576) return (b/1024).toFixed(1) + ' KB';
  return (b/1048576).toFixed(1) + ' MB';
}

// ── Init ──
showPage('files');
loadDir('/');
</script>
</body>
</html>
)HTML";

static const uint8_t* m4_web_index_html =
   reinterpret_cast<const uint8_t*>(m4_web_index_html_src);
static const size_t m4_web_index_html_len = sizeof(m4_web_index_html_src) - 1;

// ── stylesheet.css ──────────────────────────────────────

static const char m4_web_stylesheet_css_src[] = R"CSS(
* { margin: 0; padding: 0; box-sizing: border-box; }
body {
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
  background: #1a1a2e; color: #e0e0e0; min-height: 100vh;
}
.topbar {
  background: #16213e; padding: 12px 20px; display: flex;
  align-items: center; gap: 20px; border-bottom: 2px solid #0f3460;
}
.topbar h1 { font-size: 18px; color: #e94560; white-space: nowrap; }
.topbar nav { display: flex; gap: 8px; }
.btn {
  display: inline-block; padding: 6px 14px; background: #0f3460;
  color: #e0e0e0; border: 1px solid #1a3a6e; border-radius: 4px;
  text-decoration: none; font-size: 13px; cursor: pointer;
}
.btn:hover { background: #1a4a7e; }
.btn.active { background: #e94560; border-color: #e94560; }
.btn-sm { padding: 4px 10px; font-size: 12px; }
.btn-lg { padding: 10px 24px; font-size: 15px; min-width: 160px; }
.btn-accent { background: #2e7d32; border-color: #388e3c; }
.btn-accent:hover { background: #388e3c; }
.led { width: 10px; height: 10px; border-radius: 50%; margin-left: auto; }
.led-on { background: #4eff4e; box-shadow: 0 0 6px #4eff4e; }
.led-off { background: #444; }
.container { max-width: 960px; margin: 20px auto; padding: 0 20px; }
.section { margin-bottom: 24px; }
.section h2 { font-size: 15px; color: #88c0d0; margin-bottom: 10px;
  border-bottom: 1px solid #0f3460; padding-bottom: 4px; }
.toolbar {
  display: flex; justify-content: space-between; align-items: center;
  margin-bottom: 12px; padding: 8px 12px; background: #16213e;
  border-radius: 4px; font-family: monospace; font-size: 14px; color: #88c0d0;
}
.toolbar-right { display: flex; gap: 6px; flex-wrap: wrap; }
.inline-form {
  display: flex; gap: 8px; margin-bottom: 12px; padding: 8px 12px;
  background: #16213e; border-radius: 4px; align-items: center;
}
.inline-form input[type="text"] {
  flex: 1; padding: 6px 10px; background: #0d1b2a; border: 1px solid #1a3a6e;
  color: #e0e0e0; border-radius: 3px; font-size: 14px;
}
.control-grid {
  display: grid; grid-template-columns: repeat(auto-fill, minmax(160px, 1fr));
  gap: 10px; margin-bottom: 16px;
}
.info-table { width: 100%; border-collapse: collapse; margin-bottom: 8px; }
.info-table td { padding: 5px 10px; border-bottom: 1px solid #0d1b2a; }
.info-table td:first-child { color: #888; width: 140px; }
.note { font-size: 12px; color: #666; margin-top: 8px; }
.file-list { margin-bottom: 16px; }
.file-list table { width: 100%; border-collapse: collapse; }
.file-list th {
  text-align: left; padding: 6px 10px; background: #16213e;
  font-size: 12px; color: #888; border-bottom: 1px solid #0f3460;
}
.file-list td { padding: 6px 10px; border-bottom: 1px solid #0d1b2a; }
.file-list tr:hover { background: #16213e; }
.file-list a { color: #88c0d0; text-decoration: none; }
.file-list a:hover { text-decoration: underline; }
.file-list .dir td:first-child a { color: #ebcb8b; }
.file-list .dsk td:first-child a { color: #a3be8c; }
.file-list .run { color: #a3be8c; font-size: 12px; }
.file-list .del { color: #bf616a; font-size: 12px; }
.file-list td:nth-child(2) { width: 80px; text-align: right; color: #888; font-size: 12px; }
.file-list td:nth-child(3), .file-list td:nth-child(4) { width: 50px; text-align: center; }
.upload-area {
  border: 2px dashed #1a3a6e; border-radius: 6px; padding: 24px;
  text-align: center; color: #888; transition: all 0.2s;
}
.upload-area.hover { border-color: #e94560; background: rgba(233,69,96,0.05); color: #e94560; }
.upload-area .link { color: #88c0d0; cursor: pointer; text-decoration: underline; }
#upload-status p { font-size: 12px; color: #a3be8c; margin-top: 4px; }
.loading { text-align: center; padding: 40px; color: #888; }
.rom-table { width: 100%; border-collapse: collapse; }
.rom-table th { text-align: left; padding: 6px 10px; background: #16213e;
  font-size: 12px; color: #888; border-bottom: 1px solid #0f3460; }
.rom-table td { padding: 4px 10px; border-bottom: 1px solid #0d1b2a; font-size: 13px; }
.rom-table td:first-child { width: 50px; text-align: center; color: #888; }
.rom-loaded td { color: #e0e0e0; }
.dot-on { color: #a3be8c; }
.dot-off { color: #555; }
.rom-name { color: #88c0d0; }
.preview-container { background: #000; border-radius: 4px; padding: 4px;
  display: inline-block; }
.preview-img { max-width: 100%; height: auto; image-rendering: pixelated;
  display: block; }
#toast-container {
  position: fixed; bottom: 20px; right: 20px; z-index: 9999;
  display: flex; flex-direction: column-reverse; gap: 8px;
}
.toast {
  background: #16213e; color: #a3be8c; border: 1px solid #0f3460;
  border-left: 3px solid #a3be8c; padding: 10px 16px; border-radius: 4px;
  font-size: 13px; opacity: 0; transform: translateX(40px);
  transition: opacity 0.3s, transform 0.3s; max-width: 360px;
  box-shadow: 0 4px 12px rgba(0,0,0,0.4);
}
.toast.show { opacity: 1; transform: translateX(0); }
)CSS";

static const uint8_t* m4_web_stylesheet_css =
   reinterpret_cast<const uint8_t*>(m4_web_stylesheet_css_src);
static const size_t m4_web_stylesheet_css_len = sizeof(m4_web_stylesheet_css_src) - 1;
