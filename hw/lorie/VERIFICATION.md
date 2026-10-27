# Xlorie / termux-render verification notes

Living record of what was actually verified on devices, and of the known incompatibilities of
the independent Xlorie process with the termux-app display bridge. Complement to the release
notes of `termux-xlorie-21.1.16-r5`; everything below was re-checked on a Mi 11 Ultra
(Adreno 660, Android 14) and on the arm64 emulator with the app's Desktop/Game session view
as the display bridge.

## Verified working (app view as bridge)

| Server build | Mode | Clients | Result |
|---|---|---|---|
| 56af3a0+ | system GLES, native AHB | xclock, gl4es glxgears | renders, correct colours, ~3000 FPS (gl4es, `LIBGL_FB=3`) |
| 56af3a0+ | zink/Turnip, dma-buf | Mesa glxgears (`vblank_mode=0`) | renders, correct colours, ~1500 FPS; DRI3 imports ok |
| 56af3a0+ | zink/Turnip, dma-buf | native XFCE4 (xfwm4 compositor) | full desktop renders |
| 56af3a0+ | zink/Turnip, dma-buf | proot Debian: glxgears, xfdesktop | render (~300-450 FPS gears; wallpaper+icons) |
| 56af3a0+ | llvmpipe (no EGL import exts) | xclock, glxgears | software fb path, correct colours (format 2) |
| release r5 | all of the above | same clients | clients report FPS but the app view stays BLACK (see fixes in 56af3a0) |

glxgears without `vblank_mode=0` prints ~1 FPS or nothing at all; that is vsync against the
bridge, not a rendering failure.

## Known incompatibilities / open issues

1. **KWin X11 backend cannot composite on Xlorie.** `kwin_x11` (kwin 6.6.0-19,
   `kwin-x11-zink`) fails with `kwin_scene_opengl: Creating the OpenGL rendering failed:
   "Invalid QOpenGLContext::globalShareContext()"` and exits. Xlorie's minimal GLX provider
   offers window/pixmap configs only (no pbuffer, no indirect), which the Qt X11 OpenGL scene
   needs. KWin must use its Wayland backend against termux-render (verified working,
   kwin release r19, native and proot).
2. **glibc xfwm4 as window manager blacks out the root window.** (root cause narrowed 2026-10-26:
   the container's Mesa initialises GLX/DRI3 against Xlorie's minimal GLX provider; the session's
   root presentation then stops reaching the app view while the server keeps flushing and the app
   keeps presenting. Present-flip is NOT involved (zero check_flip calls). Mitigation shipped:
   `XLORIE_GLX=0` advertises an empty GLX (no fbconfigs) so glibc clients fall back to XRender,
   which glamor composites on the GPU; the provider stays on the stack so swrast is never probed.
   Desktop flows that need client GL (gl4es games) keep GLX by default. Hardening also shipped:
   presents aimed at the root window never flip, and a live flip is undone as soon as the root
   takes new damage (loriePresentFlip/Unflip track `presentFlipped`). Original trigger details:
   container xfwm4 4.20 logs Mesa `failed to load driver: (null)`, server logs 1280x720 DRI3
   imports + one `fds_from_pixmap` at wm start; killing xfwm4 restores the picture. With a proot Debian XFCE4
   session on Xlorie (zink), adding the container's `xfwm4` makes the whole root presentation
   black for every client (native and glibc); killing `xfwm4` restores it. Disabling
   xfwm4's compositor (`xfconf /general/compositing_manager false`) does NOT help, so it is
   not the composite-redirect path. The native (Bionic) xfwm4 never shows this. Container
   `xfdesktop` and `glxgears` alone render fine, so MIT-SHM uploads and DRI3 dma-buf imports
   from glibc clients are not the cause. The container xfwm4 logs a Mesa
   `failed to load driver: (null)` at start. UNRESOLVED - needs an instrumented build.
3. **system GLES + forced dma-buf receive never works on Adreno**: `eglCreateImageKHR` fails,
   the root buffer cannot be GPU-backed, screen stays black. `lorieRenderChooseReceiveMode()`
   therefore defaults to native handles for system mode; do not "test" dma-buf with system EGL.
4. **Second client on the bridge fails cleanly.** termux-render is single-client; a second
   Xlorie/KWin gets `Connection closed` / a 10 s init timeout and exits, the first client keeps
   working. Expected, but plan launches accordingly.
5. Other ABIs (x86_64/arm/i686) were never built: the packaging tree lacks per-arch dependency
   sysroots (`configure: C compiler cannot create executables`). The code is arch-neutral C.

- Xft/Cairo text (GTK, xfwm4) drew as solid black boxes on GLES stacks whose R8 framebuffer
  read format is not GL_RED (seen on the Android emulator's GLES translator): glamor skipped the
  depth-8 format, so A8 Render masks/glyphs uploaded as invalid textures that sample opaque.
  `glamor_setup_formats` now registers a non-renderable GL_ALPHA fallback for depth 8/1.
  Verified on the arm64 emulator (XRender A8 gradient mask draws a gradient; XFCE text readable).
  Not yet re-verified on a real Adreno device, where the read-format check normally passes.
- Input over the bridge: the bridge connects after InitInput(), so `lorieInputRefresh()` (called
  from the block handler) registers/follows the current input fd.

## Test recipes

- Server: `Xlorie :1 -noreset -ac -nolisten tcp -screen 0 1280x720x24 -framerate 30
  -render-socket $HOME/tmp/termux-render -xkbdir $PREFIX/share/X11/xkb`, with the app session
  view already bound (the bridge only listens while the view exists).
- system: `XLORIE_GL_MODE=system TERMUX_RENDER_AHB_RECEIVE=native`;
  gl4es clients need `LIBGL_FB=3 LD_LIBRARY_PATH=$PREFIX/lib/gl4es`.
- zink: source `kwin-android-rendering-env` + `kwin_android_select_rendering` (kwin package)
  or set `MESA_LOADER_DRIVER_OVERRIDE/GALLIUM_DRIVER=zink`, `VK_ICD_FILENAMES` to the Turnip ICD,
  `TERMUX_RENDER_AHB_RECEIVE=dmabuf`; clients add `vblank_mode=0`.
- llvmpipe: `XLORIE_GL_MODE=llvmpipe` (+ `TERMUX_ANDROID_ZINK=1` so libepoxy picks Termux Mesa);
  falls back to the software framebuffer when Mesa EGL lacks AHB/dma-buf import.
- Stale-state traps: kill a previous Xlorie and remove `$PREFIX/tmp/.X11-unix/X<n>` before
  relaunching, otherwise clients attach to a server without a bridge and the view stays black.
