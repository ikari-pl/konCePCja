/* konCePCja - Amstrad CPC Emulator
   M4 Board — Embedded web assets

   These are served by the M4 HTTP server. The original M4 Board
   firmware uses lwIP httpd with SSI; this is a modern single-page
   replacement that communicates via the same REST-like endpoints.

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
<title>M4 Board — konCePCja</title>
<link rel="stylesheet" href="/stylesheet.css">
</head>
<body>
<div class="topbar">
  <h1>M4 Board</h1>
  <nav>
    <a href="/" class="btn active">Files</a>
    <a href="/status" class="btn">Status</a>
  </nav>
  <span id="status-led" class="led led-off"></span>
</div>

<div class="container">
  <div class="toolbar">
    <span id="current-dir">/</span>
    <div class="toolbar-right">
      <button onclick="goBack()" class="btn btn-sm">Back</button>
      <button onclick="refresh()" class="btn btn-sm">Refresh</button>
      <button onclick="showMkdir()" class="btn btn-sm">New Folder</button>
    </div>
  </div>

  <div id="mkdir-form" style="display:none" class="inline-form">
    <input type="text" id="mkdir-name" placeholder="Folder name">
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

<script>
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
  var encoded = encodeURIComponent(path);
  xhr.open('GET', '/config.cgi?ls=' + encoded, true);
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
          + '<td><a href="#" onclick="doDelete(\'' + escAttr(dpath) + '\');return false" class="del">Delete</a></td></tr>';
  }
  for (var k = 0; k < dsks.length; k++) {
    var kpath = currDir + dsks[k].name;
    html += '<tr class="dsk"><td><a href="#" onclick="loadDir(\'' + escAttr(kpath) + '/\');return false">'
          + esc(dsks[k].name) + '</a></td><td>' + fmtSize(dsks[k].size) + '</td>'
          + '<td><a href="#" onclick="doRun(\'' + escAttr(kpath) + '\');return false" class="run">Run</a></td>'
          + '<td><a href="#" onclick="doDelete(\'' + escAttr(kpath) + '\');return false" class="del">Delete</a></td></tr>';
  }
  for (var f = 0; f < files.length; f++) {
    var fpath = currDir + files[f].name;
    html += '<tr><td><a href="/sd' + encURI(fpath) + '">' + esc(files[f].name) + '</a></td>'
          + '<td>' + fmtSize(files[f].size) + '</td>'
          + '<td><a href="#" onclick="doRun(\'' + escAttr(fpath) + '\');return false" class="run">Run</a></td>'
          + '<td><a href="#" onclick="doDelete(\'' + escAttr(fpath) + '\');return false" class="del">Delete</a></td></tr>';
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
  e.preventDefault();
  this.classList.remove('hover');
  uploadFiles(e.dataTransfer.files);
};

// Poll status LED
setInterval(function() {
  fetch('/status').then(function(r) { return r.json(); }).then(function(s) {
    var led = document.getElementById('status-led');
    led.className = s.enabled ? 'led led-on' : 'led led-off';
  }).catch(function(){});
}, 5000);

// Helpers
function esc(s) { var d = document.createElement('div'); d.textContent = s; return d.innerHTML; }
function escAttr(s) { return s.replace(/'/g, "\\'").replace(/"/g, '&quot;'); }
function encURI(s) { return encodeURI(s).replace(/#/g, '%23'); }
function fmtSize(b) {
  if (!b) return '';
  if (b < 1024) return b + ' B';
  if (b < 1048576) return (b/1024).toFixed(1) + ' KB';
  return (b/1048576).toFixed(1) + ' MB';
}

// Initial load
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
.led { width: 10px; height: 10px; border-radius: 50%; margin-left: auto; }
.led-on { background: #4eff4e; box-shadow: 0 0 6px #4eff4e; }
.led-off { background: #444; }
.container { max-width: 960px; margin: 20px auto; padding: 0 20px; }
.toolbar {
  display: flex; justify-content: space-between; align-items: center;
  margin-bottom: 12px; padding: 8px 12px; background: #16213e;
  border-radius: 4px; font-family: monospace; font-size: 14px; color: #88c0d0;
}
.toolbar-right { display: flex; gap: 6px; }
.inline-form {
  display: flex; gap: 8px; margin-bottom: 12px; padding: 8px 12px;
  background: #16213e; border-radius: 4px;
}
.inline-form input {
  flex: 1; padding: 4px 8px; background: #0d1b2a; border: 1px solid #1a3a6e;
  color: #e0e0e0; border-radius: 3px; font-size: 13px;
}
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
.file-list td:nth-child(3), .file-list td:nth-child(4) { width: 60px; text-align: center; }
.upload-area {
  border: 2px dashed #1a3a6e; border-radius: 6px; padding: 24px;
  text-align: center; color: #888; transition: all 0.2s;
}
.upload-area.hover { border-color: #e94560; background: rgba(233,69,96,0.05); color: #e94560; }
.upload-area .link { color: #88c0d0; cursor: pointer; text-decoration: underline; }
#upload-status p { font-size: 12px; color: #a3be8c; margin-top: 4px; }
.loading { text-align: center; padding: 40px; color: #888; }
)CSS";

static const uint8_t* m4_web_stylesheet_css =
   reinterpret_cast<const uint8_t*>(m4_web_stylesheet_css_src);
static const size_t m4_web_stylesheet_css_len = sizeof(m4_web_stylesheet_css_src) - 1;
