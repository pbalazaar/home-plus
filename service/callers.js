// Who may call Home+'s luna methods. The service is root, and any app can call a service on the bus
// (an app's calls carry its app id), so the methods that change the TV or Home+'s accounts answer
// only the TV's own tools, luna-send (terminals and LG Input Hook's scripts, run as root: its
// callers are com.webos.lunasend-<pid>, a name no app can take), and Home+ itself. `ping` answers
// anyone: the boot script and the launcher wake the service with it.
'use strict';

const OWN_APP = 'com.github.pmiraka.homeplus';

function trusted(sender) {
  const s = String(sender || '');
  return /^com\.webos\.lunasend-\d+$/.test(s) || s === OWN_APP;
}

module.exports = { trusted };
