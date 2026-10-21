import QtQuick 2.4

// A stand-in for LG's StarfishSystemUIWindow (the compositor's own, on the TV), for tools/qml-check:
// the same lifecycle signals and functions, recording what the window asks of the compositor.
FocusScope {
    id: root
    width: 1920
    height: 1080
    property bool shown: false
    property bool exited: false
    property var windowProperties: ({})

    property QtObject lifeCycleStatus: QtObject {
        signal register(var payload)
        signal launch(var payload)
        signal close(var payload)
    }
    property QtObject webOSShell: QtObject {
        function setWindowProperty(key, value) { root.windowProperties[key] = value; }
    }
    property QtObject webOSInputManager: QtObject { property bool cursorVisibility: false }

    function show() { shown = true; }
    function hide() { shown = false; }
    function exit() { exited = true; }
}
