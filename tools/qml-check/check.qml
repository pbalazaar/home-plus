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
    property int movedTo: -1
    Timer {
        id: driver
        interval: 250
        repeat: true
        running: win.status === Loader.Ready || win.status === Loader.Error
        onTriggered: top.next()
    }

    function next() {
        var w = win.item, v = w ? w.view : null, s = v ? v.settings : null, r = v ? v.recents : null;
        switch (step++) {
        case 0:
            expect(win.status === Loader.Ready, "ControlCenter.qml loads: " + win.sourceComponent);
            if (!w) return finish();
            for (var f of ["ControlCenterView.qml", "SettingsPanel.qml", "RecentsStrip.qml", "Glass.qml", "Tint.qml", "Marquee.qml", "Sounds.qml"]) {
                var c = Qt.createComponent(Qt.resolvedUrl("../../native/qml/" + f));
                expect(c.status === Component.Ready, f + " compiles: " + c.errorString());
            }
            w.lifeCycleStatus.launch({ params: { state: JSON.parse(JSON.stringify(sample)), token: "t0ken", port: 47999, frame: "" } });
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
            v.ok(); expect(v.picked === -1, "OK puts it down, and saves");
            // before that save is answered (the mock takes 600 ms), it's picked up and moved again
            v.focusIndex = 2; v.ok(); v.move(0, 1); movedTo = v.picked;
            expect(movedTo > 2 && order(v)[movedTo] === "network", "picked up again and moved: " + order(v).join());
            break;
        case 4:
        case 5:
        case 6:
            break; // the first save's answer comes, and must not undo the move
        case 7:
            expect(v.picked === movedTo && order(v)[movedTo] === "network", "an older answer doesn't undo a newer move: " + order(v).join());
            v.ok(); expect(v.picked === -1, "put down again");
            break;
        case 8:
        case 9:
        case 10:
            break;
        case 11:
            log(function (got) {
                var saves = got.filter(function (g) { return g.body.action === "layout"; });
                expect(saves.length === 2, "two saves: " + saves.length);
                expect(got.every(function (g) { return g.token === "t0ken"; }), "every request carries the token");
                var last = saves.length ? saves[saves.length - 1].body.controls.map(function (c) { return c.id; }) : [];
                expect(last[movedTo] === "network" && last.join() === order(v).join(), "the last save is the order on screen: " + last.join());
            });
            break;
        case 12:
            v.focusIndex = 0; v.longPress(); expect(!!v.sheet && v.sheet.cid === "power", "options for Power Off");
            v.focusIndex = 2; // the pointer moving behind the sheet
            v.sheetIndex = v.sheet.items.length - 2; expect(v.sheet.items[v.sheetIndex].act === "remove", "Remove");
            v.sheetOk();
            expect(v.find("power") < 0 && v.find("screenOff") >= 0, "Remove takes the control the sheet was for, not the one with the focus");
            v.focusIndex = v.focusCount() - 2; v.ok(); expect(v.listKind === 3 && v.gallery.length >= 2, "the + slot: the gallery, without what's placed");
            v.back(); v.focusIndex = v.focusCount() - 1; v.ok(); expect(!!v.sheet && v.sheet.items[0].act === "reset", "Reset asks first");
            v.back(); v.back(); expect(!v.editing, "Back: done editing");
            break;
        case 13: // one size: no choice of it. Wi-Fi's options are Remove and Cancel, on Cancel; Settings (one size,
            // and the only way in) has nothing to choose, and shakes
            v.longPress(); v.sheetOk();
            v.focusIndex = v.find("network"); v.longPress();
            expect(!!v.sheet && v.sheet.items.map(function (i) { return i.act; }).join() === "remove,cancel" && v.sheetIndex === 1,
                   "a control with one size: Remove and Cancel, with Cancel focused");
            v.back();
            v.focusIndex = v.find("settings"); v.longPress();
            expect(!v.sheet, "Settings: nothing to choose, no sheet");
            v.back();
            // no room: 28 units fill the seven rows, so nothing grows and nothing's added, and it says so
            var full = JSON.parse(JSON.stringify(sample));
            ["soundMode", "brightness", "energySaving", "gameOptimizer"].forEach(function (id) {
                full.controls.push({ id: id, size: "wide", title: id, kind: "stepper", icon: "controls", value: "", on: null, available: true });
            });
            full.controls.push({ id: "recents", size: "small", title: "Recents", kind: "opens", icon: "recents", value: "", on: null, available: true });
            v.load(full);
            expect(v.placed() === 15 && v.rows === 7, "28 units: every control on the seven rows (" + v.placed() + " on " + v.rows + ")");
            v.longPress(); v.sheetOk();
            var at = v.find("inputs");
            v.focusIndex = at;
            v.openSheet("Inputs", "", [{ label: "Wide", act: "size", arg: "wide" }], 0, "inputs");
            v.sheetOk();
            expect(v.problem === "No room for it at that size" && v.rectOf(at).w === v.unit && v.placed() === 15,
                   "a size that would push one off the grid isn't taken: " + v.problem);
            v.move(1, 0); expect(v.problem === "", "what went wrong goes with the focus");
            v.add({ id: "x", title: "Extra", kind: "stepper", icon: "controls", sizes: ["wide", "small"] });
            expect(v.problem === "No room for Extra" && v.find("x") < 0, "no room to add one: it isn't added: " + v.problem);
            v.back(); expect(!v.editing, "done editing");
            break;
        case 14:
            w.lifeCycleStatus.launch({ params: {} }); // the Settings key again
            break;
        case 15:
        case 16:
            break; // it animates out
        case 17:
            expect(w.exited, "the Settings key again closes it");
            log(function (got) {
                expect(got.some(function (g) { return g.url === "/api/overlay" && g.body.event === "closed"; }), "the service hears it closed");
            });
            relaunch({ list: "inputs" }); // the Input key: a window of its own, as on the TV
            break;
        case 18:
            expect(v && v.listKind === 2 && v.listOnly, "the Input key: the inputs alone");
            if (!v) return finish();
            v.back(); expect(v.closing, "Back closes them, as they came alone");
            break;
        case 19:
        case 20:
            break; // it animates out
        case 21:
            expect(w.exited, "and it's gone");
            relaunch({});
            break;
        case 22:
            w.lifeCycleStatus.launch({ params: { again: "input" } });
            expect(v.listKind === 2 && !v.listOnly, "the Input key over the controls: their inputs");
            v.back(); expect(v.listKind === 0 && !v.closing, "Back: the controls again");
            w.lifeCycleStatus.launch({ params: { again: "input" } });
            w.lifeCycleStatus.launch({ params: { again: "input" } });
            expect(v.closing, "the Input key again, with the inputs on show: it closes");
            break;
        case 23:
        case 24:
            break;
        case 25:
            expect(w.exited, "closed");
            relaunch(null);
            win.item.lifeCycleStatus.launch({ params: { again: "input", port: 47999, token: "t0ken" } });
            expect(!win.item.view, "the Input key again, but the panel has gone: nothing opens");
            break;
        case 26:
            expect(w.exited, "and the window goes");
            relaunch({});
            win.item.lifeCycleStatus.close(undefined); // LG closes it, and gives no reason
            break;
        case 27:
        case 28:
            break; // it animates out
        case 29:
            expect(w.exited, "LG closing it without a reason closes it");
            relaunch({});
            break;
        case 30: // the TV going to sleep: no animation, but the window waits for the service to hear it closed
            w.lifeCycleStatus.close({ reason: "powerSuspend" });
            expect(!w.exited, "the window doesn't go before the service has heard it closed");
            break;
        case 31:
            expect(w.exited, "then it goes");
            log(function (got) {
                var last = got[got.length - 1];
                expect(!!last && last.body.event === "closed", "the service heard it closed: " + JSON.stringify(last));
            });
            break;
        case 32:
            relaunch(null);
            win.item.lifeCycleStatus.launch(undefined); // a launch with nothing in it
            expect(!win.item.view, "a launch without a payload opens nothing");
            break;
        case 33:
            expect(win.item.exited, "and its window goes");
            relaunch({}); // Settings over the video
            break;
        case 34:
            v.activate(v.find("settings"));
            break;
        case 35:
            expect(!!s, "Settings' panel is made");
            if (!s) return finish();
            expect(v.listKind === 4 && v.menu() === s, "it takes the controls' place");
            expect(s.side && s.cat === 0 && s.pages.length === 6, "on its list of pages: " + s.pages.length);
            expect(s.hint.indexOf("Back closes Settings") >= 0, "the hint says what Back does: " + s.hint);
            s.move(0, 1); expect(s.cat === 1 && s.side, "Down shows the next page");
            s.move(0, -1); expect(s.cat === 0, "Up the one before");
            s.move(0, -1); expect(s.cat === 0, "Up at the top: nothing that way");
            s.move(-1, 0); expect(s.side, "Left on the list: nothing");
            s.move(1, 0); expect(!s.side && s.index === 0, "Right goes into the page");
            var first = s.rowItems.itemAt(0);
            expect(!!first && first.width > 900 && first.visible && first.opacity === 1, "its rows are drawn: " + (first ? first.width + " wide, opacity " + first.opacity : "none"));
            expect(s.hint.indexOf("change") >= 0, "the hint says Left and Right change it: " + s.hint);
            s.move(1, 0); // Right: a step on
            break;
        case 36:
            expect(s.row.value === "6 min", "Right changes the row: " + s.row.value);
            s.move(-1, 0); s.ok(); // Left, then OK: back and on again
            break;
        case 37:
            expect(!s.side, "changing a row stays on it");
            log(function (got) {
                var rows = got.filter(function (g) { return g.url === "/api/settings" && g.body.row; }).map(function (g) { return g.body.row + ":" + g.body.dir; });
                expect(rows.join() === "dimAfter:1,dimAfter:-1,dimAfter:1", "the panel asks for the row by its id: " + rows.join());
            });
            for (var i = 0; i < 7; i++) s.move(0, 1);
            expect(s.index === 7 && s.first > 0, "a long page scrolls: row " + s.index + ", first " + s.first);
            s.move(0, 1); expect(s.index === 7, "Down on the last row: nothing");
            break;
        case 38:
            s.move(-1, 0); // (it's a stepper: this changes it, and doesn't leave the page)
            expect(!s.side, "Left on a row that steps stays in the page");
            v.back(); expect(s.side, "Back: the list of pages");
            s.showPage(1); s.ok(); expect(!s.side && s.row.kind === "check", "App Tiles: into its looks");
            s.move(-1, 0); expect(s.side, "Left on a row that doesn't step: the list");
            s.ok(); s.move(0, 1); s.ok(); // Tinted
            break;
        case 39:
            expect(s.rows.length === 3 && s.row.id === "look-tinted" && s.row.on, "picking Tinted ticks it, and its colours come: " + s.rows.length);
            s.move(0, 1); expect(s.row.kind === "tint", "its colours are a row");
            s.showPage(0); s.change({ id: "refused" }, 1);
            break;
        case 40:
            expect(s.problem === "The TV didn’t do that", "what the TV refused says so: " + s.problem);
            s.move(0, 1); expect(s.problem === "", "and that goes with the focus");
            v.keysTakeOver();
            expect(!v.pointerMoved(v, 100, 100), "a pointer lying still takes no focus");
            expect(!v.pointerMoved(v, 110, 110), "nor one that trembles");
            expect(v.pointerMoved(v, 180, 100), "one that really moves does");
            expect(v.pointerMoved(v, 181, 100), "and keeps it");
            v.keysTakeOver(); expect(!v.pointerMoved(v, 181, 100), "until an arrow takes it back");
            v.back(); expect(v.listKind === 0 && !v.closing, "Back from the list: the controls");
            v.back(); expect(v.closing, "Back again closes it");
            break;
        case 41:
        case 42:
            break; // it animates out
        case 43:
            expect(w.exited, "closed");
            relaunch({}); // the Accessibility tile: that page, with what it changes following at once
            break;
        case 44:
            v.openSettings("accessibility");
            break;
        case 45:
            expect(v.listKind === 4 && !s.side && s.page.id === "accessibility", "Accessibility opens on its page: " + s.page.id);
            s.ok(); s.move(0, 1); s.ok();
            break;
        case 46:
            expect(v.still, "Reduce Motion applies at once");
            expect(v.solid, "and Reduce Transparency");
            expect(v.cc.reduceMotion === true, "and it's kept for the panel's next answers");
            s.ok(); s.move(0, -1); s.ok(); // and back
            break;
        case 47:
            expect(!v.still && !v.solid, "turned off again too");
            s.showPage(3); s.ok();
            expect(s.row.kind === "home" && s.hint.indexOf("OK opens Home+") >= 0, "a row that leaves the video says so: " + s.hint);
            expect(s.page.body.split("\n").length === 2, "the page's words");
            s.ok();
            expect(v.closing, "it closes");
            break;
        case 48:
        case 49:
            break;
        case 50:
            expect(w.exited, "closed");
            log(function (got) {
                expect(got.some(function (g) { return g.body.event === "open" && g.body.target === "accounts"; }), "and Home+ opens on that page");
            });
            relaunch({}); // LG's settings apps
            break;
        case 51:
            v.openSettings("");
            break;
        case 52:
            s.showPage(4); s.ok(); s.ok();
            expect(v.closing, "OK on an app closes it");
            break;
        case 53:
        case 54:
            break;
        case 55:
            log(function (got) {
                expect(got.some(function (g) { return g.body.event === "open" && g.body.app === "com.palm.app.settings"; }), "and opens the app");
            });
            relaunch({}); // Recents
            break;
        case 56:
            v.openRecents();
            break;
        case 57:
            expect(!!r, "Recents' strip is made");
            if (!r) return finish();
            expect(v.listKind === 5 && v.menu() === r, "it takes the controls' place");
            expect(r.index === 1, "the focus starts on the app before the one in front: " + r.index);
            r.move(1, 0); expect(r.index === 2, "Right picks the next");
            r.move(1, 0); expect(r.index === 2, "Right on the last: nothing");
            r.move(0, -1); // Up closes it
            break;
        case 58:
            break; // it flies off
        case 59:
            expect(r.index === 1, "closed, the focus moves on: " + r.index);
            log(function (got) {
                expect(got.some(function (g) { return g.url === "/api/close" && g.body.id === "hdmi1"; }), "Up closes that app");
            });
            r.move(0, 1); expect(r.closeAll, "Down reaches Close All");
            r.move(1, 0); expect(r.closeAll, "and stays there");
            r.ok(); expect(!!v.sheet && v.sheet.items[0].act === "closeAll" && v.sheetIndex === 1, "Close All asks first, on Cancel");
            v.back(); expect(!v.sheet && v.listKind === 5, "Back: Recents again");
            r.ok(); v.sheetIndex = 0; v.ok();
            break;
        case 60:
            break; // they fly off
        case 61:
            log(function (got) {
                expect(got.some(function (g) { return g.url === "/api/close" && g.body.all === true; }), "Close All closes them all");
            });
            break;
        case 62:
            break; // nothing left to show
        case 63:
            expect(v.listKind === 0, "with none left, the controls again");
            v.openRecents();
            break;
        case 64:
            expect(r.index === 1, "open again: " + r.index);
            r.ok();
            expect(v.closing, "OK on an app closes it");
            break;
        case 65:
        case 66:
            break;
        case 67:
            log(function (got) {
                expect(got.some(function (g) { return g.body.event === "open" && g.body.app === "stremio"; }), "and opens the app");
            });
            relaunch({}); // the app in front: OK only closes the strip
            break;
        case 68:
            v.openRecents();
            break;
        case 69:
            r.move(-1, 0); r.ok();
            expect(v.closing, "OK on the app in front closes it");
            break;
        case 70:
        case 71:
            break;
        case 72:
            log(function (got) {
                expect(!got.some(function (g) { return g.body.event === "open" && g.body.app === "youtube"; }), "and asks nothing of the service");
            });
            finish();
            break;
        }
    }

    function relaunch(extra) { // a new window (LG's compositor makes one each time), launched with the sample and `extra`
        win.source = "";
        win.source = Qt.resolvedUrl("../../native/qml/ControlCenter.qml");
        if (!extra) return;
        var params = { state: JSON.parse(JSON.stringify(sample)), token: "t0ken", port: 47999, frame: "" };
        for (var k in extra) params[k] = extra[k];
        win.item.lifeCycleStatus.launch({ params: params });
    }

    function order(v) { // the controls' ids, in order
        var ids = [];
        for (var k = 0; v.idAt(k); k++) ids.push(v.idAt(k));
        return ids;
    }

    function log(then) { // what the mock API got
        var x = new XMLHttpRequest();
        x.onreadystatechange = function () {
            if (x.readyState !== XMLHttpRequest.DONE) return;
            var got = [];
            try { got = JSON.parse(x.responseText); } catch (e) { fail("the mock API's log: " + x.responseText); }
            then(got);
        };
        x.open("GET", "http://127.0.0.1:47999/log");
        x.send();
    }

    function finish() {
        driver.stop();
        console.log(failures.length ? "qml-check: " + failures.length + " failed" : "qml-check: ok");
        Qt.exit(failures.length ? 1 : 0);
    }
}
