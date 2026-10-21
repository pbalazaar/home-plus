# Home+ — notes for coding agents

Home+'s development notes are in [CLAUDE.md](CLAUDE.md): what the pieces are, how to build, test and install it, and the webOS quirks found along the way. It's the one set kept up to date, so they're not repeated here; read it before changing anything, and [CONTRIBUTING.md](CONTRIBUTING.md) for how changes are sent.

## Where the launcher's code goes

The launcher (`native/src`) is a file per feature, each with that feature's data, logic and drawing; `home.h` lists them, and holds what they share: the layout, the types and the one global state, `S`. `main.c` only starts it and runs the frame loop, so:

- Put new code in its feature's file, not in `main.c`. A feature big enough for its own file gets one, added to `SRC` in `native/Makefile` and to `home.h`'s list.
- A function stays `static` unless another file calls it; then its prototype goes in `home.h`, under its file's name.
- `render.c`, `gfx.c`, `net.c`, `sound.c` and `luna.c` are libraries under the launcher, with their own headers: they don't include `home.h` or touch `S`.
- When a change should draw the same, render the screens before and after it and compare them with `tools/native-preview/compare-screens.py` (see CLAUDE.md).
