#!/bin/sh
# LG Input Hook runs this for the remote's Settings key (code 139), when Settings -> Remote ->
# Settings Button is Control Center: Home+'s Control Center while Home+ is in front, else LG's
# Quick Settings, as the key does without Home+ (also when Home+'s service doesn't answer).
APP=/media/developer/apps/usr/palm/applications/com.github.pbalazaar.homeplus
# The token goes to curl on its input, not its command line, which any process can read.
printf 'header = "X-Homeplus-Token: %s"\n' "$(cat "$APP/api-token" 2>/dev/null)" |
  curl -sf -m 2 -K - -X POST -H 'Content-Type: application/json' \
  -d '{"key":"settings"}' http://127.0.0.1:47823/api/key >/dev/null 2>&1 ||
  luna-send -n 1 luna://com.webos.service.systemUIManager/launch '{"appId":"com.webos.app.quicksettings"}' >/dev/null 2>&1
