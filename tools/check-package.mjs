// Checks a built .ipk before it goes to a TV or into a release: everything the TV needs is in it,
// the versions agree, nothing that mustn't ship does, and the launcher is a 32-bit ARM program
// that only calls SDL functions the TV's SDL 2.0.14 has.
//   node tools/check-package.mjs [file.ipk]   (default: dist/<id>_<version>_all.ipk)
import fs from 'node:fs';
import path from 'node:path';
import url from 'node:url';
import zlib from 'node:zlib';

const root = path.dirname(path.dirname(url.fileURLToPath(import.meta.url)));
const readJSON = (p) => JSON.parse(fs.readFileSync(path.join(root, p), 'utf8'));
const appinfo = readJSON('native/appinfo.json');
const svc = readJSON('service/services.json');
const ipk = process.argv[2] || path.join(root, 'dist', `${appinfo.id}_${appinfo.version}_all.ipk`);
const APP = `usr/palm/applications/${appinfo.id}`, SVC = `usr/palm/services/${svc.id}`, PKG = `usr/palm/packages/${appinfo.id}`;

const problems = [];
const check = (ok, problem) => { if (!ok) problems.push(problem); return ok; };

// ar: "!<arch>\n", then for each member a 60-byte header (name, ..., size at 48) and its data, padded to even
function arMembers(buf) {
  if (buf.toString('latin1', 0, 8) !== '!<arch>\n') throw new Error(`${ipk} isn't an .ipk (an ar archive)`);
  const members = new Map();
  for (let at = 8; at + 60 <= buf.length;) {
    const size = parseInt(buf.toString('latin1', at + 48, at + 58), 10);
    members.set(buf.toString('latin1', at, at + 16).trim().replace(/\/$/, ''), buf.subarray(at + 60, at + 60 + size));
    at += 60 + size + (size % 2);
  }
  return members;
}

// tar, as build.mjs writes it (names of 100 bytes at most): path -> { type, mode, data }
function tarEntries(gz) {
  const buf = zlib.gunzipSync(gz), entries = new Map();
  for (let at = 0; at + 512 <= buf.length;) {
    const h = buf.subarray(at, at + 512);
    if (h.every((b) => b === 0)) break;
    const field = (from, to) => h.toString('latin1', from, to).split('\0')[0].trim();
    const size = parseInt(field(124, 136), 8) || 0;
    entries.set(field(0, 100).replace(/^\.\//, '').replace(/\/$/, ''),
      { type: field(156, 157) || '0', mode: parseInt(field(100, 108), 8), data: buf.subarray(at + 512, at + 512 + size) });
    at += 512 + Math.ceil(size / 512) * 512;
  }
  return entries;
}

// the files under a folder of the repo, as paths relative to it (hidden files aren't packaged)
function walk(dir, rel = '') {
  return fs.readdirSync(path.join(root, dir, rel), { withFileTypes: true })
    .filter((e) => !e.name.startsWith('.'))
    .flatMap((e) => (e.isDirectory() ? walk(dir, `${rel}${e.name}/`) : [`${rel}${e.name}`]));
}

// the dynamic symbols an ELF32 program needs from its libraries
function undefinedSymbols(elf) {
  const shoff = elf.readUInt32LE(32), shentsize = elf.readUInt16LE(46), shnum = elf.readUInt16LE(48);
  const section = (i) => {
    const at = shoff + i * shentsize;
    return { type: elf.readUInt32LE(at + 4), offset: elf.readUInt32LE(at + 16), size: elf.readUInt32LE(at + 20), link: elf.readUInt32LE(at + 24) };
  };
  for (let i = 0; i < shnum; i++) {
    const dynsym = section(i);
    if (dynsym.type !== 11) continue; // SHT_DYNSYM
    const strtab = section(dynsym.link), names = [];
    for (let at = dynsym.offset + 16; at < dynsym.offset + dynsym.size; at += 16) { // 16-byte Elf32_Sym; the first is empty
      if (elf.readUInt16LE(at + 14) !== 0) continue; // defined in the program itself
      const name = strtab.offset + elf.readUInt32LE(at);
      names.push(elf.toString('latin1', name, elf.indexOf(0, name)));
    }
    return names;
  }
  return null;
}

if (!fs.existsSync(ipk)) {
  console.error(`${path.relative(process.cwd(), ipk)} not found: build it first (tools/docker-build.sh, or tools/build-release.sh)`);
  process.exit(1);
}
const ar = arMembers(fs.readFileSync(ipk));
for (const m of ['debian-binary', 'control.tar.gz', 'data.tar.gz']) check(ar.has(m), `the .ipk has no ${m}`);
if (!problems.length) {
  const data = tarEntries(ar.get('data.tar.gz'));
  const file = (p) => (data.get(p) && data.get(p).type === '0' ? data.get(p) : null);
  const json = (p) => { try { return JSON.parse(file(p).data.toString()); } catch (e) { return {}; } };

  // versions: the repo, the file name, the package's control file, appinfo and packageinfo agree
  const { version, id } = appinfo;
  check(readJSON('service/package.json').version === version,
    `versions differ: native/appinfo.json ${version}, service/package.json ${readJSON('service/package.json').version}`);
  check(path.basename(ipk) === `${id}_${version}_all.ipk`, `${path.basename(ipk)} should be named ${id}_${version}_all.ipk`);
  const control = tarEntries(ar.get('control.tar.gz')).get('control');
  const fields = Object.fromEntries((control ? control.data.toString() : '').split('\n').map((l) => l.split(': ')));
  check(fields.Package === id && fields.Version === version, `control says ${fields.Package} ${fields.Version}, not ${id} ${version}`);
  check(file(`${APP}/appinfo.json`) && file(`${APP}/appinfo.json`).data.equals(fs.readFileSync(path.join(root, 'native/appinfo.json'))),
    `${APP}/appinfo.json isn't native/appinfo.json`);
  const pkginfo = json(`${PKG}/packageinfo.json`);
  check(pkginfo.id === id && pkginfo.version === version && String(pkginfo.services) === svc.id,
    `${PKG}/packageinfo.json should name ${id} ${version} and the service ${svc.id}`);

  // everything the TV needs
  const wanted = [
    ...['appinfo.json', 'homeplus', 'icon.png', 'largeIcon.png', 'LICENSE', 'LICENSE-cJSON', 'LICENSE-SDL2.txt', 'THIRD_PARTY_NOTICES.md'],
    ...walk('native/assets').map((f) => `assets/${f}`),
    ...walk('native/qml').map((f) => `qml/${f}`),
    ...walk('init.d').map((f) => `init.d/${f}`)
  ].map((f) => `${APP}/${f}`);
  wanted.push(...walk('service').filter((f) => !f.startsWith('data/') && !f.endsWith('.test.js')).map((f) => `${SVC}/${f}`));
  for (const f of wanted) check(file(f), `missing: ${f}`);

  // nothing that mustn't ship: a local config (it can hold sign-ins), the API token, tests
  for (const p of data.keys()) {
    check(!p.startsWith(`${SVC}/data`) && !/(^|\/)(config\.json|api-token)$/.test(p) && !p.endsWith('.test.js'), `mustn't ship: ${p}`);
  }

  // modes: programs 0755, nothing writable by anyone but root (the service runs as root)
  for (const [p, e] of data) check(!(e.mode & 0o022), `${p} is writable by others (mode ${e.mode.toString(8)})`);
  for (const f of [`${APP}/homeplus`, ...walk('init.d').map((f) => `${APP}/init.d/${f}`)]) {
    if (file(f)) check(file(f).mode === 0o755, `${f} should be executable (mode 755, not ${file(f).mode.toString(8)})`);
  }

  // the launcher: 32-bit little-endian ARM, linked only against SDL 2.0.14's functions (LG's
  // extras, like SDL_webOSCursorVisibility, are looked up at run time with dlsym)
  const bin = file(`${APP}/homeplus`);
  if (bin && check(bin.data.readUInt32BE(0) === 0x7f454c46, 'homeplus isn\'t a Linux program (ELF)') &&
      check(bin.data[4] === 1 && bin.data[5] === 1 && bin.data.readUInt16LE(18) === 40,
        'homeplus should be 32-bit ARM, little-endian: the TV\'s userland is 32-bit (build with make -C native webos)')) {
    const sdl = new Set();
    const headers = path.join(root, 'native/third_party/SDL2');
    for (const h of fs.readdirSync(headers).filter((f) => f.endsWith('.h'))) {
      for (const m of fs.readFileSync(path.join(headers, h), 'latin1').matchAll(/\bSDLCALL\s+(SDL_\w+)\s*\(/g)) sdl.add(m[1]);
    }
    const needs = undefinedSymbols(bin.data);
    if (check(needs, 'homeplus has no dynamic symbol table')) {
      const newer = needs.filter((s) => s.startsWith('SDL_') && !sdl.has(s));
      check(!newer.length, `homeplus calls SDL functions the TV's SDL 2.0.14 doesn't have: ${newer.join(', ')}`);
    }
  }
  if (!problems.length) console.log(`${path.basename(ipk)}: ${data.size} entries, version ${version}: ok`);
}
if (problems.length) {
  console.error(`${path.basename(ipk)}:\n${problems.map((p) => `  ✗ ${p}`).join('\n')}`);
  process.exit(1);
}
