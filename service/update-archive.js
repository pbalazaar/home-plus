/* A deliberately small IPK reader/writer. Links, scripts, special files and paths outside this package are refused. */
'use strict';
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const crypto = require('crypto');
const APP = 'com.github.pbalazaar.homeplus';
const SERVICE = APP + '.service';
// PulseAudio creates this credential under the application's HOME at runtime.
const RUNTIME_COOKIE = '.config/pulse/cookie';
const LIMIT = 256 * 1024 * 1024;
const roots = ['usr/palm/applications/' + APP, 'usr/palm/services/' + SERVICE, 'usr/palm/packages/' + APP];
const hash = data => crypto.createHash('sha256').update(data).digest('hex');
function safe(name) {
  if (!name || name[0] === '/' || name.indexOf('\\') >= 0 || /[\x00-\x1f\x7f]/.test(name) || name.split('/').some(p => !p || p === '.' || p === '..')) throw new Error('Unsafe archive path');
  return name;
}
function unpackAr(buf) {
  if (buf.slice(0, 8).toString() !== '!<arch>\n') throw new Error('Invalid IPK header');
  const entries = {}; let off = 8;
  while (off < buf.length) {
    if (off + 60 > buf.length || buf.slice(off + 58, off + 60).toString() !== '`\n') throw new Error('Truncated IPK');
    const name = buf.slice(off, off + 16).toString().trim().replace(/\/$/, '');
    const number = buf.slice(off + 48, off + 58).toString().trim();
    if (!/^\d+$/.test(number)) throw new Error('Invalid IPK length');
    const size = Number(number); off += 60;
    if (!['debian-binary', 'control.tar.gz', 'data.tar.gz'].includes(name) || entries[name] || off + size > buf.length) throw new Error('Unsupported IPK member');
    entries[name] = buf.slice(off, off + size); off += size + size % 2;
  }
  if (off !== buf.length || Object.keys(entries).length !== 3 || entries['debian-binary'].toString() !== '2.0\n') throw new Error('Incomplete IPK');
  return entries;
}
function inflate(buf) {
  return new Promise((resolve, reject) => {
    const stream = zlib.createGunzip(), chunks = []; let size = 0;
    stream.on('data', c => { size += c.length; if (size > LIMIT) stream.destroy(new Error('Expanded package is too large')); else chunks.push(c); });
    stream.on('error', reject); stream.on('end', () => resolve(Buffer.concat(chunks))); stream.end(buf);
  });
}
function tar(buf, control) {
  const files = {}; let off = 0, ended = false;
  const oct = b => { const s = b.toString('ascii').replace(/\0.*$/, '').trim(); if (!/^[0-7]*$/.test(s)) throw new Error('Invalid tar number'); return parseInt(s || '0', 8); };
  while (off + 512 <= buf.length) {
    const h = buf.slice(off, off + 512); off += 512;
    if (h.every(n => n === 0)) { ended = true; break; }
    const stored = oct(h.slice(148, 156)); let sum = 0;
    for (let i = 0; i < 512; i++) sum += i >= 148 && i < 156 ? 32 : h[i];
    if (sum !== stored) throw new Error('Invalid tar checksum');
    const str = b => b.toString('utf8').replace(/\0.*$/, '');
    let name = str(h.slice(0, 100)), prefix = str(h.slice(345, 500));
    if (prefix) name = prefix + '/' + name;
    name = name.replace(/^\.\//, '').replace(/\/$/, '');
    const type = h[156], size = oct(h.slice(124, 136)), mode = oct(h.slice(100, 108));
    if (off + size > buf.length || (type !== 0 && type !== 48 && type !== 53) || (mode & 0o7000)) throw new Error('Unsupported or truncated tar entry');
    if (name === '.' || !name) { if (type !== 53 || size) throw new Error('Invalid archive root'); continue; }
    safe(name);
    if (control ? name !== 'control' : !roots.some(r => name === r || name.indexOf(r + '/') === 0 || (type === 53 && r.indexOf(name + '/') === 0))) throw new Error('Package writes outside Home+');
    if (files[name]) throw new Error('Duplicate archive path');
    files[name] = { data: buf.slice(off, off + size), mode: mode & 0o777, directory: type === 53 };
    if (type === 53 && size) throw new Error('Directory contains data');
    off += Math.ceil(size / 512) * 512;
  }
  if (!ended || buf.slice(off).some(n => n !== 0)) throw new Error('Invalid tar ending');
  return files;
}
async function inspect(buf, expectedVersion) {
  const ar = unpackAr(buf);
  const ctrl = tar(await inflate(ar['control.tar.gz']), true);
  if (!ctrl.control || ctrl.control.directory) throw new Error('Missing package control');
  const fields = {};
  ctrl.control.data.toString().trim().split('\n').forEach(line => { const at = line.indexOf(':'); if (at < 1) throw new Error('Invalid package control'); const k = line.slice(0, at); if (fields[k]) throw new Error('Duplicate control field'); fields[k] = line.slice(at + 1).trim(); });
  if (fields.Package !== APP || fields.Version !== expectedVersion || fields.Architecture !== 'all') throw new Error('Package control identity mismatch');
  const files = tar(await inflate(ar['data.tar.gz']), false);
  function json(name) { if (!files[name] || files[name].directory) throw new Error('Missing package identity'); return JSON.parse(files[name].data.toString()); }
  const app = json(roots[0] + '/appinfo.json'), svc = json(roots[1] + '/package.json'), pkg = json(roots[2] + '/packageinfo.json'), services = json(roots[1] + '/services.json');
  if (app.id !== APP || app.version !== expectedVersion || app.main !== 'homeplus' || app.type !== 'native' || svc.name !== SERVICE || svc.version !== expectedVersion ||
      pkg.id !== APP || pkg.app !== APP || pkg.version !== expectedVersion || JSON.stringify(pkg.services) !== JSON.stringify([SERVICE]) || services.id !== SERVICE ||
      !files[roots[0] + '/homeplus'] || !files[roots[1] + '/service.js']) throw new Error('Package payload identity mismatch');
  const inventory = {};
  Object.keys(files).forEach(name => { if (!files[name].directory) inventory[name] = hash(files[name].data); });
  return { files, inventory, version: expectedVersion, size: buf.length };
}
function tarGz(files) {
  const blocks = [];
  for (const name of Object.keys(files).sort()) {
    safe(name); const entry = files[name], data = entry.data, h = Buffer.alloc(512);
    let short = name, prefix = '';
    if (Buffer.byteLength(short) > 100) { const at = name.lastIndexOf('/'); prefix = name.slice(0, at); short = name.slice(at + 1); }
    if (Buffer.byteLength(short) > 100 || Buffer.byteLength(prefix) > 155) throw new Error('Backup path is too long');
    h.write(short, 0); h.write(prefix, 345);
    const oct = (n, start, length) => h.write(n.toString(8).padStart(length - 1, '0') + '\0', start, length, 'ascii');
    oct(entry.mode & 0o777, 100, 8); oct(0, 108, 8); oct(0, 116, 8); oct(data.length, 124, 12); oct(0, 136, 12);
    h.fill(32, 148, 156); h[156] = 48; h.write('ustar\0', 257); h.write('00', 263);
    let sum = 0; for (const b of h) sum += b;
    h.write(sum.toString(8).padStart(6, '0') + '\0 ', 148);
    blocks.push(h, data, Buffer.alloc((512 - data.length % 512) % 512));
  }
  blocks.push(Buffer.alloc(1024));
  return zlib.gzipSync(Buffer.concat(blocks));
}
function pack(files, version) {
  const members = { 'debian-binary': Buffer.from('2.0\n'), 'control.tar.gz': tarGz({ control: { mode: 0o644, data: Buffer.from('Package: ' + APP + '\nVersion: ' + version + '\nArchitecture: all\nwebOS-Package-Format-Version: 2\n') } }), 'data.tar.gz': tarGz(files) };
  const blocks = [Buffer.from('!<arch>\n')];
  Object.keys(members).forEach(name => { const data = members[name]; blocks.push(Buffer.from((name + '/').padEnd(16) + '0'.padEnd(12) + '0'.padEnd(6) + '0'.padEnd(6) + '100644'.padEnd(8) + String(data.length).padEnd(10) + '`\n'), data); if (data.length % 2) blocks.push(Buffer.from('\n')); });
  return Buffer.concat(blocks);
}
function snapshot(directories) {
  const files = {}; let size = 0;
  directories.forEach((dir, index) => {
    function walk(rel) {
      if (index === 0 && rel === 'api-token') return; // regenerated by the running service after replacement
      const absolute = path.join(dir, rel), st = fs.lstatSync(absolute);
      if (index === 0 && rel === RUNTIME_COOKIE) {
        if (!st.isFile()) throw new Error('Unsupported installed runtime file');
        return; // never read or include this credential in a reinstallation backup
      }
      if (st.isSymbolicLink() && /^qml-[a-z0-9]+$/.test(rel) && index === 0 && fs.readlinkSync(absolute) === 'qml') return;
      if (st.isDirectory()) { for (const n of fs.readdirSync(absolute)) walk(rel ? rel + '/' + n : n); return; }
      if (!st.isFile()) throw new Error('Unsupported installed file in backup');
      size += st.size; if (size > LIMIT) throw new Error('Installed package is too large to back up');
      files[roots[index] + '/' + rel] = { mode: st.mode & 0o777, data: fs.readFileSync(absolute) };
    }
    walk('');
  });
  return files;
}
function verifyInstalled(inventory, directories) {
  const actual = snapshot(directories);
  for (const name of Object.keys(actual)) {
    if (!Object.prototype.hasOwnProperty.call(inventory, name) && name !== roots[0] + '/api-token') throw new Error('Unexpected file in installed payload');
  }
  for (const name of Object.keys(inventory)) {
    // Older private backups may contain a cookie; its current value or absence is not payload drift.
    // snapshot() above still refuses non-regular files at this exact location.
    if (name === roots[0] + '/' + RUNTIME_COOKIE) continue;
    const index = roots.findIndex(r => name.indexOf(r + '/') === 0);
    if (index < 0) throw new Error('Invalid installed inventory');
    const file = path.join(directories[index], name.slice(roots[index].length + 1));
    if (!fs.lstatSync(file).isFile() || hash(fs.readFileSync(file)) !== inventory[name]) throw new Error('Installed payload verification failed');
  }
}
module.exports = { RUNTIME_COOKIE, inspect, pack, snapshot, verifyInstalled, hash, roots, tar, unpackAr };
