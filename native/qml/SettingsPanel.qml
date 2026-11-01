import QtQuick 2.4
import QtGraphicalEffects 1.0

// Settings over what's playing (opened by ControlCenterView.qml): Home+'s two panes, as it draws them
// (sheet.c: draw_split, draw_row; the same sizes), its pages on the left and the one on show on the right,
// with its keys (settings.c: settings_keydown). The service has the pages and changes a row when asked
// (settings.js): this draws them and says which row. A row that can't be done over an app opens Home+.
Item {
    id: panel
    anchors.fill: parent

    property var view              // ControlCenterView.qml
    property var pages: []         // the service's pages (settings.js: state())
    property int cat: 0            // the page on show
    property bool side: true       // the focus is on the list of pages; else on row `index` of the page
    property int index: 0
    property int first: 0          // the row at the top of the list
    property var changes: []       // what's been pressed and not yet answered: the first is on its way
    property string problem: ""    // a change that didn't work says so in the hint's place
    property alias rowItems: rowRepeater

    // Home+'s two panes (home.h: SPLIT_*, SWATCH_D; settings.c: SWATCH_PITCH; its list's words at 29 px)
    readonly property int splitW: 1600
    readonly property int splitH: 860
    readonly property int splitSide: 460
    readonly property int splitCat: 64
    readonly property int splitCatGap: 6
    readonly property int splitRow: 96
    readonly property int splitGap: 12
    readonly property int splitTitlePx: 40
    readonly property int splitDescPx: 24
    readonly property int splitHintPx: 22
    readonly property int swatchD: 34
    readonly property int swatchPitch: 50
    readonly property int catPx: 29
    readonly property int pad: view.sheetPad
    readonly property int rad: 28 + pad // corners concentric with the rows
    readonly property int step: splitRow + splitGap

    readonly property var page: pages[cat] || ({ id: "", name: "", about: "", body: "", rows: [] })
    readonly property var rows: page.rows
    readonly property var row: rows[index] || ({})
    readonly property string hint: side ? "OK or → opens  ·  Back closes Settings"
        : row.kind === "step" ? "← → change  ·  Back returns to the list"
        : row.kind === "home" ? "OK opens Home+  ·  Back returns to the list" : "OK selects  ·  Back returns to the list"

    // as many rows as fit above the hint (sheet.c: split_rows)
    readonly property real listTop: pageAbout.y + pageAbout.height + 28 + (body.visible ? body.height + 6 + 18 : 0)
    readonly property int fit: Math.floor((box.height - pad - hintText.height - 20 - listTop + splitGap) / step)
    readonly property int shown: rows.length < fit ? rows.length : Math.max(fit, 1)

    property real scroll: first
    Behavior on scroll { enabled: !panel.side && !panel.view.still; NumberAnimation { duration: panel.view.standard; easing.type: Easing.OutCubic } }

    property real t: view.listKind === 4 ? 1 : 0 // (closing, it goes with the panel)
    Behavior on t { NumberAnimation { duration: panel.view.standard; easing.type: Easing.OutCubic } }
    visible: t > 0.01
    opacity: view.fadeIn(view.shown)

    // ---- the service's pages

    // Settings, on the list of pages, or with the focus in page `to`
    function open(to) {
        view.host.call("settings", {}, function (a) {
            if (view.closing) return;
            if (a.returnValue === false) {
                view.problem = "The TV didn’t do that";
                return;
            }
            pages = a.pages;
            view.follow(a.prefs, a.frame);
            problem = "";
            showPage(Math.max(0, pageOf(to)));
            if (pageOf(to) >= 0) enter();
            view.listKind = 4;
        });
    }

    function pageOf(id) {
        for (var i = 0; i < pages.length; i++) if (pages[i].id === id) return i;
        return -1;
    }

    function rowOf(id) {
        for (var i = 0; i < rows.length; i++) if (rows[i].id === id) return i;
        return -1;
    }

    // the pages again: on the same row, if it's still there (a look's choice comes and goes)
    function load(a) {
        var was = page.id, at = row.id;
        pages = a.pages;
        view.follow(a.prefs, a.frame);
        cat = Math.max(0, pageOf(was));
        index = rowOf(at) >= 0 ? rowOf(at) : Math.max(0, Math.min(index, rows.length - 1));
        keep();
    }

    // One change at a time, in order: each is made from what the one before left, and two requests at once
    // needn't reach the service in the order they were sent
    function change(r, dir) {
        problem = "";
        changes.push({ row: r.id, dir: dir });
        if (changes.length === 1) sendChange();
    }

    function sendChange() {
        view.host.call("settings", changes[0], function (a) {
            changes.shift();
            if (a.returnValue === false) problem = "The TV didn’t do that"; // (as it was: nothing has changed)
            else load(a);
            if (changes.length) sendChange();
        });
    }

    // ---- moving (settings.c: settings_keydown, settings_enter; util.c: keep_in_view)

    function keep() {
        var margin = shown > 4 ? 1 : 0, f = first;
        if (f > index - margin) f = index - margin;
        if (f < index + margin - (shown - 1)) f = index + margin - (shown - 1);
        if (f > rows.length - shown) f = rows.length - shown;
        first = Math.max(0, f);
    }

    function showPage(c) {
        if (c < 0 || c >= pages.length) return;
        cat = c;
        side = true;
        index = first = 0;
    }

    // from the list into the page on show (App Tiles: on the look that's on)
    function enter() {
        var on = 0;
        for (var i = 0; i < rows.length; i++) if (rows[i].kind === "check" && rows[i].on) on = i;
        side = false;
        index = on;
        keep();
    }

    function move(dx, dy) {
        problem = "";
        if (side) {
            if (dy && cat + dy >= 0 && cat + dy < pages.length) {
                showPage(cat + dy);
                view.tick();
            } else if (dx > 0) {
                enter();
            }
        } else if (dy) {
            var to = Math.max(0, Math.min(rows.length - 1, index + dy));
            if (to !== index) view.tick();
            index = to;
            keep();
        } else if (row.kind === "step" || row.kind === "tint") {
            view.tick();
            change(row, dx);
        } else if (dx < 0) {
            side = true;
        }
    }

    function ok() {
        if (side) {
            enter();
        } else if (row.kind === "home") { // what can't be done over an app: Home+, on this page
            view.overlay({ event: "open", target: row.page });
            view.close();
        } else if (row.kind === "app") {
            view.overlay({ event: "open", app: row.app });
            view.close();
        } else if (row.kind) {
            change(row, row.kind === "tint" ? "next" : 1); // (Left and Right stop at the last colour; OK goes round)
        }
    }

    function back() {
        if (side) view.listKind = 0;
        else side = true;
    }

    // ---- what's behind steps back, as behind a sheet

    Rectangle {
        anchors.fill: parent
        color: "black"
        opacity: 80 / 255 * panel.t
        MouseArea { anchors.fill: parent; hoverEnabled: true } // clicks behind it do nothing
    }

    Item {
        id: box
        width: panel.splitW
        height: panel.splitH
        x: (panel.width - width) / 2
        y: (panel.height - height) / 2
        opacity: panel.t
        scale: panel.view.still ? 1 : 0.96 + 0.04 * panel.t

        Glass {
            anchors.fill: parent
            radius: panel.rad
            sourceA: panel.view.pictureA; sourceB: panel.view.pictureB; fade: panel.view.fade
            live: panel.view.hasFrame
            solid: panel.view.solid; dim: panel.view.editDim
            screenX: box.x
            screenY: box.y
        }
        Rectangle { anchors.fill: parent; radius: panel.rad; color: Qt.rgba(16 / 255, 16 / 255, 20 / 255, 0.35) } // darker, as sheets are

        // ---- the list of pages, under Settings' name

        Text {
            id: title
            x: panel.pad + 24
            y: panel.pad
            text: "Settings"
            color: "white"
            opacity: panel.view.textStatic / 255
            font.family: panel.view.fontSemi
            font.weight: Font.DemiBold
            font.pixelSize: panel.view.sheetTitlePx
        }
        Repeater {
            model: panel.pages.length
            delegate: Item {
                property var p: panel.pages[index] || ({})
                property bool f: panel.side && index === panel.cat
                x: panel.pad
                y: panel.pad + title.height + 32 + index * (panel.splitCat + panel.splitCatGap)
                width: panel.splitSide - panel.pad - panel.pad / 2
                height: panel.splitCat
                RectangularGlow { // focused, it lifts
                    visible: parent.f
                    x: 0; y: 8; width: parent.width; height: parent.height
                    glowRadius: 18; spread: 0.1; cornerRadius: height / 2 + glowRadius
                    color: "black"
                    opacity: 0.4
                }
                Rectangle { // focused: white; the page on show, while the focus is in it: a tint
                    anchors.fill: parent
                    radius: height / 2
                    color: parent.f ? panel.view.white : "white"
                    opacity: parent.f ? 1 : index === panel.cat ? 34 / 255 : 0
                }
                Image {
                    id: catGlyph
                    x: 24
                    anchors.verticalCenter: parent.verticalCenter
                    width: 34; height: 34
                    source: "glyphs/" + (parent.p.icon || "settings") + ".png"
                    smooth: true; mipmap: true; visible: false
                }
                Tint {
                    anchors.fill: catGlyph
                    src: catGlyph
                    color: parent.f ? panel.view.ink : "white"
                    opacity: parent.f ? 1 : panel.view.textStatic / 255
                }
                Text {
                    x: 24 + 34 + 18
                    anchors.verticalCenter: parent.verticalCenter
                    text: parent.p.name || ""
                    color: parent.f ? panel.view.ink : "white"
                    opacity: parent.f ? 1 : panel.view.textStatic / 255
                    font.family: panel.view.fontMedium
                    font.weight: Font.Medium
                    font.pixelSize: panel.catPx
                }
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: !panel.view.sheet && !panel.view.closing
                    onPositionChanged: if (panel.view.pointerMoved(parent, mouse.x, mouse.y) && (!panel.side || panel.cat !== index)) panel.showPage(index)
                    onClicked: { panel.showPage(index); panel.view.ok(); }
                }
            }
        }
        Rectangle { x: panel.splitSide; y: panel.pad; width: 2; height: parent.height - 2 * panel.pad; color: Qt.rgba(1, 1, 1, 26 / 255) }

        // ---- the page on show: its name, a line on it, its words, its rows, and what the keys do

        Text {
            id: pageName
            x: pane.x
            y: panel.pad
            text: panel.page.name
            color: "white"
            opacity: panel.view.textStatic / 255
            font.family: panel.view.fontSemi
            font.weight: Font.DemiBold
            font.pixelSize: panel.splitTitlePx
        }
        Text {
            id: pageAbout
            x: pane.x
            y: pageName.y + pageName.height + 8
            text: panel.page.about
            color: "white"
            opacity: panel.view.textSecondary / 255
            font.family: panel.view.fontMedium
            font.weight: Font.Medium
            font.pixelSize: panel.view.sheetBodyPx
        }
        Column {
            id: body
            x: pane.x
            y: pageAbout.y + pageAbout.height + 28
            spacing: 6
            visible: !!panel.page.body
            Repeater {
                model: panel.page.body ? panel.page.body.split("\n") : []
                Text {
                    width: pane.width
                    wrapMode: Text.WordWrap
                    text: modelData
                    color: "white"
                    opacity: panel.view.textSecondary / 255
                    font.family: panel.view.fontMedium
                    font.weight: Font.Medium
                    font.pixelSize: panel.view.sheetBodyPx
                }
            }
        }
        Item {
            id: pane
            x: panel.splitSide + panel.pad
            y: panel.listTop
            width: parent.width - panel.pad - x
            height: panel.shown * panel.step - panel.splitGap

            Repeater {
                id: rowRepeater
                model: panel.rows.length
                delegate: Item {
                    id: rowItem
                    property var r: panel.rows[index] || ({})
                    property bool f: !panel.side && index === panel.index
                    property bool stepper: r.kind === "step" && !!r.value
                    property bool chevron: r.kind === "home" || r.kind === "app"
                    property real ry: (index - panel.scroll) * panel.step
                    // rows sliding out of the list fade out; the rows at its edges dim while more lie beyond them
                    property real out: Math.max(-ry, ry + panel.splitRow - pane.height)
                    property real above: Math.max(0, Math.min(1, panel.scroll))
                    property real below: Math.max(0, Math.min(1, panel.rows.length - panel.shown - panel.scroll))
                    property real fromTop: Math.max(0, Math.min(1, ry / panel.step))
                    property real fromBottom: Math.max(0, Math.min(1, (pane.height - ry - panel.splitRow) / panel.step))
                    property real k: Math.max(0, 1 - Math.max(out, 0) / (panel.splitRow * 0.6)) * (1 - 0.55 * above * (1 - fromTop)) * (1 - 0.55 * below * (1 - fromBottom))
                    property color labelColor: r.destructive ? (f ? panel.view.redOnWhite : panel.view.red) : f ? panel.view.ink : "white"
                    property real labelOpacity: f ? 1 : panel.view.textStatic / 255

                    y: ry
                    width: pane.width
                    height: panel.splitRow
                    opacity: k
                    visible: k > 0

                    RectangularGlow { // focused, it lifts
                        visible: rowItem.f
                        x: 0; y: 8; width: parent.width; height: parent.height
                        glowRadius: 18; spread: 0.1; cornerRadius: 28 + glowRadius
                        color: "black"
                        opacity: 0.4
                    }
                    Rectangle { anchors.fill: parent; radius: 28; color: panel.view.white; visible: rowItem.f }
                    Rectangle { // not focused: dark under the glass, so its label reads over anything bright
                        anchors.fill: parent
                        radius: 28
                        color: Qt.rgba(12 / 255, 12 / 255, 16 / 255, 95 / 255)
                        visible: !rowItem.f
                        Rectangle { anchors.fill: parent; radius: 28; color: Qt.rgba(1, 1, 1, 18 / 255) }
                    }
                    Text {
                        id: label
                        x: 36
                        y: (rowItem.height - height - (rowItem.r.desc ? desc.height : 0)) / 2
                        text: rowItem.r.label || ""
                        color: rowItem.labelColor
                        opacity: rowItem.labelOpacity
                        font.family: panel.view.fontMedium
                        font.weight: Font.Medium
                        font.pixelSize: panel.view.sheetButtonPx
                    }
                    Text {
                        id: desc
                        x: 36
                        y: label.y + label.height
                        visible: !!rowItem.r.desc
                        text: rowItem.r.desc || ""
                        color: rowItem.f ? "#58585e" : "white"
                        opacity: rowItem.f ? 1 : panel.view.textSecondary / 255
                        font.family: panel.view.fontMedium
                        font.weight: Font.Medium
                        font.pixelSize: panel.splitDescPx
                    }
                    Text { // the choice that's on: a tick, in its label's colour
                        visible: !!rowItem.r.on && rowItem.r.kind === "check"
                        anchors.right: parent.right
                        anchors.rightMargin: 36
                        anchors.verticalCenter: parent.verticalCenter
                        text: "✓"
                        color: rowItem.labelColor
                        opacity: rowItem.labelOpacity
                        font.family: panel.view.fontMedium
                        font.weight: Font.Medium
                        font.pixelSize: panel.view.sheetButtonPx
                    }
                    Repeater { // the colours, the one that's on ringed in the label's colour
                        model: rowItem.r.kind === "tint" ? rowItem.r.colours.length : 0
                        delegate: Rectangle {
                            x: rowItem.width - 36 - panel.swatchD - (rowItem.r.colours.length - 1 - index) * panel.swatchPitch
                            anchors.verticalCenter: parent.verticalCenter
                            width: panel.swatchD
                            height: width
                            radius: width / 2
                            color: rowItem.r.colours[index]
                            Rectangle {
                                visible: index === rowItem.r.on
                                anchors.centerIn: parent
                                width: parent.width + 12
                                height: width
                                radius: width / 2
                                color: "transparent"
                                border.width: 2.5
                                border.color: rowItem.labelColor
                            }
                        }
                    }
                    Text { // ‹ value ›: Left and Right change it; › alone: OK goes somewhere
                        id: forward
                        visible: rowItem.stepper || rowItem.chevron
                        anchors.right: parent.right
                        anchors.rightMargin: 36
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.verticalCenterOffset: -2
                        text: "›"
                        color: rowItem.f ? "#78787e" : "white"
                        opacity: rowItem.f ? 1 : panel.view.textTertiary / 255
                        font.family: panel.view.fontSemi
                        font.weight: Font.DemiBold
                        font.pixelSize: 34
                    }
                    Text {
                        id: value
                        visible: !!rowItem.r.value
                        anchors.right: forward.visible ? forward.left : parent.right
                        anchors.rightMargin: rowItem.stepper ? 16 : rowItem.chevron ? 14 : 36
                        anchors.verticalCenter: parent.verticalCenter
                        text: rowItem.r.value || ""
                        color: rowItem.f ? "#58585e" : "white"
                        opacity: rowItem.f ? 1 : panel.view.textTertiary / 255
                        font.family: panel.view.fontMedium
                        font.weight: Font.Medium
                        font.pixelSize: panel.view.sheetButtonPx
                    }
                    Text {
                        visible: rowItem.stepper
                        anchors.right: value.left
                        anchors.rightMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.verticalCenterOffset: -2
                        text: "‹"
                        color: forward.color
                        opacity: forward.opacity
                        font: forward.font
                    }
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: rowItem.k > 0.4 && !panel.view.sheet && !panel.view.closing
                        onPositionChanged: {
                            if (!panel.view.pointerMoved(parent, mouse.x, mouse.y)) return;
                            panel.side = false;
                            panel.index = index;
                        }
                        onClicked: { panel.side = false; panel.index = index; panel.view.ok(); }
                    }
                }
            }
        }
        Text {
            id: hintText
            x: pane.x
            y: parent.height - panel.pad - height
            text: panel.problem || panel.hint
            color: "white"
            opacity: (panel.problem ? panel.view.textStatic : panel.view.textTertiary) / 255
            font.family: panel.view.fontMedium
            font.weight: Font.Medium
            font.pixelSize: panel.splitHintPx
        }
    }
}
