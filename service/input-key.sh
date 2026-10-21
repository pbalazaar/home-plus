#!/bin/sh
# LG Input Hook runs this for the remote's Input key (code 241), when Settings -> Remote -> Input
# Button is Home+ Inputs: the Control Center's inputs, in Home+ or over whatever's on screen, else
# LG's Input picker, as the key does without Home+ (also when Home+'s service doesn't answer).
APP=/media/developer/apps/usr/palm/applications/com.github.pbalazaar.homeplus
# The token goes to curl on its input, not its command line, which any process can read.
printf 'header = "X-Homeplus-Token: %s"\n' "$(cat "$APP/api-token" 2>/dev/null)" |
  curl -sf -m 2 -K - -X POST -H 'Content-Type: application/json' \
  -d '{"key":"input"}' http://127.0.0.1:47823/api/key >/dev/null 2>&1 ||
  luna-send -n 1 luna://com.webos.service.systemUIManager/launch '{"appId":"com.webos.app.quickinputpicker"}' >/dev/null 2>&1
