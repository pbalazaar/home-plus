/* Home+ native: GLES2 renderer. See render.h. */
#include "render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The vendored Khronos headers give us types and enums; the functions are
 * looked up at run time, so the binary doesn't link against GL at all. */
#include "SDL_opengles2_khrplatform.h"
#include "SDL_opengles2_gl2platform.h"
#include "SDL_opengles2_gl2.h"
#include "SDL_opengles2_gl2ext.h"

#define GL_FUNCS(X)                                                                                         \
    X(ActiveTexture) X(AttachShader) X(BindAttribLocation) X(BindBuffer) X(BindFramebuffer) X(BindTexture)   \
    X(BlendFuncSeparate) X(BufferData) X(CheckFramebufferStatus) X(Clear) X(ClearColor) X(CompileShader)     \
    X(CreateProgram) X(CreateShader) X(DeleteFramebuffers) X(DeleteShader) X(DeleteTextures) X(Disable)       \
    X(DrawArrays) X(Enable)                                                                                 \
    X(Finish)                                                                                               \
    X(EnableVertexAttribArray) X(FramebufferTexture2D) X(GenBuffers) X(GenFramebuffers) X(GenTextures)       \
    X(GetIntegerv) X(GetProgramInfoLog) X(GetProgramiv) X(GetShaderInfoLog) X(GetShaderiv) X(GetString)     \
    X(GetUniformLocation)                                                                                   \
    X(LinkProgram) X(PixelStorei) X(ReadPixels) X(ShaderSource) X(TexImage2D) X(TexParameteri) X(Uniform1f)  \
    X(Uniform1i) X(Uniform2f) X(Uniform4f) X(UseProgram) X(VertexAttribPointer) X(Viewport)

static struct {
#define X(n) __typeof__(gl##n) *n;
    GL_FUNCS(X)
#undef X
} gl;

/* ------------------------------------------------------------------ shaders */

static const char *VS =
    "attribute vec2 a_pos;\n"
    "uniform vec4 u_rect;\n"  /* x, y, w, h in logical px */
    "uniform vec2 u_view;\n"  /* logical size */
    "uniform vec4 u_uv;\n"    /* u0, v0, u1, v1 */
    "uniform vec2 u_offset;\n" /* OLED pixel orbit: moves the picture, not what glass samples */
    "varying vec2 v_uv;\n"
    "varying vec2 v_px;\n"
    "void main() {\n"
    "    vec2 p = u_rect.xy + a_pos * u_rect.zw;\n"
    "    v_px = p;\n"
    "    v_uv = mix(u_uv.xy, u_uv.zw, a_pos);\n"
    "    vec2 q = p + u_offset;\n"
    "    gl_Position = vec4(q.x / u_view.x * 2.0 - 1.0, 1.0 - q.y / u_view.y * 2.0, 0.0, 1.0);\n"
    "}\n";

/* shared by the fragment shaders: precision, varyings, rounded-box distance */
static const char *FS_HEAD =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "varying vec2 v_uv;\n"
    "varying vec2 v_px;\n"
    "float sd_round(vec2 p, vec2 b, float r) {\n"
    "    vec2 q = abs(p) - b + r;\n"
    "    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n";

static const char *FS_TEX =
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_color;\n"
    "uniform vec4 u_rect;\n"
    "uniform float u_radius;\n"
    "void main() {\n"
    "    vec4 c = texture2D(u_tex, v_uv) * u_color;\n"
    "    if (u_radius > 0.0) {\n"
    "        vec2 h = u_rect.zw * 0.5;\n"
    "        c.a *= clamp(0.5 - sd_round(v_px - u_rect.xy - h, h, u_radius), 0.0, 1.0);\n"
    "    }\n"
    "    gl_FragColor = c;\n"
    "}\n";

/* An app icon's logo in a look (r_logo): in one colour, its strength from how far the ink stood
 * out from the icon's background; or for a dark tile, grey and black ink turned light. */
static const char *FS_LOGO =
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_color;\n" /* one colour: it; dark tile: the colour grey ink takes, a = how much */
    "uniform vec4 u_rect;\n"
    "uniform float u_radius;\n"
    "uniform vec4 u_mono;\n" /* 1 = one colour, the background's luminance, 1 / span, faint */
    "uniform float u_alpha;\n"
    "void main() {\n"
    "    vec4 t = texture2D(u_tex, v_uv);\n"
    "    float l = dot(t.rgb, vec3(0.299, 0.587, 0.114));\n"
    "    vec4 c;\n"
    "    if (u_mono.x > 0.5) {\n"
    "        float k = clamp((u_mono.y < 0.0 ? l : abs(l - u_mono.y)) * u_mono.z, 0.0, 1.0);\n"
    "        c = vec4(u_color.rgb, t.a * mix(u_mono.w, 1.0, k));\n"
    "    } else {\n"
    "        float s = max(t.r, max(t.g, t.b)) - min(t.r, min(t.g, t.b));\n"
    "        float v = u_mono.y > 0.5 ? 1.0 - l : l;\n" /* a light icon's dark ink turns light */
    "        vec3 ink = mix(vec3(v), u_color.rgb * (0.6 + 0.4 * v), u_color.a);\n"
    "        c = vec4(mix(t.rgb, ink, 1.0 - smoothstep(0.08, 0.25, s)), t.a);\n"
    "    }\n"
    "    c.a *= u_alpha;\n"
    "    if (u_radius > 0.0) {\n"
    "        vec2 h = u_rect.zw * 0.5;\n"
    "        c.a *= clamp(0.5 - sd_round(v_px - u_rect.xy - h, h, u_radius), 0.0, 1.0);\n"
    "    }\n"
    "    gl_FragColor = c;\n"
    "}\n";

static const char *FS_SHAPE =
    "uniform vec4 u_color;\n"
    "uniform vec4 u_rect;\n"
    "uniform float u_radius;\n"
    "uniform float u_stroke;\n"
    "uniform vec2 u_grad;\n" /* ring alpha at the top, at the bottom */
    "void main() {\n"
    "    vec2 h = u_rect.zw * 0.5;\n"
    "    float d = sd_round(v_px - u_rect.xy - h, h, u_radius);\n"
    "    float a = clamp(0.5 - d, 0.0, 1.0);\n"
    "    if (u_stroke > 0.0) {\n"
    "        a -= clamp(0.5 - (d + u_stroke), 0.0, 1.0);\n"
    "        a *= mix(u_grad.x, u_grad.y, clamp((v_px.y - u_rect.y) / u_rect.w, 0.0, 1.0));\n"
    "    }\n"
    "    gl_FragColor = vec4(u_color.rgb, u_color.a * a);\n"
    "}\n";

static const char *FS_SHADOW =
    "uniform vec4 u_color;\n"
    "uniform vec4 u_shape;\n" /* the rect casting the shadow */
    "uniform float u_radius;\n"
    "uniform float u_blur;\n"
    "void main() {\n"
    "    vec2 h = u_shape.zw * 0.5;\n"
    "    float d = sd_round(v_px - u_shape.xy - h, h, u_radius);\n"
    "    float a = 1.0 - smoothstep(-u_blur * 0.5, u_blur, d);\n"
    "    gl_FragColor = vec4(u_color.rgb, u_color.a * a * a);\n"
    "}\n";

/* A rim that catches the light: brightest where it faces u_light (the direction
 * the light comes from), a fainter reflection opposite. */
static const char *FS_RIM =
    "uniform vec4 u_color;\n"
    "uniform vec4 u_rect;\n"
    "uniform float u_radius;\n"
    "uniform float u_stroke;\n"
    "uniform vec2 u_light;\n"
    "void main() {\n"
    "    vec2 h = u_rect.zw * 0.5;\n"
    "    vec2 p = v_px - u_rect.xy - h;\n"
    "    float d = sd_round(p, h, u_radius);\n"
    "    float a = clamp(0.5 - d, 0.0, 1.0) - clamp(0.5 - (d + u_stroke), 0.0, 1.0);\n"
    "    if (a <= 0.0) discard;\n"
    "    vec2 n = vec2(sd_round(p + vec2(1.0, 0.0), h, u_radius) - sd_round(p - vec2(1.0, 0.0), h, u_radius),\n"
    "                  sd_round(p + vec2(0.0, 1.0), h, u_radius) - sd_round(p - vec2(0.0, 1.0), h, u_radius));\n"
    "    float facing = dot(n / max(length(n), 0.0001), u_light);\n"
    "    float k = min(0.25 + 0.95 * max(facing, 0.0) + 0.35 * max(-facing, 0.0), 1.0);\n"
    "    gl_FragColor = vec4(u_color.rgb, u_color.a * a * k);\n"
    "}\n";

/* Dual-filter (Kawase) blur: cheap, wide, and stable when things move. */
static const char *FS_DOWN =
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_halfpix;\n"
    "void main() {\n"
    "    vec4 s = texture2D(u_tex, v_uv) * 4.0;\n"
    "    s += texture2D(u_tex, v_uv - u_halfpix);\n"
    "    s += texture2D(u_tex, v_uv + u_halfpix);\n"
    "    s += texture2D(u_tex, v_uv + vec2(u_halfpix.x, -u_halfpix.y));\n"
    "    s += texture2D(u_tex, v_uv - vec2(u_halfpix.x, -u_halfpix.y));\n"
    "    gl_FragColor = s / 8.0;\n"
    "}\n";

static const char *FS_UP =
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_halfpix;\n"
    "void main() {\n"
    "    vec2 o = u_halfpix;\n"
    "    vec4 s = texture2D(u_tex, v_uv + vec2(-o.x * 2.0, 0.0));\n"
    "    s += texture2D(u_tex, v_uv + vec2(-o.x, o.y)) * 2.0;\n"
    "    s += texture2D(u_tex, v_uv + vec2(0.0, o.y * 2.0));\n"
    "    s += texture2D(u_tex, v_uv + vec2(o.x, o.y)) * 2.0;\n"
    "    s += texture2D(u_tex, v_uv + vec2(o.x * 2.0, 0.0));\n"
    "    s += texture2D(u_tex, v_uv + vec2(o.x, -o.y)) * 2.0;\n"
    "    s += texture2D(u_tex, v_uv + vec2(0.0, -o.y * 2.0));\n"
    "    s += texture2D(u_tex, v_uv + vec2(-o.x, -o.y)) * 2.0;\n"
    "    gl_FragColor = s / 12.0;\n"
    "}\n";

/* Liquid Glass: bends the backdrop most at the rim (lensing). Clear glass stays
 * clear; glass that carries text or icons (u_legible) blurs evenly, since
 * blurring only the busy spots broke photos into patches, and over bright
 * content it dims (Apple's 35%), just enough to keep that content readable.
 * A thin rim catches the light. Past the bevel (most of a big panel) none of
 * the rim's work changes a pixel, so it's skipped there: the same picture for
 * a quarter of the work, where the dock and a sheet filled the C3's GPU. */
static const char *FS_GLASS =
    "uniform sampler2D u_sharp;\n"
    "uniform sampler2D u_soft;\n"
    "uniform vec4 u_rect;\n"
    "uniform vec2 u_view;\n"
    "uniform float u_radius;\n"
    "uniform float u_lens;\n"
    "uniform float u_legible;\n"
    "uniform vec2 u_light;\n" /* direction the light comes from */
    "uniform float u_focus;\n"
    "uniform vec4 u_tint;\n"
    "uniform float u_alpha;\n"
    "uniform vec2 u_behind;\n" /* what it sits on, as shown: how blurred, and how bright next to the backdrop */
    "vec2 to_uv(vec2 p) { return vec2(p.x / u_view.x, 1.0 - p.y / u_view.y); }\n"
    "void main() {\n"
    "    vec2 h = u_rect.zw * 0.5;\n"
    "    vec2 p = v_px - u_rect.xy - h;\n"
    "    float d = sd_round(p, h, u_radius);\n"
    "    float mask = clamp(0.5 - d, 0.0, 1.0);\n"
    "    if (mask <= 0.0) discard;\n"
    "    float depth = -d;\n"
    "    float band = clamp(min(h.x, h.y) * 0.9, 6.0, 24.0);\n" /* width of the rounded bevel: narrow, like tvOS's */
    "    vec3 col;\n"
    "    float bevel = 0.0, facing = 0.0, rim = 0.0;\n"
    "    if (depth < band) {\n"
    "        vec2 n = vec2(sd_round(p + vec2(1.0, 0.0), h, u_radius) - sd_round(p - vec2(1.0, 0.0), h, u_radius),\n"
    "                      sd_round(p + vec2(0.0, 1.0), h, u_radius) - sd_round(p - vec2(0.0, 1.0), h, u_radius));\n"
    "        n /= max(length(n), 0.0001);\n" /* outward normal */
    "        float t = clamp(1.0 - depth / band, 0.0, 1.0);\n" /* 1 at the rim, 0 past the bevel */
    "        bevel = t * t;\n" /* a gentle profile: the picture bends without smearing into streaks */
    "        vec2 sp = v_px - n * (bevel * band * 0.9 * u_lens);\n" /* sample further in: the edge magnifies */
    "        vec2 uv = to_uv(sp);\n"
    "        col = texture2D(u_soft, uv).rgb;\n"
    "        float clear = (1.0 - max(u_legible, bevel * 0.35)) * (1.0 - u_behind.x);\n"
    "        if (clear > 0.0) {\n" /* clear glass: the sharp backdrop, its colours split in the bevel (faintly: Apple's are) */
    "            vec2 duv = vec2(n.x / u_view.x, -n.y / u_view.y) * (bevel * 4.0 * u_lens);\n"
    "            vec3 sharp = vec3(texture2D(u_sharp, uv + duv).r, texture2D(u_sharp, uv).g, texture2D(u_sharp, uv - duv).b);\n"
    "            col = mix(col, sharp, clear);\n"
    "        }\n"
    "        facing = dot(n, u_light);\n"
    "        rim = 1.0 - smoothstep(0.0, 2.5, depth);\n"
    "    } else {\n" /* past the bevel (and the rim, at most 2.5 px in): the backdrop straight through */
    "        vec2 uv = to_uv(v_px);\n"
    "        col = texture2D(u_soft, uv).rgb;\n"
    "        float clear = (1.0 - u_legible) * (1.0 - u_behind.x);\n"
    "        if (clear > 0.0) col = mix(col, texture2D(u_sharp, uv).rgb, clear);\n"
    "    }\n"
    "    col *= u_behind.y;\n"
    "    col *= 1.0 - 0.10 * bevel * u_legible;\n" /* saturated tiles pulled into the rim stay quiet */
    "    float lum = dot(col, vec3(0.299, 0.587, 0.114));\n"
    "    col *= 1.0 - 0.35 * smoothstep(0.35, 0.75, lum) * (0.5 + 0.5 * u_legible);\n"
    "    col = mix(col, u_tint.rgb, u_tint.a);\n"
    /* a faint rim, even all round; it only flares where it faces the light (a corner,
     * with light from a corner), with a weaker glint on the far side. No band of
     * light along the top: tvOS's dock has none. */
    "    float lit = max(facing, 0.0), back = max(-facing, 0.0);\n"
    "    lit *= lit * lit; lit *= lit; back *= back * back; back *= back;\n"
    "    float spec = rim * (0.10 + 0.40 * lit + 0.15 * back);\n"
    "    col += spec * (0.85 + 0.45 * u_focus);\n"
    "    if (u_focus > 0.0) {\n" /* lit from within when focused: a soft glow toward the light, with no edge */
    "        vec2 g = p / h - u_light * 0.45;\n"
    "        col += u_focus * 0.12 * exp(-1.5 * dot(g, g));\n"
    "    }\n"
    "    col *= 1.0 - 0.16 * bevel * max(-facing, 0.0);\n" /* the bevel away from the light falls into shade */
    "    gl_FragColor = vec4(min(col, vec3(1.0)), mask * u_alpha);\n"
    "}\n";

/* The light Liquid Glass catches, for a tile that isn't glass (its icon's own colours, or dark), drawn over it
 * while it has focus: FS_GLASS's own light, the rim's glint where it faces the light and a fainter one opposite,
 * the glow toward the light and the bevel away from it in shade. Blended ONE, SRC_ALPHA: what's under it is
 * shaded by the alpha and the light added, as the glass does to what it shows. */
static const char *FS_SHEEN =
    "uniform vec4 u_rect;\n"
    "uniform float u_radius;\n"
    "uniform vec2 u_light;\n"
    "uniform float u_focus;\n"
    "void main() {\n"
    "    vec2 h = u_rect.zw * 0.5;\n"
    "    vec2 p = v_px - u_rect.xy - h;\n"
    "    float d = sd_round(p, h, u_radius);\n"
    "    float mask = clamp(0.5 - d, 0.0, 1.0);\n"
    "    if (mask <= 0.0) discard;\n"
    "    float depth = -d, band = clamp(min(h.x, h.y) * 0.9, 6.0, 24.0);\n"
    "    float bevel = 0.0, facing = 0.0, rim = 0.0;\n"
    "    if (depth < band) {\n"
    "        vec2 n = vec2(sd_round(p + vec2(1.0, 0.0), h, u_radius) - sd_round(p - vec2(1.0, 0.0), h, u_radius),\n"
    "                      sd_round(p + vec2(0.0, 1.0), h, u_radius) - sd_round(p - vec2(0.0, 1.0), h, u_radius));\n"
    "        float t = clamp(1.0 - depth / band, 0.0, 1.0);\n"
    "        bevel = t * t;\n"
    "        facing = dot(n / max(length(n), 0.0001), u_light);\n"
    "        rim = 1.0 - smoothstep(0.0, 2.5, depth);\n"
    "    }\n"
    "    float lit = max(facing, 0.0), away = max(-facing, 0.0), back = away;\n"
    "    lit *= lit * lit; lit *= lit; back *= back * back; back *= back;\n"
    "    vec2 g = p / h - u_light * 0.45;\n"
    "    float light = rim * (0.10 + 0.40 * lit + 0.15 * back) * (0.85 + 0.45 * u_focus) + u_focus * 0.12 * exp(-1.5 * dot(g, g));\n"
    "    float shade = 1.0 - 0.16 * bevel * away, k = clamp(u_focus, 0.0, 1.0) * mask;\n"
    "    gl_FragColor = vec4(vec3(light * shade * k), 1.0 - (1.0 - shade) * k);\n"
    "}\n";

/* ------------------------------------------------------------------ state */

typedef struct {
    GLuint id;
    GLint rect, view, uv, offset, color, radius, tex, stroke, grad, shape, blur, halfpix;
    GLint sharp, soft, lens, legible, light, focus, tint, alpha, mono, behind;
} Prog;

typedef struct {
    GLuint fbo, tex;
    int w, h;
} Target;

enum { BD_W = 960, BD_H = 540 };

static struct {
    SDL_Window *win;
    SDL_GLContext ctx;
    int lw, lh;         /* logical size */
    int dw, dh;         /* drawable size */
    float orbit_x, orbit_y; /* OLED pixel orbit, screen only */
    float tx, ty;           /* r_translate(), on every target */
    float ox, oy;       /* offset for the current target */
    Prog tex, logo, shape, shadow, rim, down, up, glass, sheen;
    const Prog *cur;
    GLuint quad;
    Tex white;          /* 1x1, for untextured image draws */
    Target backdrop[2], soft[2]; /* what glass refracts, and its blurred copy: the Top Shelf's picture; the scene */
    Target d1, d2, d3, u2;       /* the blur's steps */
    int targets;                 /* they're made (r_targets_release lets them go) */
    size_t tex_bytes;            /* what the textures hold (r_tex), and how many there are */
    int textures;
    int drawing, used;           /* the backdrop being drawn; the one glass refracts */
    Uint64 print[2];             /* each backdrop's fingerprint: what was drawn into it */
    int held[2];                 /* (it holds what its fingerprint says) */
    unsigned drawn[2], serial;   /* how often each was drawn; the last texture's serial */
} R;

/* Noting what would be drawn instead of drawing it (r_backdrop_note): each draw's kind, texture,
 * numbers and where it's moved go into a fingerprint of the picture (FNV-1a). */
static struct {
    int on;
    Uint64 h;
} H;

static void note(const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) H.h = (H.h ^ b[i]) * 1099511628211ULL;
}

static int noting(unsigned kind, const Tex *t, const float *v, int n)
{
    if (!H.on) return 0;
    unsigned id[2] = { kind, t ? t->serial : 0 };
    float at[2] = { R.tx, R.ty };
    note(id, sizeof id);
    note(at, sizeof at);
    note(v, (size_t)n * sizeof *v);
    return 1;
}

static GLuint compile(GLenum kind, const char *src1, const char *src2)
{
    GLuint s = gl.CreateShader(kind);
    const char *srcs[2] = { src1, src2 ? src2 : "" };
    gl.ShaderSource(s, 2, srcs, NULL);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl.GetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "homeplus: shader: %s\n", log);
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

static int program(Prog *p, const char *fs)
{
    memset(p, 0, sizeof *p);
    GLuint v = compile(GL_VERTEX_SHADER, VS, NULL), f = compile(GL_FRAGMENT_SHADER, FS_HEAD, fs);
    if (!v || !f) return 0;
    p->id = gl.CreateProgram();
    gl.AttachShader(p->id, v);
    gl.AttachShader(p->id, f);
    gl.BindAttribLocation(p->id, 0, "a_pos");
    gl.LinkProgram(p->id);
    gl.DeleteShader(v);
    gl.DeleteShader(f);
    GLint ok = 0;
    gl.GetProgramiv(p->id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl.GetProgramInfoLog(p->id, sizeof log, NULL, log);
        fprintf(stderr, "homeplus: link: %s\n", log);
        return 0;
    }
#define U(field, name) p->field = gl.GetUniformLocation(p->id, name);
    U(rect, "u_rect") U(view, "u_view") U(uv, "u_uv") U(offset, "u_offset") U(color, "u_color") U(radius, "u_radius")
    U(tex, "u_tex") U(stroke, "u_stroke") U(grad, "u_grad") U(shape, "u_shape") U(blur, "u_blur")
    U(halfpix, "u_halfpix") U(sharp, "u_sharp") U(soft, "u_soft") U(lens, "u_lens")
    U(legible, "u_legible")
    U(light, "u_light") U(focus, "u_focus") U(tint, "u_tint") U(alpha, "u_alpha") U(mono, "u_mono")
    U(behind, "u_behind")
#undef U
    gl.UseProgram(p->id);
    gl.Uniform2f(p->view, (float)R.lw, (float)R.lh);
    gl.Uniform4f(p->uv, 0, 0, 1, 1);
    gl.Uniform1i(p->tex, 0);
    gl.Uniform1i(p->sharp, 0);
    gl.Uniform1i(p->soft, 1);
    return 1;
}

static void use(const Prog *p)
{
    if (R.cur != p) {
        gl.UseProgram(p->id);
        R.cur = p;
    }
}

static void bind_tex(int unit, GLuint id)
{
    gl.ActiveTexture(GL_TEXTURE0 + unit);
    gl.BindTexture(GL_TEXTURE_2D, id);
}

static GLuint new_texture(int w, int h, const void *px)
{
    GLuint t;
    gl.GenTextures(1, &t);
    gl.BindTexture(GL_TEXTURE_2D, t);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return t;
}

static int target(Target *t, int w, int h)
{
    t->w = w;
    t->h = h;
    t->tex = new_texture(w, h, NULL);
    gl.GenFramebuffers(1, &t->fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    int ok = gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok;
}

/* every render target, for making them and letting them go */
static Target *const ALL_TARGETS[] = { &R.backdrop[0], &R.backdrop[1], &R.soft[0], &R.soft[1], &R.d1, &R.d2, &R.d3, &R.u2 };
enum { NTARGETS = sizeof ALL_TARGETS / sizeof *ALL_TARGETS };

static void drop_targets(void)
{
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int i = 0; i < NTARGETS; i++) {
        Target *t = ALL_TARGETS[i];
        if (t->fbo) gl.DeleteFramebuffers(1, &t->fbo);
        if (t->tex) gl.DeleteTextures(1, &t->tex);
        t->fbo = t->tex = 0;
    }
    R.targets = 0;
    R.held[0] = R.held[1] = 0; /* (what they held is gone: both are drawn anew) */
}

static void bind_target(const Target *t);

/* the backdrops (cleared to black: until drawn, glass shows black through them) and the blur's steps */
static int make_targets(void)
{
    int ok = 1;
    for (int i = 0; i < 2; i++) {
        if (!target(&R.backdrop[i], BD_W, BD_H) || !target(&R.soft[i], BD_W / 2, BD_H / 2)) ok = 0;
        else {
            bind_target(&R.backdrop[i]);
            gl.ClearColor(0, 0, 0, 1);
            gl.Clear(GL_COLOR_BUFFER_BIT);
        }
    }
    ok = ok && target(&R.d1, BD_W / 2, BD_H / 2) && target(&R.d2, BD_W / 4, (BD_H + 3) / 4) &&
         target(&R.d3, BD_W / 8, (BD_H + 7) / 8) && target(&R.u2, BD_W / 4, (BD_H + 3) / 4);
    bind_target(NULL);
    if (!ok) {
        fprintf(stderr, "homeplus: render targets unavailable\n");
        drop_targets();
        return 0;
    }
    R.targets = 1;
    return 1;
}

static void bind_target(const Target *t)
{
    if (t) {
        gl.BindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        gl.Viewport(0, 0, t->w, t->h);
        R.ox = R.oy = 0;
    } else {
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        gl.Viewport(0, 0, R.dw, R.dh);
        R.ox = R.orbit_x;
        R.oy = R.orbit_y;
    }
}

static void quad(const Prog *p, float x, float y, float w, float h)
{
    gl.Uniform4f(p->rect, x, y, w, h);
    gl.Uniform2f(p->offset, R.ox + R.tx, R.oy + R.ty);
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* ------------------------------------------------------------------ setup */

void r_gl_attributes(void)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}

int r_init(SDL_Window *win, int logical_w, int logical_h)
{
    R.win = win;
    R.lw = logical_w;
    R.lh = logical_h;
    R.ctx = SDL_GL_CreateContext(win);
    if (!R.ctx) {
        fprintf(stderr, "homeplus: GL context: %s\n", SDL_GetError());
        return 0;
    }
    SDL_GL_MakeCurrent(win, R.ctx);
    SDL_GL_SetSwapInterval(1);
#define X(n)                                                                  \
    if (!(gl.n = (__typeof__(gl.n))SDL_GL_GetProcAddress("gl" #n))) {         \
        fprintf(stderr, "homeplus: missing gl" #n "\n");                     \
        return 0;                                                             \
    }
    GL_FUNCS(X)
#undef X
    SDL_GL_GetDrawableSize(win, &R.dw, &R.dh);

    if (!program(&R.tex, FS_TEX) || !program(&R.logo, FS_LOGO) || !program(&R.shape, FS_SHAPE) || !program(&R.shadow, FS_SHADOW) || !program(&R.rim, FS_RIM) ||
        !program(&R.down, FS_DOWN) || !program(&R.up, FS_UP) || !program(&R.glass, FS_GLASS) || !program(&R.sheen, FS_SHEEN))
        return 0;

    static const float unit[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    gl.GenBuffers(1, &R.quad);
    gl.BindBuffer(GL_ARRAY_BUFFER, R.quad);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof unit, unit, GL_STATIC_DRAW);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    static const unsigned char white[4] = { 255, 255, 255, 255 };
    R.white.id = new_texture(1, 1, white);
    R.white.w = R.white.h = 1;

    if (!make_targets()) return 0;

    gl.Enable(GL_BLEND);
    gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    return 1;
}

void r_shutdown(void)
{
    if (R.ctx) SDL_GL_DeleteContext(R.ctx);
    R.ctx = NULL;
}

void r_frame_begin(Uint8 r, Uint8 g, Uint8 b)
{
    SDL_GL_GetDrawableSize(R.win, &R.dw, &R.dh);
    bind_target(NULL);
    gl.ClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT);
}

void r_present(void) { SDL_GL_SwapWindow(R.win); }

void r_set_offset(float dx, float dy)
{
    R.orbit_x = dx;
    R.orbit_y = dy;
}

void r_translate(float dx, float dy)
{
    R.tx = dx;
    R.ty = dy;
}

/* ------------------------------------------------------------------ textures */

Tex *r_tex(const unsigned char *rgba, int w, int h)
{
    if (!rgba || w <= 0 || h <= 0) return NULL;
    Tex *t = malloc(sizeof *t);
    if (!t) return NULL;
    t->id = new_texture(w, h, rgba);
    t->w = w;
    t->h = h;
    t->serial = ++R.serial;
    R.tex_bytes += (size_t)w * h * 4;
    R.textures++;
    return t;
}

void r_tex_free(Tex **t)
{
    if (!*t) return;
    gl.DeleteTextures(1, &(*t)->id);
    R.tex_bytes -= (size_t)(*t)->w * (*t)->h * 4;
    R.textures--;
    free(*t);
    *t = NULL;
}

size_t r_texture_bytes(int *count)
{
    if (count) *count = R.textures;
    return R.tex_bytes;
}

size_t r_target_bytes(void)
{
    size_t n = 0;
    for (int i = 0; R.targets && i < NTARGETS; i++) n += (size_t)ALL_TARGETS[i]->w * ALL_TARGETS[i]->h * 4;
    return n;
}

size_t r_targets_release(void)
{
    size_t n = r_target_bytes();
    if (R.targets) drop_targets();
    return n;
}

/* ------------------------------------------------------------------ drawing */

static void image_uv(const Tex *t, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                     Uint8 r, Uint8 g, Uint8 b, Uint8 a, float radius)
{
    if (!t || !a || w <= 0 || h <= 0) return;
    if (noting(1, t, (float[]){ x, y, w, h, u0, v0, u1, v1, r, g, b, a, radius }, 13)) return;
    use(&R.tex);
    bind_tex(0, t->id);
    gl.Uniform4f(R.tex.uv, u0, v0, u1, v1);
    gl.Uniform4f(R.tex.color, r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
    gl.Uniform1f(R.tex.radius, radius);
    quad(&R.tex, x, y, w, h);
}

void r_image(const Tex *t, float x, float y, float w, float h, Uint8 r, Uint8 g, Uint8 b, Uint8 a, float radius)
{
    image_uv(t, x, y, w, h, 0, 0, 1, 1, r, g, b, a, radius);
}

void r_image_part(const Tex *t, float x, float y, float w, float h, float u0, float u1, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    image_uv(t, x, y, w, h, u0, 0, u1, 1, r, g, b, a, 0);
}

void r_logo(const Tex *t, float x, float y, float w, float h, float radius, const LogoLook *l, Uint8 a)
{
    if (!t || !a || w <= 0 || h <= 0) return;
    if (noting(2, t, (float[]){ x, y, w, h, radius, l->mono, l->ref, l->span, l->faint, l->ink, l->r, l->g, l->b, a }, 14)) return;
    use(&R.logo);
    bind_tex(0, t->id);
    gl.Uniform4f(R.logo.color, l->r / 255.0f, l->g / 255.0f, l->b / 255.0f, l->mono ? 1 : l->ink);
    gl.Uniform4f(R.logo.mono, l->mono ? 1 : 0, l->ref, l->span > 0.02f ? 1 / l->span : 1, l->faint);
    gl.Uniform1f(R.logo.alpha, a / 255.0f);
    gl.Uniform1f(R.logo.radius, radius);
    quad(&R.logo, x, y, w, h);
}

void r_cover(const Tex *t, float x, float y, float w, float h, Uint8 a, float radius)
{
    if (!t) return;
    float s = fmaxf(w / t->w, h / t->h);
    float sw = w / s, sh = h / s;
    float x0 = (t->w - sw) / 2, y0 = (t->h - sh) / 4; /* bias upward, like the old cover crop */
    image_uv(t, x, y, w, h, x0 / t->w, y0 / t->h, (x0 + sw) / t->w, (y0 + sh) / t->h, 255, 255, 255, a, radius);
}

void r_shape(float x, float y, float w, float h, float radius, Uint8 r, Uint8 g, Uint8 b, Uint8 a,
             float stroke, float top_a, float bottom_a)
{
    if (!a || w <= 0 || h <= 0) return;
    if (noting(3, NULL, (float[]){ x, y, w, h, radius, r, g, b, a, stroke, top_a, bottom_a }, 12)) return;
    use(&R.shape);
    gl.Uniform4f(R.shape.color, r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
    gl.Uniform1f(R.shape.radius, fminf(radius, fminf(w, h) / 2));
    gl.Uniform1f(R.shape.stroke, stroke);
    gl.Uniform2f(R.shape.grad, top_a, bottom_a);
    quad(&R.shape, x, y, w, h);
}

static void shadow(float x, float y, float w, float h, float radius, float blur, float dy, float alpha, int under)
{
    if (alpha <= 0 || w <= 0 || h <= 0) return;
    if (noting(4, NULL, (float[]){ x, y, w, h, radius, blur, dy, alpha, under }, 9)) return;
    use(&R.shadow);
    radius = fminf(radius, fminf(w, h) / 2);
    gl.Uniform4f(R.shadow.color, 0, 0, 0, alpha);
    gl.Uniform4f(R.shadow.shape, x, y + dy, w, h);
    gl.Uniform1f(R.shadow.radius, radius);
    gl.Uniform1f(R.shadow.blur, blur);
    float qx = x - blur * 1.5f, qy = y + dy - blur * 1.5f, qw = w + blur * 3, qh = h + blur * 3;
    /* what the shape covers: inside its corners' arcs (0.3 r in), on whole pixels, so the four bands
     * around it meet exactly (no seam, no pixel drawn twice) */
    float in = radius * 0.3f + 1, hx0 = ceilf(x + in), hy0 = ceilf(y + in), hx1 = floorf(x + w - in), hy1 = floorf(y + h - in);
    if (!under || hx1 - hx0 < 8 || hy1 - hy0 < 8 || hy0 < qy || hy1 > qy + qh) {
        quad(&R.shadow, qx, qy, qw, qh);
        return;
    }
    float x0 = floorf(qx), y0 = floorf(qy), x1 = ceilf(qx + qw), y1 = ceilf(qy + qh);
    quad(&R.shadow, x0, y0, x1 - x0, hy0 - y0);   /* above it, */
    quad(&R.shadow, x0, hy1, x1 - x0, y1 - hy1);  /* below, */
    quad(&R.shadow, x0, hy0, hx0 - x0, hy1 - hy0); /* and either side */
    quad(&R.shadow, hx1, hy0, x1 - hx1, hy1 - hy0);
}

void r_shadow(float x, float y, float w, float h, float radius, float blur, float dy, float alpha)
{
    shadow(x, y, w, h, radius, blur, dy, alpha, 0);
}

void r_shadow_under(float x, float y, float w, float h, float radius, float blur, float dy, float alpha)
{
    shadow(x, y, w, h, radius, blur, dy, alpha, 1);
}

void r_rim(float x, float y, float w, float h, float radius, float width, Uint8 a, float light_x, float light_y)
{
    if (!a || w <= 0 || h <= 0) return;
    if (noting(5, NULL, (float[]){ x, y, w, h, radius, width, a, light_x, light_y }, 9)) return;
    float ll = sqrtf(light_x * light_x + light_y * light_y);
    if (ll < 0.001f) { light_x = -0.6f; light_y = -0.8f; ll = 1; }
    use(&R.rim);
    gl.Uniform4f(R.rim.color, 1, 1, 1, a / 255.0f);
    gl.Uniform1f(R.rim.radius, fminf(radius, fminf(w, h) / 2));
    gl.Uniform1f(R.rim.stroke, width);
    gl.Uniform2f(R.rim.light, light_x / ll, light_y / ll);
    quad(&R.rim, x, y, w, h);
}

void r_fill(Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    /* past the edges by more than the pixel orbit moves things: shifted with everything else, an
     * exact fill left a strip at one edge where the next row of tiles showed through the black */
    r_shape(-16, -16, (float)R.lw + 32, (float)R.lh + 32, 0, r, g, b, a, 0, 1, 1);
}

/* ------------------------------------------------------------------ backdrop + glass */

void r_backdrop_note(void)
{
    H.on = 1;
    H.h = 1469598103934665603ULL;
}

int r_backdrop_unchanged(int which)
{
    H.on = 0;
    if (R.held[which] && R.print[which] == H.h) return 1;
    R.print[which] = H.h;
    R.held[which] = 1;
    return 0;
}

unsigned r_backdrops_drawn(int which) { return R.drawn[which]; }

void r_backdrop_begin(int which)
{
    if (!R.targets) make_targets(); /* (let go while hidden: made again for the first frame back) */
    R.drawing = which;
    R.drawn[which]++;
    bind_target(&R.backdrop[which]);
    gl.ClearColor(0, 0, 0, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT);
}

void r_use_backdrop(int which) { R.used = which; }

/* one full-target blur pass; FBO textures are bottom-up, hence the flipped v. The clear first tells a
 * tiled GPU (the C3's Mali) it needn't load what the target held: the pass covers all of it. */
static void blur_pass(const Prog *p, const Target *src, const Target *dst)
{
    bind_target(dst);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    use(p);
    bind_tex(0, src->tex);
    gl.Uniform2f(p->halfpix, 0.5f / src->w, 0.5f / src->h);
    gl.Uniform4f(p->uv, 0, 1, 1, 0);
    quad(p, 0, 0, (float)R.lw, (float)R.lh);
}

void r_backdrop_end(void)
{
    gl.Disable(GL_BLEND);
    blur_pass(&R.down, &R.backdrop[R.drawing], &R.d1);
    blur_pass(&R.down, &R.d1, &R.d2);
    blur_pass(&R.down, &R.d2, &R.d3);
    blur_pass(&R.up, &R.d3, &R.u2);
    blur_pass(&R.up, &R.u2, &R.soft[R.drawing]);
    gl.Enable(GL_BLEND);
    bind_target(NULL);
}

void r_backdrop_image(int blurred, Uint8 alpha, Uint8 dim)
{
    if (!alpha) return;
    if (noting(6, NULL, (float[]){ blurred, alpha, dim, R.used }, 4)) return;
    const Target *t = blurred ? &R.soft[R.used] : &R.backdrop[R.used];
    float k = alpha == 255 ? 1 - dim / 255.0f : 1; /* over nothing: dimmed as it's drawn, no fill over the screen */
    use(&R.tex);
    bind_tex(0, t->tex);
    gl.Uniform4f(R.tex.uv, 0, 1, 1, 0); /* FBO textures are bottom-up */
    gl.Uniform4f(R.tex.color, k, k, k, alpha / 255.0f);
    gl.Uniform1f(R.tex.radius, 0);
    quad(&R.tex, 0, 0, (float)R.lw, (float)R.lh);
    gl.Uniform4f(R.tex.uv, 0, 0, 1, 1);
    if (alpha < 255 && dim) r_fill(0, 0, 0, dim);
}

void r_glass(const Glass *g)
{
    if (g->alpha <= 0 || g->w <= 0 || g->h <= 0) return;
    if (noting(7, NULL, (float[]){ g->x, g->y, g->w, g->h, g->radius, g->legible, g->lens, g->light_x, g->light_y, g->focus,
                                   g->alpha, g->tr, g->tg, g->tb, g->ta, g->blurred, g->dimmed, R.used }, 18))
        return;
    use(&R.glass);
    bind_tex(1, R.soft[R.used].tex);
    bind_tex(0, R.backdrop[R.used].tex);
    float lx = g->light_x, ly = g->light_y, ll = sqrtf(lx * lx + ly * ly);
    if (ll < 0.001f) { lx = -0.6f; ly = -0.8f; ll = 1; }
    gl.Uniform1f(R.glass.radius, fminf(g->radius, fminf(g->w, g->h) / 2));
    gl.Uniform1f(R.glass.lens, g->lens);
    gl.Uniform1f(R.glass.legible, g->legible);
    gl.Uniform2f(R.glass.light, lx / ll, ly / ll);
    gl.Uniform1f(R.glass.focus, g->focus);
    gl.Uniform4f(R.glass.tint, g->tr / 255.0f, g->tg / 255.0f, g->tb / 255.0f, g->ta / 255.0f);
    gl.Uniform1f(R.glass.alpha, g->alpha);
    gl.Uniform2f(R.glass.behind, g->blurred, 1 - g->dimmed);
    quad(&R.glass, g->x, g->y, g->w, g->h);
}

/* ------------------------------------------------------------------ GPU timing */

/* GL_EXT_disjoint_timer_query: a few frames' queries in flight, read back in order */
enum { GPU_Q = 4 };
static struct {
    void (GL_APIENTRYP gen)(GLsizei, GLuint *);
    void (GL_APIENTRYP begin)(GLenum, GLuint);
    void (GL_APIENTRYP end)(GLenum);
    void (GL_APIENTRYP get)(GLuint, GLenum, GLuint *);
    void (GL_APIENTRYP get64)(GLuint, GLenum, khronos_uint64_t *);
    void (GL_APIENTRYP bits)(GLenum, GLenum, GLint *);
    GLuint q[GPU_Q];
    unsigned begun, read;
    int tried, ok, active;
} T;

int r_gpu_timer(void)
{
    if (T.tried) return T.ok;
    T.tried = 1;
    const char *ext = (const char *)gl.GetString(GL_EXTENSIONS);
    if (!ext || !strstr(ext, "GL_EXT_disjoint_timer_query")) return 0;
    T.gen = SDL_GL_GetProcAddress("glGenQueriesEXT");
    T.begin = SDL_GL_GetProcAddress("glBeginQueryEXT");
    T.end = SDL_GL_GetProcAddress("glEndQueryEXT");
    T.get = SDL_GL_GetProcAddress("glGetQueryObjectuivEXT");
    T.get64 = SDL_GL_GetProcAddress("glGetQueryObjectui64vEXT");
    T.bits = SDL_GL_GetProcAddress("glGetQueryivEXT");
    if (!T.gen || !T.begin || !T.end || !T.get || !T.get64 || !T.bits) return 0;
    GLint bits = 0;
    T.bits(GL_TIME_ELAPSED_EXT, GL_QUERY_COUNTER_BITS_EXT, &bits); /* 0: it has the extension but can't time */
    if (bits <= 0) return 0;
    T.gen(GPU_Q, T.q);
    return T.ok = 1;
}

void r_finish(void) { gl.Finish(); }

const char *r_gl_info(int what) { return (const char *)gl.GetString(what ? GL_VERSION : GL_RENDERER); }

void r_gpu_begin(void)
{
    if (!T.ok || T.active || T.begun - T.read >= GPU_Q) return; /* all of them still on the GPU: this frame goes untimed */
    T.begin(GL_TIME_ELAPSED_EXT, T.q[T.begun % GPU_Q]);
    T.active = 1;
}

void r_gpu_end(void)
{
    if (!T.active) return;
    T.end(GL_TIME_ELAPSED_EXT);
    T.begun++;
    T.active = 0;
}

long r_gpu_result(int wait)
{
    if (T.read == T.begun) return -1;
    GLuint q = T.q[T.read % GPU_Q], in = 0;
    if (!wait && (T.get(q, GL_QUERY_RESULT_AVAILABLE_EXT, &in), !in)) return -1;
    khronos_uint64_t ns = 0;
    T.get64(q, GL_QUERY_RESULT_EXT, &ns);
    T.read++;
    GLint disjoint = 0;
    gl.GetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    return disjoint ? -2 : (long)(ns / 1000);
}

void r_sheen(float x, float y, float w, float h, float radius, float light_x, float light_y, float focus)
{
    if (focus <= 0.004f || w <= 0 || h <= 0) return;
    if (noting(8, NULL, (float[]){ x, y, w, h, radius, light_x, light_y, focus }, 8)) return;
    float ll = sqrtf(light_x * light_x + light_y * light_y);
    if (ll < 0.001f) { light_x = -0.6f; light_y = -0.8f; ll = 1; }
    use(&R.sheen);
    gl.Uniform1f(R.sheen.radius, fminf(radius, fminf(w, h) / 2));
    gl.Uniform2f(R.sheen.light, light_x / ll, light_y / ll);
    gl.Uniform1f(R.sheen.focus, focus);
    gl.BlendFuncSeparate(GL_ONE, GL_SRC_ALPHA, GL_ZERO, GL_ONE);
    quad(&R.sheen, x, y, w, h);
    gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

int r_read_pixels(unsigned char *buf, int *w, int *h)
{
    *w = R.dw;
    *h = R.dh;
    if (!buf) return 0;
    bind_target(NULL);
    gl.ReadPixels(0, 0, R.dw, R.dh, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    size_t row = (size_t)R.dw * 4;
    unsigned char *tmp = malloc(row);
    if (!tmp) return 0;
    for (int y = 0; y < R.dh / 2; y++) { /* GL rows are bottom-up */
        memcpy(tmp, buf + y * row, row);
        memcpy(buf + y * row, buf + (R.dh - 1 - y) * row, row);
        memcpy(buf + (R.dh - 1 - y) * row, tmp, row);
    }
    free(tmp);
    return 1;
}
