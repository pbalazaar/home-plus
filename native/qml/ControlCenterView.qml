import QtQuick 2.4
import QtGraphicalEffects 1.0

// The Control Center over other apps (opened by ControlCenter.qml): the controls the owner placed,
// on Home+'s four-unit grid under its clock, drawn as Home+ draws them (main.c: draw_control_center,
// draw_clock; the same sizes), with tvOS 26's states: what's on has a cyan glyph, a switch that's
// off sits on a pale disc. The glass is the video behind it, blurred (Glass.qml). They drop in.
// Editing works as Home+'s: hold OK for Edit Controls; then OK picks one up and puts it down,
// holding OK gives its sizes and Remove, the + slot adds one, Reset starts over.
FocusScope {
    id: view
    anchors.fill: parent

    property var cc: ({})   // the service's Control Center state (control.js: state())
    property var host       // ControlCenter.qml: call()
    signal closed()

    // Home+'s grid and clock (main.c: CC_U, CC_G, cc_grid_x/y, RIGHT, TOP_Y, BAR)
    readonly property int unit: 91
    readonly property int gap: 20
    readonly property int rightEdge: 1872
    readonly property int gridX: rightEdge - 4 * unit - 3 * gap
    readonly property int gridY: 52 + 56 + 26
    readonly property int gridW: 4 * unit + 3 * gap
    readonly property color cyan: "#10e8f4"   // tvOS 26's colour for what's on
    readonly property color teal: "#0089ad"   // the same, on a focused (white) control
    readonly property color ink: "#1c1c1e"    // a focused control's glyph and text
    readonly property color white: "#f5f5f7"  // a focused control
    readonly property color red: "#ff5a50"    // what can't be undone (Home+'s sheets)

    readonly property bool still: !!cc.reduceMotion      // Settings -> Accessibility: Reduce Motion
    readonly property bool solid: !!cc.reduceTransparency // ... and Reduce Transparency

    property int focusIndex: 0     // a control (0..n-1); while editing, the + slot (n) and Reset (n + 1)
    property int listKind: 0       // in the Control Center's place: 1 picture modes, 2 inputs, 3 the gallery
    property int listIndex: 0
    property var listItems: []
    property bool closing: false
    property bool opening: true    // the first drop; controls placed later just appear
    property string problem: ""    // a control that didn't work says why, under the grid

    // editing (main.c: cc_edit_ok, cc_move_picked, cc_long_press)
    property bool editing: false
    property int picked: -1        // the control being moved
    property int plusCol: -1       // the + slot: the first free unit after the last control
    property int plusRow: -1
    property int rows: 0           // the rows in use (with the + slot while editing)
    property real wobble: 0        // seconds, while editing
    property int edits: 0          // local changes to the layout: an answer to an older request doesn't undo them

    // a sheet over it, as Home+'s: {title, body, items: [{label, act, arg, destructive, check}]}
    property var sheet: null
    property int sheetIndex: 0

    FontLoader { id: medium; source: "../assets/fonts/Inter-Medium.ttf" }
    FontLoader { id: semi; source: "../assets/fonts/Inter-SemiBold.ttf" }

    // ---- the picture behind, for the glass: the service's frames of the video while this is open
    // (see the service; the TV gives about five a second, each a fifth of a second old), loaded in
    // turn into two images, blurred hard and small so the glass carries the video's colours rather
    // than its detail (which would show the delay), the newer faded in over the older. The service
    // answers an ask for the next frame when it's taken, so each shows at once, and only new ones
    // load. A blur runs when its frame comes, not every frame. No frame (none yet, or the video
    // can't be captured): the glass is clear over what's there.
    property real fade: 0          // 0: frame A on show, 1: frame B
    property int frameN: 0         // the service's number for the picture on show (or none)
    property bool hasFrame: false
    property bool frameDue: false  // a new one came while the last was fading in
    property var asking: ({ on: true }) // off once it closes: an answer that comes later is let be
    Component.onDestruction: asking.on = false
    function askFrame() {
        if (closing || solid || !cc.frame) return;
        var a = asking;
        host.call("overlay", { event: "frame", after: frameN }, function (r) {
            if (!a.on) return;
            if (r.n === undefined) { askAgain.start(); return; } // (no answer: in a moment)
            if (r.n !== frameN) {
                frameN = r.n;
                if (r.picture) loadFrame(); else hasFrame = false;
            }
            askFrame();
        });
    }
    Timer { id: askAgain; interval: 500; onTriggered: view.askFrame() }
    function loadFrame() {
        if (fade > 0.001 && fade < 0.999) { frameDue = true; return; } // after this fade
        frameDue = false;
        (fade < 0.5 ? frameB : frameA).source = cc.frame + "?n=" + frameN;
    }
    Image {
        id: frameA
        visible: false; width: 160; height: 90; sourceSize: Qt.size(160, 90); cache: false; asynchronous: true
        onStatusChanged: if (status === Image.Ready) view.show(0); else if (status === Image.Error) view.hasFrame = false
    }
    Image {
        id: frameB
        visible: false; width: 160; height: 90; sourceSize: Qt.size(160, 90); cache: false; asynchronous: true
        onStatusChanged: if (status === Image.Ready) view.show(1); else if (status === Image.Error) view.hasFrame = false
    }
    NumberAnimation { id: fadeTo; target: view; property: "fade"; duration: 90; easing.type: Easing.InOutQuad
        onStopped: if (view.frameDue) view.loadFrame() }
    function show(which) { // the first frame at once; then each fades in
        if (!hasFrame) fade = which;
        else { fadeTo.to = which; fadeTo.restart(); }
        hasFrame = true;
    }
    FastBlur { id: blurA; width: 160; height: 90; source: frameA; radius: 32; visible: false }
    FastBlur { id: blurB; width: 160; height: 90; source: frameB; radius: 32; visible: false }
    ShaderEffectSource { id: glassA; sourceItem: blurA; width: 160; height: 90; visible: false; smooth: true }
    ShaderEffectSource { id: glassB; sourceItem: blurB; width: 160; height: 90; visible: false; smooth: true }

    // frames a second, for tuning (cc.debugFps): a running animation changes its value once a frame
    property int fps: 0
    property int frames: 0
    property real phase: 0
    onPhaseChanged: frames++
    NumberAnimation on phase { running: !!view.cc.debugFps; from: 0; to: 1; duration: 1000; loops: Animation.Infinite }
    Timer { interval: 1000; running: !!view.cc.debugFps; repeat: true; onTriggered: { view.fps = view.frames; view.frames = 0; } }
    property int lastKey: 0
    Text { visible: !!view.cc.debugFps; x: 60; y: 60; color: "#7CFC00"; font.pixelSize: 40; text: view.fps + " fps  ·  key " + view.lastKey.toString(16) + "  ·  sounds " + (view.sounds ? (view.sounds.ready ? "ready" : "loading") : "none"); z: 100 }

    ListModel { id: tiles }

    // ---- the state

    // what a tile shows: a control that's on (`on` true) has a cyan glyph; a switch that's off, a disc
    function tileOf(c) {
        return {
            cid: c.id, size: c.size || "small", kind: c.kind || "", icon: c.icon || "controls", title: c.title || "",
            name: c.name || c.title || "", // the whole name (the tile's may be short): its options' title
            value: c.value || "", active: c.on === true, disc: c.kind === "switch" && c.on === false,
            available: c.available !== false
        };
    }

    // the service's answer: in place when it's the same controls (only their values change)
    function load(state) {
        var list = state.controls || [], same = list.length === tiles.count, i;
        for (i = 0; same && i < list.length; i++) same = tiles.get(i).cid === list[i].id && tiles.get(i).size === list[i].size;
        if (same) {
            for (i = 0; i < list.length; i++) tiles.set(i, tileOf(list[i]));
        } else {
            tiles.clear();
            for (i = 0; i < list.length; i++) {
                var o = tileOf(list[i]);
                o.col = -1; o.row = -1; o.w = 1; o.h = 1;
                tiles.append(o);
            }
        }
        relayout();
        if (focusIndex >= focusCount()) focusIndex = 0;
    }

    // Places the controls in order on the four-unit grid, each in the first space it fits; the +
    // slot takes the first free unit after the last of them (main.c: cc_pack). Row -1: no room.
    function relayout() {
        var used = [], last = -1, i, y, x, yy, xx;
        for (y = 0; y < 6; y++) used.push([0, 0, 0, 0]);
        for (i = 0; i < tiles.count; i++) {
            var t = tiles.get(i), w = t.size === "small" ? 1 : 2, h = t.size === "large" ? 2 : 1, row = -1, col = -1;
            for (y = 0; y + h <= 6 && row < 0; y++) {
                for (x = 0; x + w <= 4 && row < 0; x++) {
                    var fits = true;
                    for (yy = y; yy < y + h; yy++)
                        for (xx = x; xx < x + w; xx++) if (used[yy][xx]) fits = false;
                    if (!fits) continue;
                    for (yy = y; yy < y + h; yy++)
                        for (xx = x; xx < x + w; xx++) used[yy][xx] = 1;
                    row = y;
                    col = x;
                    if (y * 4 + x > last) last = y * 4 + x;
                }
            }
            tiles.set(i, { col: col, row: row, w: w, h: h });
        }
        plusRow = plusCol = -1;
        for (var u = last + 1; u < 24; u++) {
            if (!used[Math.floor(u / 4)][u % 4]) {
                plusRow = Math.floor(u / 4);
                plusCol = u % 4;
                break;
            }
        }
        var n = 0;
        for (i = 0; i < tiles.count; i++) if (tiles.get(i).row >= 0 && tiles.get(i).row + tiles.get(i).h > n) n = tiles.get(i).row + tiles.get(i).h;
        if (editing && plusRow + 1 > n) n = plusRow + 1;
        rows = n;
    }

    function focusCount() { return tiles.count + (editing ? 2 : 0); }

    // where focusable thing i is (main.c: cc_rect): a control, the + slot, Reset
    function rectOf(i) {
        var n = tiles.count;
        if (i >= 0 && i < n) {
            var t = tiles.get(i);
            if (t.row < 0) return null;
            return { x: gridX + t.col * (unit + gap), y: gridY + t.row * (unit + gap),
                     w: t.w * unit + (t.w - 1) * gap, h: t.h * unit + (t.h - 1) * gap };
        }
        if (i === n && editing && plusRow >= 0) return { x: gridX + plusCol * (unit + gap), y: gridY + plusRow * (unit + gap), w: unit, h: unit };
        if (i === n + 1 && editing) return { x: gridX, y: gridY + rows * (unit + gap) + 44, w: gridW, h: 64 };
        return null;
    }

    function catalogEntry(id) {
        var cat = cc.catalog || [];
        for (var i = 0; i < cat.length; i++) if (cat[i].id === id) return cat[i];
        return null;
    }

    // Home+'s focus sounds, unless they're off in its Settings (Sounds.qml)
    property var sounds: null
    onFocusIndexChanged: if (sounds) sounds.tick()
    onListIndexChanged: if (sounds) sounds.tick()
    onSheetIndexChanged: if (sounds) sounds.tick()

    Component.onCompleted: {
        if (cc.sounds !== false) {
            var sc = Qt.createComponent("Sounds.qml");
            if (sc.status === Component.Ready) sounds = sc.createObject(view);
        }
        load(cc);
        var net = find("network"); // nearest the clock, and harmless (unlike Power Off)
        if (net >= 0) focusIndex = net;
        idle.restart();
        settled.start();
        askFrame();
    }
    Timer { id: settled; interval: 1200; onTriggered: view.opening = false }

    function find(id) {
        for (var i = 0; i < tiles.count; i++) if (tiles.get(i).cid === id) return i;
        return -1;
    }
    function idAt(i) { return i >= 0 && i < tiles.count ? tiles.get(i).cid : ""; }

    // ---- what OK does (main.c: cc_activate)

    function run(body, then) {
        problem = "";
        var asked = edits;
        host.call("control", body, function (r) {
            if (r.returnValue === false) {
                problem = r.errorText || "That didn't work";
                var t = grid.itemAt(focusIndex);
                if (t) t.shake();
                return;
            }
            if (r.controls && asked === edits && picked < 0) { // the new state (unless moved on since), with what only the opening said
                r.frame = cc.frame;
                r.clock24 = cc.clock24;
                r.debugFps = cc.debugFps;
                r.reduceMotion = cc.reduceMotion;
                r.reduceTransparency = cc.reduceTransparency;
                cc = r;
                load(r);
            }
            if (then) then(r);
        });
    }

    function overlay(params) {
        host.call("overlay", params);
    }

    function ok() {
        if (sounds) sounds.select();
        if (sheet) sheetOk();
        else if (listKind === 3) galleryOk();
        else if (listKind) listOk();
        else if (editing) editOk();
        else activate(focusIndex);
    }

    function activate(i) {
        if (i < 0 || i >= tiles.count || closing) return;
        var t = tiles.get(i), item = grid.itemAt(i);
        if (!t.available) {
            if (item) item.shake();
            return;
        }
        switch (t.cid) {
        case "power": run({ action: "powerOff" }); close(); return;
        case "screenOff": run({ action: "screenOff" }); close(); return;
        case "pictureMode": openList(1); return;
        case "inputs": openList(2); return;
        case "network": case "recents": case "accessibility": case "settings":
            overlay({ event: "open", target: t.cid });
            close();
            return;
        }
        if (t.kind === "stepper" || t.kind === "switch") {
            run({ action: "run", value: t.cid }, function () {
                var again = grid.itemAt(i);
                if (again && t.kind === "stepper" && again.small) again.flash();
            });
        }
    }

    // ---- editing

    function editOk() {
        var n = tiles.count;
        if (picked >= 0) {
            picked = -1;
            save();
        } else if (focusIndex < n) {
            picked = focusIndex;
            edits++;
        } else if (focusIndex === n) {
            if (plusRow >= 0) openGallery();
        } else {
            openSheet("Reset Control Center?", "The controls go back to how they were at first.",
                      [{ label: "Reset", act: "reset", destructive: true }, { label: "Cancel", act: "cancel" }], 1);
        }
    }

    // holding OK: Edit Controls; while editing, the control's sizes and Remove
    function longPress() {
        if (sheet || listKind || picked >= 0 || closing) return;
        if (!editing) {
            openSheet("Control Center", "", [{ label: "Edit Controls", act: "edit" }, { label: "Cancel", act: "cancel" }], 0);
            return;
        }
        if (focusIndex >= tiles.count) return;
        var t = tiles.get(focusIndex), e = catalogEntry(t.cid), items = [];
        var names = { small: "Small", wide: "Wide", large: "Large" }, sizes = e ? e.sizes : [t.size];
        for (var k = 0; k < sizes.length; k++)
            items.push({ label: names[sizes[k]] || sizes[k], act: "size", arg: sizes[k], check: sizes[k] === t.size });
        if (!e || !e.pinned) items.push({ label: "Remove", act: "remove", destructive: true }); // not Settings: the only way in
        items.push({ label: "Cancel", act: "cancel" });
        openSheet(t.name, "", items, 0, t.cid);
    }

    // the picked control moves through the order; up and down, to the control in that row nearest
    // its middle (main.c: cc_move_picked)
    function movePicked(dx, dy) {
        var i = picked, to = i + dx;
        if (dy) {
            var c = tiles.get(i), row = dy > 0 ? c.row + c.h : c.row - 1, best = -1, bd = 1e9, cx = c.col + c.w / 2;
            for (var k = 0; k < tiles.count; k++) {
                var o = tiles.get(k);
                if (k === i || o.row < 0 || row < o.row || row >= o.row + o.h) continue;
                var d = Math.abs(o.col + o.w / 2 - cx);
                if (d < bd) { bd = d; best = k; }
            }
            if (best < 0 && dy > 0) best = tiles.count - 1; // below the last row: the end
            if (best < 0) return;
            to = best;
        }
        if (to < 0 || to >= tiles.count || to === i) return;
        tiles.move(i, to, 1);
        picked = focusIndex = to;
        edits++;
        relayout();
    }

    // saves the list as it is on screen (the service answers with the whole state)
    function save() {
        var list = [];
        for (var i = 0; i < tiles.count; i++) list.push({ id: tiles.get(i).cid, size: tiles.get(i).size });
        run({ action: "layout", controls: list });
    }

    // a control from the gallery, at the end in the first of its sizes that fits
    function add(e) {
        var o = tileOf({ id: e.id, size: e.sizes[0], kind: e.kind, icon: e.icon, title: e.title, available: true });
        o.col = -1; o.row = -1; o.w = 1; o.h = 1;
        tiles.append(o);
        listKind = 0;
        edits++;
        if (!fit(tiles.count - 1, e.sizes)) {
            tiles.remove(tiles.count - 1, 1);
            relayout();
            problem = "No room for " + e.title;
            return;
        }
        focusIndex = tiles.count - 1;
        save();
    }

    // gives control i the first of `sizes` that leaves it on screen; false (and as it was) if none does
    function fit(i, sizes) {
        var was = tiles.get(i).size;
        for (var k = 0; k < sizes.length; k++) {
            tiles.setProperty(i, "size", sizes[k]);
            relayout();
            if (tiles.get(i).row >= 0) return true;
        }
        tiles.setProperty(i, "size", was);
        relayout();
        return false;
    }

    // ---- sheets

    function openSheet(title, body, items, index, cid) {
        sheet = { title: title, body: body, items: items, cid: cid || "" }; // cid: the control it's about
        sheetIndex = index;
    }

    function sheetOk() {
        var it = sheet.items[sheetIndex], at = sheet.cid ? find(sheet.cid) : focusIndex;
        sheet = null;
        if (!it || at < 0) return;
        if (it.act === "edit") {
            editing = true;
            picked = -1;
            relayout();
        } else if (it.act === "size") {
            edits++;
            var keep = tiles.get(at).size;
            if (!fit(at, [it.arg])) { // no room at that size
                fit(at, [keep]);
                problem = "No room for it at that size";
                return;
            }
            save();
        } else if (it.act === "remove") {
            edits++;
            tiles.remove(at, 1);
            relayout();
            if (focusIndex >= tiles.count) focusIndex = Math.max(0, tiles.count - 1);
            save();
        } else if (it.act === "reset") {
            run({ action: "reset" });
        }
    }

    // ---- the lists (main.c: cc_list_open, cc_list_ok)

    function openList(kind) {
        var items = [], i;
        if (kind === 1) {
            var p = cc.picture || {}, modes = p.modes || [];
            for (i = 0; i < modes.length; i++)
                items.push({ id: modes[i].id, label: modes[i].label, detail: "", ticked: modes[i].id === p.mode });
        } else {
            var inputs = cc.inputs || [];
            for (i = 0; i < inputs.length; i++)
                items.push({ id: inputs[i].appId, label: inputs[i].label, detail: inputs[i].plugged ? inputs[i].device : "Nothing Connected", ticked: false });
        }
        listItems = items;
        listIndex = 0;
        for (i = 0; i < items.length; i++) if (items[i].ticked) listIndex = i;
        listKind = kind;
    }

    function listOk() {
        var it = listItems[listIndex];
        if (!it) return;
        if (listKind === 1) {
            run({ action: "pictureMode", value: it.id }, function () { openList(1); });
        } else {
            overlay({ event: "open", app: it.id });
            close();
        }
    }

    // ---- the gallery: what can be added, by group, as round buttons with their names under them;
    // wider than the grid, its right edge the grid's (main.c: cc_gallery_build; GAL_W, GAL_PAD,
    // GAL_HEAD, GAL_GROUP, GAL_GROUP_TOP, GAL_BTN, GAL_ROW, GAL_NAME_PX, GAL_NAME_GAP)

    readonly property int galW: 680
    readonly property int galPad: 24
    readonly property int galHead: 66
    readonly property int galGroup: 52 // a group's name, near the top: room between it and its controls
    readonly property int galGroupTop: 6
    readonly property int galBtn: 72
    readonly property int galRow: 144
    readonly property int galNamePx: 20
    readonly property int galNameGap: 16 // between neighbours' names: a longer one takes two lines
    property var gallery: []        // [{entry, x, y}]
    property var galleryHeads: []   // [{text, y}]
    property int galleryH: 0

    function openGallery() {
        var cat = cc.catalog || [], placed = {}, groups = [], byGroup = {}, i;
        for (i = 0; i < tiles.count; i++) placed[tiles.get(i).cid] = true;
        for (i = 0; i < cat.length; i++) {
            var e = cat[i];
            if (!e.available || placed[e.id]) continue;
            if (!byGroup[e.group]) { byGroup[e.group] = []; groups.push(e.group); }
            byGroup[e.group].push(e);
        }
        var cw = (galW - 2 * galPad) / 4, y = galHead, items = [], heads = [];
        for (var g = 0; g < groups.length; g++) {
            heads.push({ text: groups[g], y: y });
            y += galGroup;
            var list = byGroup[groups[g]];
            for (i = 0; i < list.length; i++) {
                if (i && i % 4 === 0) y += galRow;
                items.push({ entry: list[i], x: galPad + (i % 4) * cw + (cw - galBtn) / 2, y: y });
            }
            y += galRow;
        }
        gallery = items;
        galleryHeads = heads;
        galleryH = (items.length ? y : galHead + 60) + 4;
        listIndex = 0;
        listKind = 3;
    }

    function galleryOk() {
        var it = gallery[listIndex];
        if (it) add(it.entry);
    }

    // ---- moving: the nearest thing that way, as tvOS's focus moves: something beside it, or within
    // 45 degrees of the way (main.c: cc_move); -1: nothing that way

    function nearest(count, rect, from, dx, dy) {
        var a = rect(from) || { x: gridX, y: gridY, w: 0, h: 0 }; // (on nothing on screen: from the grid's corner)
        var ax = a.x + a.w / 2, ay = a.y + a.h / 2, best = -1, bestScore = 1e9;
        for (var i = 0; i < count; i++) {
            if (i === from) continue;
            var b = rect(i);
            if (!b) continue;
            var bx = b.x + b.w / 2, by = b.y + b.h / 2;
            var along = dx ? (bx - ax) * dx : (by - ay) * dy, across = dx ? Math.abs(by - ay) : Math.abs(bx - ax);
            var beside = dx ? b.y < a.y + a.h && b.y + b.h > a.y : b.x < a.x + a.w && b.x + b.w > a.x;
            if (along <= 1 || (!beside && across > along)) continue;
            var score = along + 2.5 * across;
            if (score < bestScore) { bestScore = score; best = i; }
        }
        return best;
    }

    function move(dx, dy) {
        if (picked >= 0) {
            movePicked(dx, dy);
            return;
        }
        var to = nearest(focusCount(), rectOf, focusIndex, dx, dy);
        if (to >= 0) focusIndex = to;
        else if (dy < 0 && !editing) close(); // up past the top: it goes back into the clock
    }

    function galleryRect(i) {
        var it = gallery[i];
        return it ? { x: it.x, y: it.y, w: galBtn, h: galBtn } : null;
    }

    // ---- opening and closing

    function close() {
        if (closing) return;
        closing = true;
        asking.on = false;
        sheet = null;
        listKind = 0;
        if (picked >= 0) { picked = -1; save(); } // one being moved stays where it is now
        for (var i = 0; i < tiles.count; i++) {
            var t = grid.itemAt(i);
            if (t) t.leave();
        }
        clock.leave();
        bye.start();
    }
    Timer { id: bye; interval: 320; onTriggered: view.closed() }
    Timer { id: idle; interval: 60000; onTriggered: view.close() } // left open, untouched

    function back() {
        if (sheet) sheet = null;
        else if (listKind) listKind = 0;
        else if (picked >= 0) { picked = -1; save(); }
        else if (editing) { // done (every change is already saved)
            editing = false;
            relayout();
            if (focusIndex >= tiles.count) focusIndex = Math.max(0, tiles.count - 1);
        } else close();
    }

    function isOk(k) { return k === Qt.Key_Return || k === Qt.Key_Enter || k === Qt.Key_Select; }
    Timer { id: hold; interval: 550; onTriggered: view.longPress() } // holding OK

    Keys.onPressed: {
        idle.restart();
        lastKey = event.key;
        event.accepted = true;
        if (closing) return;
        var k = event.key, dx = k === Qt.Key_Left ? -1 : k === Qt.Key_Right ? 1 : 0, dy = k === Qt.Key_Up ? -1 : k === Qt.Key_Down ? 1 : 0;
        if (isOk(k)) {
            if (!event.isAutoRepeat) hold.restart(); // OK acts when it's let go, unless it's held
        } else if (k === 0x1200003 || k === Qt.Key_Escape || k === Qt.Key_Back) { // LG's Back (0x1200003)
            back();
        } else if (dx || dy) {
            if (sheet) {
                if (dy) sheetIndex = Math.max(0, Math.min(sheet.items.length - 1, sheetIndex + dy));
            } else if (listKind === 3) {
                var g = nearest(gallery.length, galleryRect, listIndex, dx, dy);
                if (g >= 0) listIndex = g;
            } else if (listKind) {
                if (dy) listIndex = Math.max(0, Math.min(listItems.length - 1, listIndex + dy));
            } else {
                move(dx, dy);
            }
        } else {
            event.accepted = false;
        }
    }
    Keys.onReleased: {
        if (!isOk(event.key) || event.isAutoRepeat) return;
        event.accepted = true;
        if (hold.running) {
            hold.stop();
            ok();
        }
    }

    // ---- editing: the video steps back, the controls wobble

    Rectangle {
        anchors.fill: parent
        color: "black"
        opacity: view.editDim
    }
    NumberAnimation on wobble { running: view.editing && !view.still; from: 0; to: 100000; duration: 100000000; loops: Animation.Infinite }
    // how much the picture steps back while editing: under the glass too, which shows the picture
    // (so the glass over something bright, Reset say, dims with it)
    property real editDim: editing && !closing ? 0.4 : 0
    Behavior on editDim { NumberAnimation { duration: 250 } }

    // ---- the clock, as Home+'s header while its Control Center is open (main.c: draw_clock): the
    // time on its own, and the Control Center's circle, grown and white. Over another app it's over
    // the picture itself, white over white or a channel's logo as often as not: both cast a soft
    // shadow (unseen over dark pictures), and the corner they're in dims where the picture is bright,
    // as tvOS dims behind its clear glass (a dark layer of up to 35%, none over a dark picture). With
    // no picture to go by, the corner dims a little, always.

    property real live: hasFrame && !solid ? 1 : 0 // the picture, for the corner as for the glass
    Behavior on live { NumberAnimation { duration: 240; easing.type: Easing.InOutQuad } }
    ShaderEffect {
        x: view.rightEdge - 80 - 760; y: 0; width: view.width - x; height: 80 + 560
        visible: view.live > 0
        opacity: clock.opacity * view.live
        property variant srcA: frameA // the frames themselves (not blurred: a logo would melt away)
        property variant srcB: frameB
        property real fade: view.fade
        property rect area: Qt.rect(x / 1920, y / 1080, width / 1920, height / 1080)
        property point centre: Qt.point(760 / width, 80 / height) // the clock's corner, in it
        property size reach: Qt.size(760 / width, 560 / height)
        fragmentShader: "
            varying highp vec2 qt_TexCoord0;
            uniform sampler2D srcA;
            uniform sampler2D srcB;
            uniform lowp float fade;
            uniform highp vec4 area;
            uniform highp vec2 centre;
            uniform highp vec2 reach;
            uniform lowp float qt_Opacity;
            lowp float lum(highp vec2 uv) {
                lowp vec3 c = mix(texture2D(srcA, uv).rgb, texture2D(srcB, uv).rgb, fade);
                return dot(c, vec3(0.299, 0.587, 0.114));
            }
            void main() {
                highp vec2 uv = area.xy + qt_TexCoord0 * area.zw;
                highp vec2 px = vec2(1.0 / 160.0, 1.0 / 90.0); // a pixel of a frame: four around, softened
                lowp float l = 0.25 * (lum(uv + px * vec2(-0.75, -0.75)) + lum(uv + px * vec2(0.75, -0.75))
                                     + lum(uv + px * vec2(-0.75, 0.75)) + lum(uv + px * vec2(0.75, 0.75)));
                highp float r = length((qt_TexCoord0 - centre) / reach); // 0 at the clock, 1 at the edge
                lowp float near = 1.0 - smoothstep(0.0, 1.0, r);
                gl_FragColor = vec4(0.0, 0.0, 0.0, 0.35 * smoothstep(0.3, 0.7, l) * near) * qt_Opacity;
            }"
    }
    RadialGradient {
        x: view.rightEdge - 80 - 760; y: 80 - 560; width: 2 * 760; height: 2 * 560
        horizontalRadius: width / 2; verticalRadius: height / 2
        visible: view.live < 1
        opacity: clock.opacity * (1 - view.live)
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0.38) }
            GradientStop { position: 0.45; color: Qt.rgba(0, 0, 0, 0.16) }
            GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0) }
        }
    }
    Item {
        id: clock
        property real drop: 0
        x: 0; y: 0; width: view.width; height: 160
        opacity: Math.min(1, drop * 2)
        transform: Translate { y: (1 - clock.drop) * -24 }
        function leave() { clockOut.start(); }

        Text {
            id: time
            x: view.rightEdge - 65 - width
            y: 80 - height / 2
            visible: false // drawn by its shadow, below
            color: "white"
            font.family: semi.name
            font.weight: Font.DemiBold
            font.pixelSize: 27
            text: Qt.formatTime(new Date(), view.cc.clock24 ? "hh:mm" : "h:mm AP")
            Timer { interval: 5000; running: true; repeat: true
                onTriggered: time.text = Qt.formatTime(new Date(), view.cc.clock24 ? "hh:mm" : "h:mm AP") }
        }
        DropShadow {
            anchors.fill: time
            source: time
            radius: 14; samples: 29; spread: 0.3
            verticalOffset: 1
            color: Qt.rgba(0, 0, 0, 0.6)
            transparentBorder: true
            cached: true
        }
        RectangularGlow {
            x: view.rightEdge - 28 - 29; y: 80 - 29 + 2
            width: 58; height: 58
            glowRadius: 12; spread: 0.1; cornerRadius: 29 + glowRadius
            color: "black"
            opacity: 0.4
        }
        Rectangle {
            x: view.rightEdge - 28 - 29; y: 80 - 29
            width: 58; height: 58; radius: 29
            color: view.white
            Image { id: ccGlyph; anchors.centerIn: parent; width: 34; height: 34; source: "glyphs/controls.png"; smooth: true; mipmap: true; visible: false }
            Tint { anchors.fill: ccGlyph; src: ccGlyph; color: view.ink }
        }
        NumberAnimation on drop { id: clockIn; from: 0; to: 1; duration: 320; easing.type: Easing.OutCubic }
        NumberAnimation { id: clockOut; target: clock; property: "drop"; to: 0; duration: 220; easing.type: Easing.InCubic }
    }

    // ---- the controls

    Repeater {
        id: grid
        model: tiles
        delegate: Item {
            id: tile
            property bool small: model.w === 1
            property bool large: model.h === 2
            property bool focused: index === view.focusIndex && !view.listKind && !view.sheet && !view.closing
            property bool isPicked: index === view.picked
            property real lit: focused ? 1 : 0 // focused: white
            property real lift: focused ? 1 : 0
            property real drop: 0                             // 0 above, 1 in place
            property real shakeX: 0
            property real rad: large ? 40 : height / 2
            property real ink: model.available ? 1 : 0.45    // Not Available: its glyph and words dim

            visible: model.row >= 0 // no room for it
            x: view.gridX + Math.max(0, model.col) * (view.unit + view.gap)
            y: view.gridY + Math.max(0, model.row) * (view.unit + view.gap)
            Behavior on x { enabled: !view.opening; NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
            Behavior on y { enabled: !view.opening; NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
            width: model.w * view.unit + (model.w - 1) * view.gap
            height: model.h * view.unit + (model.h - 1) * view.gap
            z: isPicked ? 3 : focused ? 2 : 1
            opacity: Math.min(1, drop * 2.5) * (view.listKind ? 0 : 1)
            Behavior on opacity { enabled: tile.drop >= 1; NumberAnimation { duration: 200 } }
            Behavior on lit { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
            Behavior on lift { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

            transform: [
                Scale {
                    origin.x: tile.width / 2; origin.y: tile.height / 2
                    xScale: (view.still ? 1 : 0.94 + 0.06 * Math.min(1, tile.drop)) * (1 + 0.06 * tile.lift) * (tile.isPicked ? 1.08 : 1)
                    yScale: xScale
                },
                Translate { // the drop, a shake, and while editing a wobble (none with Reduce Motion)
                    x: tile.shakeX + (view.editing && !view.still && !tile.isPicked ? Math.sin(view.wobble * 7 + index * 1.7) * 1.4 : 0)
                    y: (view.still ? 0 : (1 - tile.drop) * -80) + (view.editing && !view.still && !tile.isPicked ? Math.cos(view.wobble * 9.1 + index * 1.7) * 1.1 : 0)
                }
            ]

            // drops in from the clock, nearest first, settling softly; one added later just appears
            property int delay: 40 + (model.row + (3 - model.col)) * 28
            SequentialAnimation {
                id: dropIn
                PauseAnimation { duration: tile.delay }
                NumberAnimation { target: tile; property: "drop"; from: 0; to: 1; duration: 450; easing.type: view.still ? Easing.OutCubic : Easing.OutBack; easing.overshoot: 0.9 }
            }
            NumberAnimation { id: popIn; target: tile; property: "drop"; from: 0.4; to: 1; duration: 260; easing.type: Easing.OutBack }
            Component.onCompleted: if (view.opening) dropIn.start(); else popIn.start()
            SequentialAnimation {
                id: dropOut
                PauseAnimation { duration: (5 - model.row) * 12 }
                NumberAnimation { target: tile; property: "drop"; to: 0; duration: 200; easing.type: Easing.InCubic }
            }
            function leave() { dropIn.stop(); popIn.stop(); dropOut.start(); }

            SequentialAnimation {
                id: shaking // the TV said no
                NumberAnimation { target: tile; property: "shakeX"; to: 9; duration: 50 }
                NumberAnimation { target: tile; property: "shakeX"; to: -8; duration: 80 }
                NumberAnimation { target: tile; property: "shakeX"; to: 5; duration: 70 }
                NumberAnimation { target: tile; property: "shakeX"; to: 0; duration: 60 }
            }
            function shake() { if (!view.still) shaking.start(); }

            RectangularGlow { // a focused control lifts
                x: 0; y: 10; width: parent.width; height: parent.height
                glowRadius: 20; spread: 0.1; cornerRadius: tile.rad + glowRadius
                color: "#000000"
                opacity: 0.45 * Math.max(tile.lift, tile.isPicked ? 1 : 0)
                visible: opacity > 0.01
            }
            Glass {
                anchors.fill: parent
                radius: tile.rad
                sourceA: glassA; sourceB: glassB; fade: view.fade
                live: view.hasFrame
                solid: view.solid; dim: view.editDim
                screenX: tile.x
                screenY: tile.y
                visible: tile.lit < 0.99 // under white, nothing to see
            }
            Rectangle { // focused: white, as tvOS's
                anchors.fill: parent
                radius: tile.rad
                color: view.white
                opacity: tile.lit
            }
            Rectangle { // editing: the one picked up, outlined; with Reduce Motion (no wobble), all of them
                anchors.fill: parent
                radius: tile.rad
                color: "transparent"
                border.width: 2
                border.color: tile.lit > 0.5 ? Qt.rgba(0, 0, 0, 0.35) : Qt.rgba(1, 1, 1, 0.6)
                visible: tile.isPicked || (view.editing && view.still)
            }

            // the glyph: Home+'s sizes and places (a pill's on its left, a large one's above its name)
            Item {
                id: glyphBox
                property real size: tile.large ? 76 : tile.small ? 40 : 30
                width: size; height: size
                x: tile.small || tile.large ? (tile.width - size) / 2 : 18
                y: (tile.large ? tile.height * 0.42 : tile.height / 2) - size / 2
                opacity: tile.ink

                Rectangle { // a switch that's off: its glyph on a pale disc
                    anchors.centerIn: parent
                    width: glyphBox.size * (tile.large ? 1.35 : 1.4); height: width; radius: width / 2
                    color: tile.lit > 0.5 ? Qt.rgba(0, 0, 0, 0.08) : Qt.rgba(1, 1, 1, 0.24)
                    opacity: model.disc ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 180 } }
                }
                Image {
                    id: glyph
                    anchors.centerIn: parent
                    width: glyphBox.size
                    height: width
                    source: "glyphs/" + model.icon + ".png"
                    sourceSize: Qt.size(128, 128)
                    smooth: true
                    mipmap: true
                    visible: false
                }
                Tint {
                    anchors.fill: glyph
                    src: glyph
                    color: !model.available ? "white" : tile.lit > 0.5 ? (model.active ? view.teal : view.ink) : (model.active ? view.cyan : "white")
                    Behavior on color { ColorAnimation { duration: 160 } }
                }
            }

            // a pill's name over its value, after its glyph; a large one's name under its glyph
            Column {
                visible: !tile.small
                opacity: tile.ink
                x: tile.large ? 16 : 60
                width: tile.large ? tile.width - 32 : tile.width - 22 - 60
                anchors.verticalCenter: tile.large ? undefined : parent.verticalCenter
                y: tile.large ? tile.height - 26 - height : 0
                Text {
                    visible: tile.large
                    width: parent.width
                    text: model.title
                    color: tile.lit > 0.5 ? view.ink : "white"
                    font.family: semi.name
                    font.weight: Font.DemiBold
                    font.pixelSize: 24
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                }
                Marquee { // a name too long for its pill scrolls while it has the focus
                    visible: !tile.large
                    width: parent.width
                    text: model.title
                    color: tile.lit > 0.5 ? view.ink : "white"
                    font.family: semi.name
                    font.weight: Font.DemiBold
                    font.pixelSize: 19
                    running: tile.focused && !view.editing && !view.still
                }
                Marquee {
                    visible: model.value !== "" && !tile.large
                    width: parent.width
                    text: model.value
                    color: tile.lit > 0.5 ? view.ink : "white"
                    opacity: 0.72
                    font.family: medium.name
                    font.weight: Font.Medium
                    font.pixelSize: 18
                    running: tile.focused && !view.editing && !view.still
                }
            }

            // a round stepper's new value shows under it for a moment
            Text {
                id: flashText
                visible: tile.small
                anchors.horizontalCenter: parent.horizontalCenter
                y: parent.height + 8
                text: model.value
                color: "white"
                font.family: medium.name
                font.weight: Font.Medium
                font.pixelSize: 18
                opacity: 0
                style: Text.Raised
                styleColor: "#80000000"
            }
            SequentialAnimation {
                id: flashing
                NumberAnimation { target: flashText; property: "opacity"; to: 1; duration: 120 }
                PauseAnimation { duration: 1200 }
                NumberAnimation { target: flashText; property: "opacity"; to: 0; duration: 500 }
            }
            function flash() { flashing.restart(); }

            MouseArea { // the Magic Remote's pointer (not behind a sheet or a list)
                anchors.fill: parent
                hoverEnabled: true
                enabled: !view.listKind && !view.sheet && !view.closing
                onEntered: if (view.picked < 0) view.focusIndex = index
                onClicked: { view.focusIndex = index; view.ok(); }
                onPressAndHold: { view.focusIndex = index; view.longPress(); }
            }
        }
    }

    // ---- editing: the + slot, what OK does, and Reset (main.c: draw_control_center)

    Item {
        id: plus
        property bool focused: view.focusIndex === tiles.count && !view.sheet && !view.listKind
        visible: view.editing && view.plusRow >= 0 && !view.listKind && !view.closing
        x: view.gridX + Math.max(0, view.plusCol) * (view.unit + view.gap)
        y: view.gridY + Math.max(0, view.plusRow) * (view.unit + view.gap)
        Behavior on x { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        Behavior on y { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        width: view.unit; height: view.unit
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: plus.focused ? view.white : "transparent"
            border.width: plus.focused ? 0 : 2
            border.color: Qt.rgba(1, 1, 1, 0.43)
        }
        Image { id: plusGlyph; anchors.centerIn: parent; width: 34; height: 34; source: "glyphs/plus.png"; smooth: true; mipmap: true; visible: false }
        Tint { anchors.fill: plusGlyph; src: plusGlyph; color: plus.focused ? view.ink : "white" }
        MouseArea { anchors.fill: parent; hoverEnabled: true; enabled: !view.sheet && !view.listKind; onEntered: view.focusIndex = tiles.count; onClicked: { view.focusIndex = tiles.count; view.ok(); } }
    }
    Text {
        visible: view.editing && !view.listKind && !view.closing
        x: view.rightEdge - width
        y: view.gridY + view.rows * (view.unit + view.gap) + 4
        text: view.picked >= 0 ? "Arrows move it  ·  OK puts it down"
            : view.plusRow < 0 ? "No Room  ·  Hold OK on one for options" : "OK to move  ·  Hold for options"
        color: "white"
        opacity: 0.85
        font.family: medium.name
        font.weight: Font.Medium
        font.pixelSize: 22
        style: Text.Raised
        styleColor: "#60000000"
    }
    Item { // Reset: red, like sheets' buttons that can't be undone
        id: reset
        property bool focused: view.focusIndex === tiles.count + 1 && !view.sheet && !view.listKind
        visible: view.editing && !view.listKind && !view.closing
        x: view.gridX
        y: view.gridY + view.rows * (view.unit + view.gap) + 44
        width: view.gridW; height: 64
        Glass {
            anchors.fill: parent
            sourceA: glassA; sourceB: glassB; fade: view.fade
            live: view.hasFrame
            solid: view.solid; dim: view.editDim
            screenX: reset.x; screenY: reset.y
            visible: !reset.focused
        }
        Rectangle { anchors.fill: parent; radius: height / 2; color: view.white; visible: reset.focused }
        Text {
            anchors.centerIn: parent
            text: "Reset"
            color: reset.focused ? "#e03024" : view.red
            font.family: semi.name
            font.weight: Font.DemiBold
            font.pixelSize: 24
        }
        MouseArea { anchors.fill: parent; hoverEnabled: true; enabled: !view.sheet && !view.listKind; onEntered: view.focusIndex = tiles.count + 1; onClicked: { view.focusIndex = tiles.count + 1; view.ok(); } }
    }

    // what went wrong, under the grid
    Text {
        x: view.gridX
        width: view.gridW
        y: view.gridY + Math.max(view.rows, 1) * (view.unit + view.gap) + (view.editing ? 124 : 4)
        horizontalAlignment: Text.AlignRight
        visible: view.problem !== "" && !view.listKind
        text: view.problem
        color: "white"
        opacity: 0.85
        font.family: medium.name
        font.pixelSize: 20
        wrapMode: Text.WordWrap
        style: Text.Raised
        styleColor: "#80000000"
    }

    // ---- a list, in the Control Center's place (main.c: draw_cc_list)

    Item {
        id: list
        property real t: (view.listKind === 1 || view.listKind === 2) && !view.closing ? 1 : 0
        Behavior on t { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        readonly property int rowH: 56
        readonly property int rowGap: 6
        readonly property int head: 66
        readonly property int shown: Math.min(view.listItems.length, 8)
        readonly property int first: Math.max(0, Math.min(view.listIndex - 4, view.listItems.length - shown))
        x: view.gridX
        y: view.gridY - 12 * (1 - t)
        width: view.gridW
        height: head + (shown ? shown * (rowH + rowGap) - rowGap : 40) + 14
        opacity: t
        visible: t > 0.01

        Glass {
            anchors.fill: parent
            radius: 34
            sourceA: glassA; sourceB: glassB; fade: view.fade
            live: view.hasFrame
            solid: view.solid; dim: view.editDim
            screenX: list.x
            screenY: view.gridY
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: (list.head - height) / 2 + 3
            text: view.listKind === 1 ? "Picture" : "Inputs"
            color: "white"
            font.family: semi.name
            font.weight: Font.DemiBold
            font.pixelSize: 24
        }
        Repeater {
            model: list.shown
            delegate: Item {
                property int i: list.first + index
                property var it: view.listItems[i] || ({})
                property bool f: i === view.listIndex
                x: 14
                y: list.head + index * (list.rowH + list.rowGap)
                width: list.width - 28
                height: list.rowH
                Rectangle { anchors.fill: parent; radius: height / 2; color: view.white; visible: parent.f }
                Text {
                    x: 24
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 48 - detail.width
                    text: (parent.it.ticked ? "✓  " : "") + (parent.it.label || "")
                    color: parent.f ? view.ink : "white"
                    font.family: medium.name
                    font.weight: Font.Medium
                    font.pixelSize: 22
                    elide: Text.ElideRight
                }
                Text {
                    id: detail
                    anchors.right: parent.right
                    anchors.rightMargin: 24
                    anchors.verticalCenter: parent.verticalCenter
                    text: parent.it.detail || ""
                    color: parent.f ? view.ink : "white"
                    opacity: 0.7
                    font.family: medium.name
                    font.pixelSize: 19
                }
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: !view.sheet
                    onEntered: view.listIndex = parent.i
                    onClicked: { view.listIndex = parent.i; view.listOk(); }
                }
            }
        }
    }

    // ---- the gallery, in the Control Center's place (main.c: draw_cc_gallery)

    Item {
        id: gal
        property real t: view.listKind === 3 && !view.closing ? 1 : 0
        Behavior on t { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        x: view.rightEdge - view.galW
        y: view.gridY - 12 * (1 - t)
        width: view.galW
        height: view.galleryH
        opacity: t
        visible: t > 0.01

        Glass {
            anchors.fill: parent
            radius: 34
            sourceA: glassA; sourceB: glassB; fade: view.fade
            live: view.hasFrame
            solid: view.solid; dim: view.editDim
            screenX: gal.x
            screenY: view.gridY
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: (view.galHead - height) / 2 + 3
            text: "Add a Control"
            color: "white"
            font.family: semi.name
            font.weight: Font.DemiBold
            font.pixelSize: 24
        }
        Text {
            visible: !view.gallery.length
            anchors.horizontalCenter: parent.horizontalCenter
            y: view.galHead + 14
            text: "Every control this TV has is placed."
            color: "white"
            opacity: 0.7
            font.family: medium.name
            font.pixelSize: 20
        }
        Repeater { // each group's name, small, on the left like iOS's
            model: view.galleryHeads.length
            delegate: Text {
                x: view.galPad + 8
                y: view.galleryHeads[index].y + view.galGroupTop
                text: view.galleryHeads[index].text
                color: "white"
                opacity: 0.6
                font.family: medium.name
                font.weight: Font.Medium
                font.pixelSize: 22
            }
        }
        Repeater {
            model: view.gallery.length
            delegate: Item {
                property var it: view.gallery[index]
                property bool f: index === view.listIndex
                x: it.x; y: it.y
                width: view.galBtn; height: view.galBtn
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width * (parent.f ? 1.08 : 1); height: width; radius: width / 2
                    color: parent.f ? view.white : Qt.rgba(1, 1, 1, 0.14)
                    border.width: parent.f ? 0 : 1.5
                    border.color: Qt.rgba(1, 1, 1, 0.24)
                }
                Image { id: galGlyph; anchors.centerIn: parent; width: 32; height: 32; source: "glyphs/" + (parent.it.entry.icon || "controls") + ".png"; smooth: true; mipmap: true; visible: false }
                Tint { anchors.fill: galGlyph; src: galGlyph; color: parent.f ? view.ink : "white" }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: view.galBtn + 8
                    width: (view.galW - 2 * view.galPad) / 4 - view.galNameGap
                    horizontalAlignment: Text.AlignHCenter
                    text: parent.it.entry.title
                    color: "white"
                    opacity: parent.f ? 1 : 0.75
                    font.family: medium.name
                    font.weight: Font.Medium
                    font.pixelSize: view.galNamePx
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    lineHeight: 0.9
                }
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: !view.sheet
                    onEntered: view.listIndex = index
                    onClicked: { view.listIndex = index; view.galleryOk(); }
                }
            }
        }
    }

    // ---- a sheet, as Home+'s: in the middle, over the rest stepped back

    Rectangle {
        anchors.fill: parent
        color: "black"
        opacity: view.sheet ? 0.35 : 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: 180 } }
        MouseArea { anchors.fill: parent; enabled: !!view.sheet; hoverEnabled: true } // clicks behind a sheet do nothing
    }
    Item {
        id: sheetBox
        property var s: view.sheet || ({ title: "", body: "", items: [] })
        property real t: view.sheet ? 1 : 0
        Behavior on t { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
        width: 640
        height: 36 + sheetTitle.height + (sheetBody.visible ? sheetBody.height + 14 : 0) + 28 + s.items.length * (80 + 16) - 16 + 32
        x: (view.width - width) / 2
        y: (view.height - height) / 2
        opacity: t
        scale: 0.96 + 0.04 * t
        visible: t > 0.01

        Glass {
            anchors.fill: parent
            radius: 40
            sourceA: glassA; sourceB: glassB; fade: view.fade
            live: view.hasFrame
            solid: view.solid; dim: view.editDim
            screenX: sheetBox.x
            screenY: sheetBox.y
        }
        Rectangle { anchors.fill: parent; radius: 40; color: Qt.rgba(16 / 255, 16 / 255, 20 / 255, 0.35) } // sheets are darker
        Text {
            id: sheetTitle
            y: 36
            width: parent.width - 64
            anchors.horizontalCenter: parent.horizontalCenter
            horizontalAlignment: Text.AlignHCenter
            text: sheetBox.s.title
            color: "white"
            font.family: semi.name
            font.weight: Font.DemiBold
            font.pixelSize: 30
            wrapMode: Text.WordWrap
        }
        Text {
            id: sheetBody
            visible: !!sheetBox.s.body
            y: sheetTitle.y + sheetTitle.height + 14
            width: parent.width - 80
            anchors.horizontalCenter: parent.horizontalCenter
            horizontalAlignment: Text.AlignHCenter
            text: sheetBox.s.body || ""
            color: "white"
            opacity: 0.7
            font.family: medium.name
            font.pixelSize: 22
            wrapMode: Text.WordWrap
        }
        Repeater {
            model: sheetBox.s.items.length
            delegate: Item {
                property var it: sheetBox.s.items[index]
                property bool f: index === view.sheetIndex
                x: 32
                y: (sheetBody.visible ? sheetBody.y + sheetBody.height : sheetTitle.y + sheetTitle.height) + 28 + index * (80 + 16)
                width: sheetBox.width - 64
                height: 80
                Rectangle {
                    anchors.fill: parent
                    radius: height / 2
                    color: parent.f ? view.white : Qt.rgba(1, 1, 1, 0.12)
                }
                Text {
                    anchors.centerIn: parent
                    text: parent.it.label
                    color: parent.it.destructive ? (parent.f ? "#e03024" : view.red) : parent.f ? view.ink : "white"
                    font.family: semi.name
                    font.weight: Font.DemiBold
                    font.pixelSize: 26
                }
                Text {
                    visible: !!parent.it.check
                    anchors.right: parent.right
                    anchors.rightMargin: 32
                    anchors.verticalCenter: parent.verticalCenter
                    text: "✓"
                    color: parent.f ? view.ink : "white"
                    font.family: semi.name
                    font.pixelSize: 26
                }
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    onEntered: view.sheetIndex = index
                    onClicked: { view.sheetIndex = index; view.sheetOk(); }
                }
            }
        }
    }
}
