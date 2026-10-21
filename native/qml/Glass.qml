import QtQuick 2.4

// Home+'s glass over other apps: the picture behind it, blurred (ControlCenterView.qml's frames of
// the video, the newer fading in over the older) and a little magnified, inside a rounded shape;
// over it a tint and light on the upper half, and a rim that's brightest where the light comes
// from (the top left, as Home+'s). With no picture (protected video gives none) the tint is darker.
Item {
    id: glass
    property variant sourceA      // the blurred pictures: textures standing for the whole 1920 x 1080 screen
    property variant sourceB
    property real fade: 0          // 0: A, 1: B
    property bool live: false      // there's a picture
    property bool solid: false     // Reduce Transparency: a solid panel, as Home+'s
    property real radius: height / 2
    property real screenX: 0       // where it is on the screen, before any movement
    property real screenY: 0

    ShaderEffect {
        anchors.fill: parent
        visible: glass.live && !glass.solid
        property variant srcA: glass.sourceA
        property variant srcB: glass.sourceB
        property real fade: glass.fade
        property rect area: Qt.rect(glass.screenX / 1920, glass.screenY / 1080, glass.width / 1920, glass.height / 1080)
        property real radius: glass.radius
        property size box: Qt.size(width, height)
        fragmentShader: "
            varying highp vec2 qt_TexCoord0;
            uniform sampler2D srcA;
            uniform sampler2D srcB;
            uniform lowp float fade;
            uniform highp vec4 area;
            uniform highp float radius;
            uniform highp vec2 box;
            uniform lowp float qt_Opacity;
            void main() {
                highp vec2 q = abs(qt_TexCoord0 * box - box * 0.5) - (box * 0.5 - vec2(radius));
                highp float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius;
                lowp float inside = clamp(0.5 - d, 0.0, 1.0);
                highp vec2 uv = area.xy + (qt_TexCoord0 * 0.9 + 0.05) * area.zw;
                lowp vec3 c = mix(texture2D(srcA, uv).rgb, texture2D(srcB, uv).rgb, fade);
                gl_FragColor = vec4(mix(c, vec3(1.0), 0.04), 1.0) * inside * 0.85 * qt_Opacity; // a little of the live picture shows through
            }"
    }
    Rectangle {
        anchors.fill: parent
        radius: glass.radius
        color: glass.solid ? Qt.rgba(38 / 255, 38 / 255, 44 / 255, 0.96) : Qt.rgba(0.11, 0.11, 0.13, glass.live ? 0.08 : 0.22)
        Behavior on color { ColorAnimation { duration: 300 } }
    }
    Rectangle {
        anchors.fill: parent
        radius: glass.radius
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, 0.10) }
            GradientStop { position: 0.55; color: Qt.rgba(1, 1, 1, 0.0) }
        }
    }
    ShaderEffect { // the rim
        anchors.fill: parent
        property real radius: glass.radius
        property size box: Qt.size(width, height)
        fragmentShader: "
            varying highp vec2 qt_TexCoord0;
            uniform highp float radius;
            uniform highp vec2 box;
            uniform lowp float qt_Opacity;
            void main() {
                highp vec2 p = qt_TexCoord0 * box - box * 0.5;
                highp vec2 q = abs(p) - (box * 0.5 - vec2(radius));
                highp float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius;
                lowp float ring = clamp(0.5 - d, 0.0, 1.0) * clamp(d + 2.0, 0.0, 1.0);
                lowp float light = 0.16 + 0.5 * clamp(dot(normalize(p + vec2(0.001)), vec2(-0.6, -0.8)), 0.0, 1.0);
                gl_FragColor = vec4(1.0) * ring * light * qt_Opacity;
            }"
    }
}
