# Third-party notices

Home+ includes the following third-party code and assets. Each keeps its own licence; the
full texts are in the files listed, and they ship inside the `.ipk` with the app.

| Component | Where | Version | Licence | Licence text |
|---|---|---|---|---|
| SDL 2 headers | `native/third_party/SDL2/` | 2.0.14 | zlib | `native/third_party/SDL2/LICENSE-SDL2.txt` |
| OpenGL ES 2 headers (`SDL_opengles2_gl2*.h`) | `native/third_party/SDL2/` | as shipped with SDL 2.0.14 | SGI Free Software License B 2.0 | below |
| Khronos platform / EGL / GL extension headers (`SDL_opengles2_khrplatform.h`, `SDL_egl.h`, `SDL_opengl_glext.h`) | `native/third_party/SDL2/` | as shipped with SDL 2.0.14 | Khronos (MIT-style) | below |
| Mesa OpenGL header (`SDL_opengl.h`) | `native/third_party/SDL2/` | as shipped with SDL 2.0.14 | MIT (Brian Paul, VMware) | in the file's header |
| cJSON | `native/third_party/cJSON.c`, `cJSON.h` | 1.7.x | MIT | `native/third_party/LICENSE-cJSON` |
| stb_image, stb_image_write, stb_truetype | `native/third_party/stb_*.h` | 2.30 / 1.16 / 1.26 | MIT or public domain (your choice) | end of each file |
| Inter typeface | `native/assets/fonts/Inter-*.ttf` | 4.0 | SIL Open Font License 1.1 | `native/assets/fonts/LICENSE-Inter.txt` |

Home+ doesn't link SDL, EGL or OpenGL ES statically: at run time it uses the copies that
come with webOS on the TV. Only the headers above are part of this repository.

## SGI Free Software License B, Version 2.0

The OpenGL ES 2 headers carry this notice:

> This document is licensed under the SGI Free Software B License Version 2.0.
> For details, see http://oss.sgi.com/projects/FreeB/ .

SGI Free Software License B 2.0 is equivalent to the X11 (MIT) licence:

> Copyright (C) 1991-2000 Silicon Graphics, Inc. All Rights Reserved.
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this
> software and associated documentation files (the "Software"), to deal in the Software
> without restriction, including without limitation the rights to use, copy, modify, merge,
> publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
> to whom the Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice including the dates of first publication and either this
> permission notice or a reference to http://oss.sgi.com/projects/FreeB/ shall be included
> in all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
> INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
> PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL SILICON GRAPHICS, INC. BE LIABLE FOR ANY
> CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
> ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
> THE SOFTWARE.
>
> Except as contained in this notice, the name of Silicon Graphics, Inc. shall not be used
> in advertising or otherwise to promote the sale, use or other dealings in this Software
> without prior written authorization from Silicon Graphics, Inc.

## Khronos Group

The Khronos headers carry this licence:

> Copyright (c) 2008-2014 The Khronos Group Inc.
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this
> software and/or associated documentation files (the "Materials"), to deal in the Materials
> without restriction, including without limitation the rights to use, copy, modify, merge,
> publish, distribute, sublicense, and/or sell copies of the Materials, and to permit persons
> to whom the Materials are furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or
> substantial portions of the Materials.
>
> THE MATERIALS ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
> INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
> PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
> FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
> OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE MATERIALS OR THE USE OR OTHER
> DEALINGS IN THE MATERIALS.

## Not included

Home+ works with Stremio, Trakt, Homebrew Channel and lginputhook but contains none of their
code. Stremio artwork comes from `images.metahub.space` at run time and isn't redistributed.
