// loader.js — get the player's own disc files into the Emscripten file system
// and start the engine. Nothing is uploaded: files are read locally and kept
// in IndexedDB (IDBFS at /disc) so later visits start at once. Settings and
// high scores persist through IDBFS at /libsdl (SDL_GetPrefPath).
'use strict';

const NEEDED = /^(JUNG\w*\.BIN|HYENA\.TTF|JUNGA01\.DLL|JUNGU01\.DLL)$/i;
const $ = (id) => document.getElementById(id);
const status = (t) => { $('status').textContent = t; };
const progress = (f) => {                          // Pumbaa's trot across the loading track
  document.documentElement.style.setProperty('--progress', Math.max(0, Math.min(1, f)).toFixed(3));
  $('track').classList.toggle('done', f >= 1);
};

let pending = null;          // [{name, data}] chosen this visit, not yet written

// ---- ISO 9660: find the JUNGLE directory and read the files the game needs ----
async function fromIso(file) {
  const sector = async (lba, n = 1) => new Uint8Array(await file.slice(lba * 2048, (lba + n) * 2048).arrayBuffer());
  const pvd = await sector(16);
  if (String.fromCharCode(...pvd.slice(1, 6)) !== 'CD001') throw new Error('not an ISO 9660 image');
  const u32 = (b, o) => b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24) >>> 0;
  const records = async (lba, len) => {
    const b = await sector(lba, Math.ceil(len / 2048)); const out = [];
    for (let p = 0; p < len;) {
      const rl = b[p];
      if (!rl) { p = (Math.floor(p / 2048) + 1) * 2048; continue; }
      const nl = b[p + 32];
      let name = String.fromCharCode(...b.slice(p + 33, p + 33 + nl)).replace(/;1$/, '').replace(/\.$/, '');
      out.push({ name, lba: u32(b, p + 2), len: u32(b, p + 10), dir: !!(b[p + 25] & 2) });
      p += rl;
    }
    return out;
  };
  const root = await records(u32(pvd, 156 + 2), u32(pvd, 156 + 10));
  let dir = root.find((r) => r.dir && r.name.toUpperCase() === 'JUNGLE');
  let list = dir ? await records(dir.lba, dir.len) : root;
  const files = [];
  for (const r of list) {
    if (r.dir || !NEEDED.test(r.name)) continue;
    files.push({ name: r.name.toUpperCase(), data: new Uint8Array(await file.slice(r.lba * 2048, r.lba * 2048 + r.len).arrayBuffer()) });
  }
  return files;
}

async function fromFolder(fileList) {
  const files = [];
  for (const f of fileList) if (NEEDED.test(f.name)) files.push({ name: f.name.toUpperCase(), data: new Uint8Array(await f.arrayBuffer()) });
  return files;
}

function check(files) {
  const names = new Set(files.map((f) => f.name));
  const miss = ['JUNGLE.BIN', 'JUNGMAIN.BIN', 'HYENA.TTF', 'JUNGA01.DLL', 'JUNGU01.DLL'].filter((n) => !names.has(n));
  if (miss.length) throw new Error('missing ' + miss.join(', ') + ' — is this the Jungle Games disc?');
}

async function choose(files) {
  check(files);
  pending = files;
  const mb = files.reduce((a, f) => a + f.data.length, 0) / 1048576;
  status(`Found ${files.length} game files (${mb.toFixed(0)} MB). Press Play.`);
  $('play').style.display = '';
}

$('dir').addEventListener('change', (e) => fromFolder(e.target.files).then(choose).catch((x) => status(String(x.message || x))));
$('iso').addEventListener('change', (e) => { status('Reading the disc image…'); fromIso(e.target.files[0]).then(choose).catch((x) => status(String(x.message || x))); });

// ---- the engine -------------------------------------------------------------
const syncfs = (populate) => new Promise((res) => EFS.syncfs(populate, () => res()));
let EFS;

var Module = {
  noInitialRun: true,
  canvas: $('canvas'),
  print: (t) => console.log(t),
  printErr: (t) => console.warn(t),
  onRuntimeInitialized: async () => {
    EFS = Module.FS;
    for (const d of ['/disc', '/libsdl']) { try { EFS.mkdir(d); } catch (_) {} EFS.mount(Module.IDBFS || EFS.filesystems.IDBFS, {}, d); }
    await syncfs(true);
    let stored = false;
    try { stored = EFS.analyzePath('/disc/JUNGLE.BIN').exists && EFS.analyzePath('/disc/JUNGMAIN.BIN').exists; } catch (_) {}
    if (await hosted(stored)) return;
    document.querySelector('#setup .row').style.display = '';          /* no hosted data: ask for a disc */
    if (stored) { status('Your disc is stored in this browser. Press Play.'); $('play').style.display = ''; $('forget').style.display = ''; }
    else status('Choose your disc to begin.');
  },
};

// ---- hosted mode: the server provides the game files (data/manifest.json) ----
async function hosted(stored) {
  let man;
  try { const r = await fetch('data/manifest.json?t=' + Date.now(), { cache: 'no-store' }); if (!r.ok) return false; man = await r.json(); }
  catch (_) { return false; }
  document.querySelector('#setup .row').style.display = 'none';
  document.querySelectorAll('#setup > p:not(#status)').forEach((p) => { p.style.display = 'none'; });
  let have = '';
  try { have = EFS.readFile('/disc/.version', { encoding: 'utf8' }); } catch (_) {}
  if (!(stored && have === man.version)) {
    const total = man.files.reduce((a, f) => a + f.size, 0); let got = 0;
    for (const f of man.files) {
      const r = await fetch('data/' + f.name + '?v=' + man.version, { cache: 'no-store' });
      if (!r.ok) { status('Could not load ' + f.name + ' (' + r.status + ')'); return true; }
      const reader = r.body.getReader(); const parts = [];
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        parts.push(value); got += value.length;
        status(`Loading the game… ${Math.floor(100 * got / total)}%`); progress(got / total);
      }
      const buf = new Uint8Array(f.size); let o = 0;
      for (const p of parts) { buf.set(p, o); o += p.length; }
      EFS.writeFile('/disc/' + f.name, buf);
    }
    EFS.writeFile('/disc/.version', man.version);
    await syncfs(false);
  }
  progress(1); status('Ready! Press Play.');
  $('play').style.display = '';
  return true;
}

$('play').addEventListener('click', async () => {
  if (pending) {
    status('Storing the disc in this browser…');
    for (const f of pending) EFS.writeFile('/disc/' + f.name, f.data);
    await syncfs(false);
    pending = null;
  }
  document.body.classList.add('playing');
  $('canvas').focus();
  setInterval(() => syncfs(false), 10000);        // settings and high scores
  Module.callMain(['/disc/JUNGLE.BIN', '--game']);
});

$('forget').addEventListener('click', async () => {
  for (const n of EFS.readdir('/disc')) if (n !== '.' && n !== '..') EFS.unlink('/disc/' + n);
  await syncfs(false);
  status('Stored disc removed.'); $('play').style.display = 'none'; $('forget').style.display = 'none';
});

// ---- on-screen buttons (phones): each is a key held while touched ----
let webKey = null;
for (const b of document.querySelectorAll('#pad button')) {
  const vk = +b.dataset.vk;
  const down = (e) => { e.preventDefault(); if (!webKey) webKey = Module.cwrap('web_key', null, ['number', 'number']); b.classList.add('on'); webKey(vk, 1); };
  const up = (e) => { e.preventDefault(); if (!b.classList.contains('on')) return; b.classList.remove('on'); webKey && webKey(vk, 0); };
  b.addEventListener('pointerdown', down);
  for (const ev of ['pointerup', 'pointercancel', 'pointerleave']) b.addEventListener(ev, up);
}

// ---- size: fit the window, or the original 800x600 (remembered) ----
const setSize = (orig) => {
  document.body.classList.toggle('orig', orig);
  $('size').textContent = orig ? 'Fit window' : 'Original size';
  try { localStorage.setItem('jungle-size', orig ? 'orig' : 'fit'); } catch (_) {}
};
let startOrig = false;
try { startOrig = localStorage.getItem('jungle-size') === 'orig'; } catch (_) {}
setSize(startOrig);
$('size').addEventListener('click', () => { setSize(!document.body.classList.contains('orig')); $('canvas').focus(); });

window.Module = Module;
const BUILD = '1790357658';                            // cache-busting: every URL is versioned
Module.locateFile = (path) => path + '?v=' + BUILD;
const s = document.createElement('script'); s.src = 'jungle.js?v=' + BUILD; document.body.appendChild(s);
