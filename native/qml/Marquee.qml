import QtQuick 2.4

// A line of text in a room it may not fit, as Home+'s (main.c: draw_marquee) and tvOS's: what doesn't
// fit fades out at the room's edge. While it's `running` (its control has the focus) it waits 2 s, then
// slides left to show the rest, a gap, then the start again, and waits again; the left edge fades too
// while it's moved.
Item {
    id: m
    property alias text: label.text
    property alias color: label.color
    property alias font: label.font
    property bool running: false
    readonly property bool overflows: label.implicitWidth > width + 1
    readonly property real period: label.implicitWidth + 40 // the text and a gap, then the text again
    property real off: 0
    height: label.implicitHeight
    clip: true

    Text { id: label; x: -m.off }
    Text { x: m.period - m.off; text: label.text; color: label.color; font: label.font; visible: m.off > 0 }
    SequentialAnimation {
        running: m.running && m.overflows
        loops: Animation.Infinite
        onRunningChanged: if (!running) m.off = 0
        PauseAnimation { duration: 2000 }
        NumberAnimation { target: m; property: "off"; from: 0; to: m.period; duration: m.period / 36 * 1000 } // 36 px a second
        ScriptAction { script: m.off = 0 } // the start again, as it was
    }
    layer.enabled: m.overflows
    layer.effect: ShaderEffect {
        property real f: 16 / Math.max(1, m.width)
        property real moved: m.off > 0.5 ? 1 : 0 // the left edge fades too
        fragmentShader: "
            varying highp vec2 qt_TexCoord0;
            uniform sampler2D source;
            uniform lowp float qt_Opacity;
            uniform highp float f;
            uniform lowp float moved;
            void main() {
                highp float x = qt_TexCoord0.x;
                lowp float a = clamp((1.0 - x) / f, 0.0, 1.0) * mix(1.0, clamp(x / f, 0.0, 1.0), moved);
                gl_FragColor = texture2D(source, qt_TexCoord0) * a * qt_Opacity;
            }"
    }
}
