import QtQuick 2.4
import QtGraphicalEffects 1.0

// Recents over what's playing (opened by ControlCenterView.qml): Home+'s App Switcher (recents.c) as a strip
// along the bottom of the screen, with its keys: Left and Right pick an app, OK opens it, Up closes it, Down
// reaches Close All. An app is playing here, so two things differ: the focus starts on the app before the
// one in front, so that OK flips back to it, and Close All asks first, as it closes what's playing too.
Item {
    id: strip
    anchors.fill: parent

    property var view             // ControlCenterView.qml
    property string front: ""     // the app in front
    property int index: 0
    property bool closeAll: false // the focus is on Close All
    property bool loading: false
    property bool failed: false
    property string problem: ""   // what went wrong, in the hint's place
    property int openings: 0      // an answer to an older opening is let be: it would add its cards to this one's
    readonly property int count: cards.count

    readonly property int cardW: 320
    readonly property int cardH: 180
    readonly property int cardR: 28     // home.h: TILE_R, on a card this size
    readonly property int pitch: 360
    readonly property int cardY: 700
    readonly property int iconS: 104

    property real t: view.listKind === 5 ? 1 : 0 // (closing, it goes with the panel)
    Behavior on t { NumberAnimation { duration: strip.view.standard; easing.type: Easing.OutCubic } }
    visible: t > 0.01
    opacity: t * view.fadeIn(view.shown)

    ListModel { id: cards }

    function open() {
        var mine = ++openings;
        cards.clear();
        index = 0;
        closeAll = failed = false;
        problem = "";
        loading = true;
        view.listKind = 5;
        view.host.call("overlay", { event: "recents" }, function (a) {
            if (mine !== openings) return;
            loading = false;
            if (a.returnValue === false || !a.apps) {
                failed = true;
                return;
            }
            front = a.front || "";
            for (var i = 0; i < a.apps.length; i++) {
                var app = a.apps[i];
                cards.append({ appId: app.id, title: app.title || "", tile: app.color || "", icon: app.icon || "", flying: false });
            }
            if (a.apps.length > 1 && a.apps[0].id === front) index = 1;
        });
    }

    function closeOne() {
        var c = cards.get(index);
        if (c.flying) return;
        cards.setProperty(index, "flying", true);
        view.host.call("close", { id: c.appId }, function (r) {
            if (r.returnValue === false) problem = "Couldn’t close that app";
        });
    }

    // Close All, once it's been asked (ControlCenterView.qml: sheetOk)
    function closeEverything() {
        view.host.call("close", { all: true }, function (r) {
            if (r.returnValue === false) problem = "Couldn’t close that app";
        });
        closeAll = false;
        for (var i = 0; i < cards.count; i++) cards.setProperty(i, "flying", true);
    }

    // a card that flew off is gone, and the others close the gap
    function gone(id) {
        for (var i = 0; i < cards.count; i++) if (cards.get(i).appId === id) cards.remove(i);
        if (index >= cards.count) index = Math.max(0, cards.count - 1);
        if (!cards.count) empty.restart();
    }
    Timer { id: empty; interval: 500; onTriggered: if (strip.view.listKind === 5) strip.view.listKind = 0 } // nothing left to show

    function move(dx, dy) {
        problem = "";
        if (!cards.count) return;
        if (closeAll) {
            if (dy < 0) closeAll = false;
        } else if (dx) {
            if (index + dx < 0 || index + dx >= cards.count) return;
            index += dx;
        } else if (dy > 0) {
            closeAll = true;
        } else {
            closeOne();
            return;
        }
        view.tick();
    }

    function ok() {
        if (!cards.count) return;
        if (closeAll) {
            view.openSheet("Close All Apps?", "What’s playing closes too.",
                           [{ label: "Close All", act: "closeAll", destructive: true }, { label: "Cancel", act: "cancel" }], 1);
            return;
        }
        var c = cards.get(index);
        if (c.flying) return;
        if (c.appId !== front) view.overlay({ event: "open", app: c.appId });
        view.close();
    }

    function back() {
        view.listKind = 0;
    }

    // the picture stays in view above; what's here reads over anything bright
    Rectangle {
        y: 520
        width: parent.width
        height: parent.height - y
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0) }
            GradientStop { position: 0.45; color: Qt.rgba(0, 0, 0, 0.5) }
            GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.75) }
        }
    }

    Repeater {
        model: cards
        delegate: Item {
            id: card
            property bool f: index === strip.index && !strip.closeAll
            property real s: f ? 1 : 0
            property real fly: 0 // closed: up and away
            property bool flying: model.flying
            property bool pictured: model.icon !== "" && logo.status !== Image.Error
            property real size: 0.88 + 0.12 * s
            Behavior on s { NumberAnimation { duration: strip.view.standard; easing.type: Easing.OutCubic } }
            Behavior on x { enabled: !strip.view.still; NumberAnimation { duration: strip.view.standard; easing.type: Easing.OutCubic } }
            onFlyingChanged: if (flying) away.start()

            SequentialAnimation {
                id: away
                NumberAnimation { target: card; property: "fly"; to: 1; duration: strip.view.still ? 0 : strip.view.slow; easing.type: Easing.InCubic }
                ScriptAction { script: strip.gone(model.appId) }
            }

            x: strip.width / 2 + (index - strip.index) * strip.pitch - width / 2
            y: strip.cardY + 40 * (1 - strip.t) - fly * 760
            width: strip.cardW
            height: strip.cardH
            opacity: 1 - fly

            Text { // its name, above it
                x: strip.cardW * (1 - card.size) / 2 + 4
                y: strip.cardH * (1 - card.size) / 2 - 12 - height
                width: strip.cardW * card.size - 8
                text: model.title
                color: "white"
                opacity: 0.9 * (0.75 + 0.25 * card.s)
                font.family: strip.view.fontSemi
                font.weight: Font.DemiBold
                font.pixelSize: 24
                elide: Text.ElideRight
                style: Text.Raised
                styleColor: "#80000000"
            }
            Item {
                anchors.fill: parent
                scale: card.size
                RectangularGlow { // focused, it lifts
                    visible: card.s > 0.01
                    x: 0; y: 10; width: parent.width; height: parent.height
                    glowRadius: 20; spread: 0.1; cornerRadius: strip.cardR + glowRadius
                    color: "black"
                    opacity: 0.45 * card.s
                }
                Rectangle { // the app's own colour
                    anchors.fill: parent
                    radius: strip.cardR
                    color: model.tile || "#3a3a40"
                }
                Image {
                    id: logo
                    anchors.centerIn: parent
                    width: strip.iconS; height: strip.iconS
                    sourceSize: Qt.size(strip.iconS * 2, strip.iconS * 2)
                    source: model.icon
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    cache: false
                    smooth: true
                    visible: card.pictured
                }
                Text { // no icon: its name
                    visible: !card.pictured
                    anchors.centerIn: parent
                    width: parent.width - 32
                    horizontalAlignment: Text.AlignHCenter
                    text: model.title
                    color: "white"
                    font.family: strip.view.fontSemi
                    font.weight: Font.DemiBold
                    font.pixelSize: 26
                    elide: Text.ElideRight
                }
                Rectangle { // not focused, it steps back as one: faded part by part, an icon's own square shows on its card
                    anchors.fill: parent
                    radius: strip.cardR
                    color: "black"
                    opacity: 0.25 * (1 - card.s)
                }
                Rectangle { // focused: a ring, over any picture
                    anchors.fill: parent
                    radius: strip.cardR
                    color: "transparent"
                    border.width: 4
                    border.color: strip.view.white
                    opacity: card.s
                }
            }
            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                enabled: !strip.view.sheet && !strip.view.closing
                onPositionChanged: {
                    if (!strip.view.pointerMoved(parent, mouse.x, mouse.y)) return;
                    strip.closeAll = false;
                    strip.index = index;
                }
                onClicked: { strip.closeAll = false; strip.index = index; strip.view.ok(); }
            }
        }
    }

    Text { // none to show, or what the keys do
        anchors.horizontalCenter: parent.horizontalCenter
        y: cards.count ? strip.cardY + strip.cardH + 16 : strip.cardY + (strip.cardH - height) / 2
        visible: !strip.loading
        text: strip.problem || (strip.failed ? "Couldn’t load recent apps" : cards.count ? "OK opens  ·  Up closes" : "No apps running")
        color: "white"
        opacity: strip.view.textSecondary / 255
        font.family: strip.view.fontMedium
        font.weight: Font.Medium
        font.pixelSize: strip.view.hintPx
        style: Text.Raised
        styleColor: "#80000000"
    }

    Item { // Close All, as Home+'s: at the bottom, red
        id: closeButton
        visible: cards.count > 0
        x: (parent.width - width) / 2
        y: parent.height - 60 - height
        width: 360
        height: strip.view.sheetBtn
        Rectangle { anchors.fill: parent; radius: height / 2; color: strip.view.white; visible: strip.closeAll }
        Rectangle { // not focused: dark, so its label reads over anything bright
            anchors.fill: parent
            radius: height / 2
            color: Qt.rgba(12 / 255, 12 / 255, 16 / 255, 95 / 255)
            visible: !strip.closeAll
            Rectangle { anchors.fill: parent; radius: height / 2; color: Qt.rgba(1, 1, 1, 18 / 255) }
        }
        Text {
            anchors.centerIn: parent
            text: "Close All"
            color: strip.closeAll ? strip.view.redOnWhite : strip.view.red
            font.family: strip.view.fontMedium
            font.weight: Font.Medium
            font.pixelSize: strip.view.sheetButtonPx
        }
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            enabled: !strip.view.sheet && !strip.view.closing
            onPositionChanged: if (strip.view.pointerMoved(parent, mouse.x, mouse.y)) strip.closeAll = true
            onClicked: { strip.closeAll = true; strip.view.ok(); }
        }
    }
}
