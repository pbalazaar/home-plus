#!/bin/sh
# Loads the Control Center's QML (native/qml) on Qt's QML runtime and drives it (check.qml), against a
# stand-in for the service's API (mock-api.js). Fails on a failed check or any QML error. Needs qmlscene,
# Qt's QtQuick, QtGraphicalEffects and QtMultimedia modules, Xvfb and Node (the preview container has them).
set -e
cd "$(dirname "$0")/../.."
out=$(mktemp)
status=0
node tools/qml-check/mock-api.js 47999 & mock=$! # the service's API, answering saves slowly
trap 'kill $mock 2>/dev/null' EXIT
xvfb-run -a -s '-screen 0 1920x1080x24' qmlscene -I tools/qml-check/stubs tools/qml-check/check.qml >"$out" 2>&1 || status=$?
grep -v -E '^(QStandardPaths|libEGL warning|MESA|pci id)' "$out" || true
if [ $status -ne 0 ] || grep -q -E 'qml-check: [0-9]+ failed|(Error|error|TypeError|ReferenceError|non-existent|Cannot assign|FINAL|is not a type|is not installed|Unable to assign)' "$out" \
   || ! grep -q 'qml-check: ok' "$out"; then
  rm -f "$out"
  echo "qml-check: FAILED" >&2
  exit 1
fi
rm -f "$out"
