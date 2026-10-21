// Builds the .ipk packages into dist/ without ares-cli or a system tar, so macOS and Linux
// produce the same packages.
// An .ipk is an `ar` archive of: debian-binary, control.tar.gz, data.tar.gz.
//   node tools/build.mjs   -> dist/com.github.pbalazaar.homeplus_<version>_all.ipk: the native
//                             launcher with its service and boot script (one package, as the
//                             Homebrew Channel lists one). Run `make -C native webos` first.
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import url from 'node:url';
import zlib from 'node:zlib';

const root = path.dirname(path.dirname(url.fileURLToPath(import.meta.url)));
const dist = path.join(root, 'dist');
const readJSON = (p) => JSON.parse(fs.readFileSync(path.join(root, p), 'utf8'));

function duKB(dir) {
  let total = 0;
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    total += e.isDirectory() ? duKB(p) * 1024 : fs.statSync(p).size;
  }
  return Math.ceil(total / 1024);
}

// Plain (BSD-style) ar: every member name here fits in 16 chars, no long-name table needed.
function ar(dir, members) {
  const chunks = [Buffer.from('!<arch>\n')];
  const now = Math.floor(Date.now() / 1000);
  for (const name of members) {
    const data = fs.readFileSync(path.join(dir, name));
    const h = name.padEnd(16) + String(now).padEnd(12) + '0'.padEnd(6) + '0'.padEnd(6) +
      '100644'.padEnd(8) + String(data.length).padEnd(10) + '`\n';
    chunks.push(Buffer.from(h, 'ascii'), data);
    if (data.length % 2) chunks.push(Buffer.from('\n'));
  }
  return Buffer.concat(chunks);
}

// The .tar.gz that `tar --format=gnu --owner=0 --group=0 --numeric-owner --sort=name -czf out -C dir .`
// writes. macOS's bsdtar has none of those flags, and it would add ._* entries for extended attributes.
// Modes are set here, not read from the disk, so a package built from a Windows folder (where every
// file looks 0777, or 0666) comes out the same: folders and the files in `executable` 0755, the rest 0644.
function tarGz(dir, executable = new Set()) {
  const blocks = [];
  const header = (name, st, type, size, mode) => {
    if (Buffer.byteLength(name) > 100) throw new Error(`path too long for a tar header: ${name}`);
    const h = Buffer.alloc(512);
    const octal = (offset, width, n) => h.write(n.toString(8).padStart(width - 1, '0'), offset, 'ascii');
    h.write(name, 0);
    octal(100, 8, mode);
    octal(108, 8, 0); // uid
    octal(116, 8, 0); // gid
    octal(124, 12, size);
    octal(136, 12, Math.floor(st.mtimeMs / 1000));
    h.write(type, 156, 'ascii');
    h.write('ustar  ', 257, 'ascii'); // GNU magic and version
    h.fill(' ', 148, 156); // the checksum is summed with spaces in its own field
    const sum = h.reduce((a, b) => a + b, 0);
    h.write(sum.toString(8).padStart(6, '0') + '\0 ', 148, 'ascii');
    return h;
  };
  (function add(rel) {
    const p = path.join(dir, rel);
    const st = fs.statSync(p);
    if (st.isDirectory()) {
      blocks.push(header(rel ? `./${rel}/` : './', st, '5', 0, 0o755));
      for (const name of fs.readdirSync(p).sort()) add(rel ? `${rel}/${name}` : name);
    } else {
      const data = fs.readFileSync(p);
      blocks.push(header(`./${rel}`, st, '0', data.length, executable.has(rel) ? 0o755 : 0o644),
        data, Buffer.alloc((512 - data.length % 512) % 512));
    }
  })('');
  blocks.push(Buffer.alloc(1024)); // end-of-archive marker
  const size = blocks.reduce((a, b) => a + b.length, 0);
  blocks.push(Buffer.alloc((10240 - size % 10240) % 10240)); // pad to whole 10 KiB records, as tar does
  return zlib.gzipSync(Buffer.concat(blocks));
}

// stage(appDir) fills the data tree; services lists service ids for packageinfo; executable lists
// the programs in it (paths in the data tree).
function buildPackage({ appinfo, stage: fill, services = [], executable = [] }) {
  const { id, version } = appinfo;
  const build = path.join(root, 'build', id);
  const data = path.join(build, 'data');
  const ctrl = path.join(build, 'control');
  fs.rmSync(build, { recursive: true, force: true });

  const appDir = path.join(data, 'usr/palm/applications', id);
  fs.mkdirSync(appDir, { recursive: true });
  fill(appDir, data);
  const pkgDir = path.join(data, 'usr/palm/packages', id);
  fs.mkdirSync(pkgDir, { recursive: true });
  fs.writeFileSync(path.join(pkgDir, 'packageinfo.json'), JSON.stringify({
    app: id, id, loc_name: appinfo.title, package_format_version: 2,
    vendor: appinfo.vendor || 'unknown', version, ...(services.length ? { services } : {})
  }, null, 2));

  fs.mkdirSync(ctrl, { recursive: true });
  fs.writeFileSync(path.join(ctrl, 'control'), [
    `Package: ${id}`, `Version: ${version}`, 'Section: misc', 'Priority: optional', 'Architecture: all',
    `Installed-Size: ${duKB(data)}`, 'Maintainer: N/A <nobody@example.com>', `Description: ${appinfo.title}`,
    'webOS-Package-Format-Version: 2', 'webOS-Packager-Version: x.y.x', ''
  ].join('\n'));

  fs.writeFileSync(path.join(build, 'control.tar.gz'), tarGz(ctrl));
  fs.writeFileSync(path.join(build, 'data.tar.gz'), tarGz(data, new Set(executable)));
  fs.writeFileSync(path.join(build, 'debian-binary'), '2.0\n');

  fs.mkdirSync(dist, { recursive: true });
  const out = path.join(dist, `${id}_${version}_all.ipk`);
  fs.writeFileSync(out, ar(build, ['debian-binary', 'control.tar.gz', 'data.tar.gz']));
  console.log(`built ${path.relative(root, out)} (${fs.statSync(out).size} bytes)`);
  return out;
}

// ---- Home+: the launcher, its service and its boot script, in one package ----
const appinfo = readJSON('native/appinfo.json');
const svc = readJSON('service/services.json');
const svcVersion = readJSON('service/package.json').version;
if (svcVersion !== appinfo.version) throw new Error(`versions differ: native/appinfo.json ${appinfo.version}, service/package.json ${svcVersion}`);
const bin = path.join(root, 'native/build/webos/homeplus');
if (!fs.existsSync(bin)) throw new Error('native/build/webos/homeplus missing: run `make -C native webos` first');
const svcDir = path.join(root, 'service');
// never hidden files (.DS_Store), service/data (a local config, maybe with sign-ins) or tests
const shipped = (src) => !path.basename(src).startsWith('.') &&
  path.relative(svcDir, src).split(path.sep)[0] !== 'data' && !src.endsWith('.test.js');
const ipk = buildPackage({
  appinfo,
  services: [svc.id],
  executable: ['homeplus', ...fs.readdirSync(path.join(root, 'init.d')).map((f) => `init.d/${f}`)]
    .map((f) => `usr/palm/applications/${appinfo.id}/${f}`),
  stage(appDir, data) {
    fs.copyFileSync(path.join(root, 'native/appinfo.json'), path.join(appDir, 'appinfo.json'));
    fs.copyFileSync(bin, path.join(appDir, 'homeplus'));
    fs.cpSync(path.join(root, 'native/assets'), path.join(appDir, 'assets'), { recursive: true, filter: shipped });
    fs.copyFileSync(path.join(root, 'native/img/icon.png'), path.join(appDir, 'icon.png'));
    fs.copyFileSync(path.join(root, 'native/img/largeIcon.png'), path.join(appDir, 'largeIcon.png'));
    // cJSON is compiled into the binary, so its notice ships with it (the font's is in assets/)
    fs.copyFileSync(path.join(root, 'LICENSE'), path.join(appDir, 'LICENSE'));
    fs.copyFileSync(path.join(root, 'native/third_party/LICENSE-cJSON'), path.join(appDir, 'LICENSE-cJSON'));
    // stb and the vendored SDL/Khronos headers are compiled in as well
    fs.copyFileSync(path.join(root, 'THIRD_PARTY_NOTICES.md'), path.join(appDir, 'THIRD_PARTY_NOTICES.md'));
    fs.copyFileSync(path.join(root, 'native/third_party/SDL2/LICENSE-SDL2.txt'), path.join(appDir, 'LICENSE-SDL2.txt'));
    // the Control Center over other apps: QML that LG's compositor runs, and its glyphs
    fs.cpSync(path.join(root, 'native/qml'), path.join(appDir, 'qml'), { recursive: true, filter: shipped });
    // the boot script travels with the app, so it can be installed on the TV with cp
    fs.cpSync(path.join(root, 'init.d'), path.join(appDir, 'init.d'), { recursive: true, filter: shipped });
    fs.cpSync(svcDir, path.join(data, 'usr/palm/services', svc.id), { recursive: true, filter: shipped });
  }
});

// The Homebrew Channel's manifest for the package: webosbrew's apps-repo lists Home+ from the latest
// release's copy, and its hash and size describe this very .ipk, so every release carries a fresh one.
// GITHUB_REPOSITORY (set in CI) keeps its links right if the repository moves.
const repo = process.env.GITHUB_REPOSITORY || 'pbalazaar/home-plus';
const manifest = path.join(dist, `${appinfo.id}.manifest.json`);
fs.writeFileSync(manifest, JSON.stringify({
  id: appinfo.id, version: appinfo.version, type: appinfo.type, title: appinfo.title,
  appDescription: 'A tvOS-style home screen for rooted LG TVs, in a Liquid Glass look',
  iconUri: `https://raw.githubusercontent.com/${repo}/main/docs/icon.png`,
  sourceUrl: `https://github.com/${repo}`,
  rootRequired: true,
  ipkUrl: path.basename(ipk),
  ipkSize: fs.statSync(ipk).size,
  ipkHash: { sha256: crypto.createHash('sha256').update(fs.readFileSync(ipk)).digest('hex') }
}, null, 2) + '\n');
console.log(`wrote ${path.relative(root, manifest)}`);
