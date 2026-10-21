import QtQuick 2.4

// A white glyph (native/qml/glyphs/) in another colour, in one pass: QtGraphicalEffects' ColorOverlay
// renders its source to a texture first, which a glyph that's already one doesn't need.
ShaderEffect {
    property variant src        // the glyph: an Image
    property color color: "white"
    fragmentShader: "
        varying highp vec2 qt_TexCoord0;
        uniform sampler2D src;
        uniform lowp vec4 color;
        uniform lowp float qt_Opacity;
        void main() {
            gl_FragColor = color * texture2D(src, qt_TexCoord0).a * qt_Opacity;
        }"
}
