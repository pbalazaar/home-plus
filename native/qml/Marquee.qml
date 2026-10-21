import QtQuick 2.4

// A line of text that, when it doesn't fit and it's `running` (its control has the focus), scrolls to
// its end and back, as Home+'s and tvOS's focused names do; while it moves, its edges fade
// (main.c: draw_marquee). Otherwise it ends in an ellipsis.
Item {
    id: m
    property alias text: label.text
    property alias color: label.color
    property alias font: label.font
    property bool running: false
    readonly property bool overflows: label.implicitWidth > width + 1
    height: label.implicitHeight
    clip: true

    Text {
        id: label
        style: Text.Raised // a faint shadow: the glass is clear, and the picture behind can be bright
        styleColor: Qt.rgba(0, 0, 0, color.r > 0.5 ? 0.35 : 0)
        elide: scroll.running ? Text.ElideNone : Text.ElideRight
        width: scroll.running ? implicitWidth : m.width
    }
    SequentialAnimation {
        id: scroll
        running: m.running && m.overflows
        loops: Animation.Infinite
        onRunningChanged: if (!running) label.x = 0
        PauseAnimation { duration: 1200 }
        NumberAnimation {
            target: label; property: "x"; to: m.width - label.implicitWidth
            duration: Math.max(900, (label.implicitWidth - m.width) * 28); easing.type: Easing.InOutSine
        }
        PauseAnimation { duration: 1500 }
        NumberAnimation { target: label; property: "x"; to: 0; duration: 450; easing.type: Easing.InOutQuad }
    }
    layer.enabled: scroll.running
    layer.effect: ShaderEffect {
        property real f: 18 / Math.max(1, m.width)
        fragmentShader: "
            varying highp vec2 qt_TexCoord0;
            uniform sampler2D source;
            uniform lowp float qt_Opacity;
            uniform highp float f;
            void main() {
                highp float x = qt_TexCoord0.x;
                lowp float a = smoothstep(0.0, f, x) * smoothstep(0.0, f, 1.0 - x);
                gl_FragColor = texture2D(source, qt_TexCoord0) * a * qt_Opacity;
            }"
    }
}
