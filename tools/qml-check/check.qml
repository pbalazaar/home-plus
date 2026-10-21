import QtQuick 2.4

// Loads the Control Center over other apps (native/qml) with sample controls and drives it as the
// remote would, on Qt's own QML runtime with stand-ins for LG's modules (stubs/): an error in the QML
// fails here instead of on the TV, where nothing logs it. Run by tools/qml-check/run.sh.
Item {
    id: top
    width: 1920
    height: 1080

    property var failures: []
    function fail(what) { failures.push(what); console.error("qml-check: " + what); }
    function expect(ok, what) { if (!ok) fail(what); }

    // sample state, as the service sends it (control.js: state())
    readonly property var sample: ({
        controls: [
            { id: "power", size: "large", title: "Power Off", kind: "action", icon: "power", value: "", on: null, available: true },
            { id: "network", size: "wide", title: "Wi-Fi", kind: "opens", icon: "wifi", value: "Home Wi-Fi", on: true, available: true },
            { id: "screenOff", size: "wide", title: "Screen Off", kind: "action", icon: "screenOff", value: "", on: null, available: true },
            { id: "sound", size: "wide", title: "Sound", kind: "stepper", icon: "sound", value: "HDMI eARC", on: true, available: true },
            { id: "sleepTimer", size: "wide", title: "Sleep Timer", kind: "stepper", icon: "timer", value: "Off", on: false, available: true },
            { id: "pictureMode", size: "wide", title: "Picture", kind: "list", icon: "picture", value: "Filmmaker Mode", on: null, available: true },
            { id: "eyeComfort", size: "wide", title: "Eye Comfort", kind: "switch", icon: "eye", value: "Off", on: false, available: true },
            { id: "inputs", size: "small", title: "Inputs", kind: "list", icon: "input", value: "", on: null, available: true },
            { id: "toneMapping", size: "small", title: "Dynamic Tone Mapping", kind: "stepper", icon: "hdr", value: "Not Available", on: null, available: false },
            { id: "settings", size: "small", title: "Settings", kind: "opens", icon: "settings", value: "", on: null, available: true }
        ],
        catalog: [
            { id: "power", title: "Power Off", kind: "action", icon: "power", group: "TV", sizes: ["large", "small"], available: true, placed: true },
            { id: "soundMode", title: "Sound Mode", kind: "stepper", icon: "soundMode", group: "Picture & Sound", sizes: ["wide", "small"], available: true, placed: false },
            { id: "brightness", title: "OLED Brightness", kind: "stepper", icon: "brightness", group: "Picture & Sound", sizes: ["wide", "small"], available: true, placed: false },
            { id: "settings", title: "Settings", kind: "opens", icon: "settings", group: "Home+", sizes: ["small"], available: true, placed: true, pinned: true }
        ],
        picture: { mode: "filmMaker", label: "Filmmaker Mode", modes: [{ id: "vivid", label: "Vivid" }, { id: "filmMaker", label: "Filmmaker Mode" }] },
        inputs: [{ appId: "com.webos.app.hdmi1", label: "HDMI 1", device: "PS5", plugged: true }],
        sounds: false // QtMultimedia's sound needs a sound card: Sounds.qml is only compiled, below
    })

    Loader { id: win; anchors.fill: parent; source: Qt.resolvedUrl("../../native/qml/ControlCenter.qml") }

    property int step: 0
    Timer {
        id: driver
        interval: 250
        repeat: true
        running: win.status === Loader.Ready || win.status === Loader.Error
        onTriggered: top.next()
    }

    function next() {
        var w = win.item, v = w ? w.view : null;
        switch (step++) {
        case 0:
            expect(win.status === Loader.Ready, "ControlCenter.qml loads: " + win.sourceComponent);
            if (!w) return finish();
            for (var f of ["ControlCenterView.qml", "Glass.qml", "Tint.qml", "Marquee.qml", "Sounds.qml"]) {
                var c = Qt.createComponent(Qt.resolvedUrl("../../native/qml/" + f));
                expect(c.status === Component.Ready, f + " compiles: " + c.errorString());
            }
            w.lifeCycleStatus.launch({ params: { state: JSON.parse(JSON.stringify(sample)), token: "t", port: 1, frame: "" } });
            break;
        case 1:
            expect(w.shown, "the window is shown");
            expect(!!v, "the panel is created");
            if (!v) return finish();
            expect(w.windowProperties["_WEBOS_ACCESS_POLICY_KEYS_BACK"] === "true", "Back is the panel's");
            expect(v.focusIndex === 1, "the focus starts on Wi-Fi: " + v.focusIndex);
            v.move(0, 1); expect(v.focusIndex === 2, "Down: Screen Off");
            v.move(1, 0); expect(v.focusIndex === 2, "Right at the edge: nothing that way");
            v.move(0, 1); v.move(0, 1); v.move(-1, 0);
            break;
        case 2:
            v.longPress(); expect(!!v.sheet && v.sheet.items[0].act === "edit", "holding OK: Edit Controls");
            v.back(); expect(!v.sheet, "Back closes the sheet");
            v.openList(1); expect(v.listKind === 1 && v.listItems.length === 2, "the Picture list");
            v.back(); expect(v.listKind === 0, "Back closes the list");
            break;
        case 3:
            v.longPress(); v.sheetOk(); expect(v.editing, "editing");
            v.focusIndex = 1; v.ok(); expect(v.picked === 1, "OK picks Wi-Fi up");
            v.move(0, 1); expect(v.picked === 2, "Down moves it");
            v.move(0, -1); v.ok(); expect(v.picked === -1, "OK puts it down (and saves)");
            break;
        case 4:
            v.focusIndex = 1; v.longPress(); expect(!!v.sheet && v.sheet.items.some(function (i) { return i.act === "remove"; }), "its options: Remove");
            v.back();
            v.focusIndex = v.focusCount() - 2; v.ok(); expect(v.listKind === 3 && v.gallery.length === 2, "the + slot: the gallery, without what's placed");
            v.back(); v.focusIndex = v.focusCount() - 1; v.ok(); expect(!!v.sheet && v.sheet.items[0].act === "reset", "Reset asks first");
            v.back(); v.back(); expect(!v.editing, "Back: done editing");
            break;
        case 5:
            w.lifeCycleStatus.launch({ params: {} }); // the Settings key again
            break;
        case 6:
        case 7:
            break; // it animates out
        case 8:
            expect(w.exited, "the Settings key again closes it");
            finish();
            break;
        }
    }

    function finish() {
        driver.stop();
        console.log(failures.length ? "qml-check: " + failures.length + " failed" : "qml-check: ok");
        Qt.exit(failures.length ? 1 : 0);
    }
}
