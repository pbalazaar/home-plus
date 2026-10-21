import QtQuick 2.4

// Home+'s glass over other apps: Home+'s own Liquid Glass (render.c: FS_GLASS, as its Control Center
// draws it) over the picture behind, blurred (ControlCenterView.qml's frames of the video, the newer
// fading in over the older). No tint: the blurred picture, its bright parts dimmed (up to 35%) so the
// words on it stay readable; the rounded bevel bends it (the edge magnifies); a thin rim that flares
// where it faces the light (the top left), with a faint glint opposite and the far side of the bevel
// in shade. With no picture (none yet, or video the TV can't capture) it's frosted, a dense tint
// with the rim, the picture fading in over it when it comes. Dark (Settings) is a clear dark tint,
// with no picture; Reduce Transparency, a solid panel, as Home+'s.
Item {
    id: glass
    property variant sourceA      // the blurred pictures: textures standing for the whole 1920 x 1080 screen
    property variant sourceB
    property real fade: 0          // 0: A, 1: B
    property bool live: false      // there's a picture
    property bool dark: false      // Settings: the Control Center's appearance, Dark
    property bool solid: false     // Reduce Transparency
    property real radius: height / 2
    property real screenX: 0       // where it is on the screen, before any movement
    property real screenY: 0
    property real pic: live && !solid && !dark ? 1 : 0 // the picture, over the tint
    Behavior on pic { NumberAnimation { duration: 240; easing.type: Easing.InOutQuad } }

    ShaderEffect {
        anchors.fill: parent
        visible: glass.pic > 0
        opacity: glass.pic
        property variant srcA: glass.sourceA
        property variant srcB: glass.sourceB
        property real fade: glass.fade
        property rect area: Qt.rect(glass.screenX / 1920, glass.screenY / 1080, glass.width / 1920, glass.height / 1080)
        property real radius: Math.min(glass.radius, Math.min(width, height) / 2)
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
            highp float sdRound(highp vec2 p, highp vec2 h, highp float r) {
                highp vec2 q = abs(p) - (h - vec2(r));
                return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
            }
            void main() {
                highp vec2 h = box * 0.5;
                highp vec2 p = qt_TexCoord0 * box - h;
                highp float d = sdRound(p, h, radius);
                lowp float mask = clamp(0.5 - d, 0.0, 1.0);
                highp vec2 n = vec2(sdRound(p + vec2(1.0, 0.0), h, radius) - sdRound(p - vec2(1.0, 0.0), h, radius),
                                    sdRound(p + vec2(0.0, 1.0), h, radius) - sdRound(p - vec2(0.0, 1.0), h, radius));
                n /= max(length(n), 0.0001);
                highp float depth = -d;
                highp float band = clamp(min(h.x, h.y) * 0.9, 6.0, 24.0);
                highp float t = clamp(1.0 - depth / band, 0.0, 1.0);
                highp float bevel = t * t;
                highp vec2 sp = p - n * (bevel * band * 0.9);
                highp vec2 uv = area.xy + (sp / box + 0.5) * area.zw;
                lowp vec3 col = mix(texture2D(srcA, uv).rgb, texture2D(srcB, uv).rgb, fade);
                col *= 1.0 - 0.10 * bevel;
                lowp float lum = dot(col, vec3(0.299, 0.587, 0.114));
                col *= 1.0 - 0.35 * smoothstep(0.35, 0.75, lum);
                lowp float facing = dot(n, vec2(-0.6, -0.8));
                lowp float rim = 1.0 - smoothstep(0.0, 2.5, depth);
                lowp float lit = max(facing, 0.0), back = max(-facing, 0.0);
                lit = lit * lit * lit; lit *= lit; back = back * back * back; back *= back;
                col += rim * (0.10 + 0.40 * lit + 0.15 * back) * 0.85;
                col *= 1.0 - 0.16 * bevel * max(-facing, 0.0);
                gl_FragColor = vec4(min(col, vec3(1.0)), 1.0) * mask * qt_Opacity;
            }"
    }

    // no picture to bend: frosted (or, Dark, a clear tint; with Reduce Transparency, a solid panel), and the same rim
    Rectangle {
        anchors.fill: parent
        visible: glass.pic < 1
        radius: glass.radius
        color: glass.solid ? Qt.rgba(38 / 255, 38 / 255, 44 / 255, 0.96)
             : glass.dark ? Qt.rgba(0.11, 0.11, 0.13, 0.3) : Qt.rgba(0.22, 0.22, 0.25, 0.72)
    }
    ShaderEffect {
        anchors.fill: parent
        visible: glass.pic < 1
        property real radius: Math.min(glass.radius, Math.min(width, height) / 2)
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
                lowp float rim = clamp(0.5 - d, 0.0, 1.0) * (1.0 - smoothstep(0.0, 2.5, -d));
                lowp float facing = dot(normalize(p + vec2(0.001)), vec2(-0.6, -0.8));
                lowp float lit = max(facing, 0.0), back = max(-facing, 0.0);
                lit = lit * lit * lit; lit *= lit; back = back * back * back; back *= back;
                gl_FragColor = vec4(1.0) * rim * (0.10 + 0.40 * lit + 0.15 * back) * 0.85 * qt_Opacity;
            }"
    }
}
