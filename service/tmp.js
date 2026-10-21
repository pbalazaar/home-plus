/* The service's own files in /tmp (its log, Recents, PicCap's pause mark), where any program on the
 * TV can make files: one of these names could be someone else's file there, or a link to one of
 * root's (config.json, say) for the service to write over. So they're opened without following a
 * link (O_NOFOLLOW), and used only if they're regular files of the user the service runs as; one
 * that's to be written but isn't is removed (just the link, if it's one) and made anew. Reading one
 * that isn't fails, as if it weren't there.
 */
'use strict';

const fs = require('fs');

const { O_RDONLY, O_WRONLY, O_CREAT, O_APPEND, O_EXCL } = fs.constants;
const NOFOLLOW = fs.constants.O_NOFOLLOW || 0; /* (none on Windows, which runs only the tests) */
const me = process.geteuid ? process.geteuid() : null; /* the user it runs as: root, on the TV */

const ours = (fd) => {
  const st = fs.fstatSync(fd);
  return st.isFile() && (me === null || st.uid === me);
};

// a file descriptor for file as the service's own (see above); flags are open(2)'s
function open(file, flags) {
  let fd = null;
  try {
    fd = fs.openSync(file, flags | NOFOLLOW, 0o600);
  } catch (e) {
    if (e.code !== 'ELOOP' || !(flags & O_CREAT)) throw e; /* (ELOOP: it's a link) */
  }
  if (fd !== null && ours(fd)) {
    if (flags & O_CREAT) fs.fchmodSync(fd, 0o600); /* (one made by an older service may be anyone's to read) */
    return fd;
  }
  if (fd !== null) fs.closeSync(fd);
  if (!(flags & O_CREAT)) throw new Error(`${file} isn't the service's own file`);
  try { fs.unlinkSync(file); } catch (e) { if (e.code !== 'ENOENT') throw e; }
  return fs.openSync(file, flags | NOFOLLOW | O_EXCL, 0o600); /* (made again by someone in between: it fails) */
}

function using(file, flags, then) {
  const fd = open(file, flags);
  try { return then(fd); } finally { fs.closeSync(fd); }
}

module.exports = {
  read: (file) => using(file, O_RDONLY, (fd) => fs.readFileSync(fd, 'utf8')),
  write: (file, data) => using(file, O_WRONLY | O_CREAT, (fd) => {
    fs.ftruncateSync(fd, 0);
    fs.writeFileSync(fd, data);
  }),
  // added at the end; first emptied when it's grown past max bytes
  append: (file, data, max) => using(file, O_WRONLY | O_CREAT | O_APPEND, (fd) => {
    if (fs.fstatSync(fd).size > max) fs.ftruncateSync(fd, 0);
    fs.writeFileSync(fd, data);
  }),
  // there, and the service's own
  has: (file) => {
    try { return using(file, O_RDONLY, () => true); } catch (e) { return false; }
  }
};
