# Xlorie r7 verification and runtime contract

The r7 code contains the eight fixes tested as the internal v8 candidate. The shipped
AArch64 Xlorie SHA256 is `741e9b87cbf01ea48e2db89648bd2fd1441b6df2ddadf70ebdf05793f4544ef6`.

## Explicit runtime selection

- Set `XLORIE_GL_MODE=system`, `zink`, or `llvmpipe`. Missing, unknown or conflicting
  `KWIN_ANDROID_GL_MODE` values fail. Initialization or GPU resource failure never selects
  a different renderer, EGL display, GLES version or software framebuffer.
- Set `TERMUX_RENDER_AHB_RECEIVE=native` for the App System EGL path, or `dmabuf` for
  its Mesa paths. Import failure does not change the selected ABI.
- Set `XLORIE_GLX=1` for the native XFCE path and `0` for container XFCE. Zero disables
  extension/provider/vendor registration entirely; it does not advertise empty fbconfigs.
- Zink requires one readable matching ICD and a hardware Vulkan renderer; llvmpipe
  requires the selected software Mesa renderer. System uses Android EGL.

## Verified behavior

Formal App UI checks passed on the arm64 Android emulator and Xiaomi Mi 11 Ultra:
System EGL native/container XFCE window rendering and refresh, context menus, text,
touch navigation, scrolling and orientation. Container XFCE uses Debian trixie.
Container Plasma uses Debian forky and the separate KWin Wayland backend; cross-version
container combinations are outside the supported scope.

The A8 temporary-picture path uses a renderable R8 texture with explicit channel
swizzles; no CPU pixmap fallback is introduced. The readback paths preserve channel
values, rectangle bounds and row padding. Unsupported exact depth-30 pixmaps return
BadAlloc. Gradient fragment evaluation uses high precision, guaranteed by the required GLES3 API. Present
deadlines and shared-lock publication prevent stalled refreshes and lost wakeups.

The source regression tests are in `tests/lorie`. They cover the actual mode parser,
GLX registration, readback, resource failures, format transfers, Present scheduling,
and memory-picture upload; a separate real-GLES probe covers shader precision. Native compilation and these tests
run on the designated build machine, not on the local development host.

Xlorie Mesa Zink/llvmpipe formal UI coverage is separate from parser/unit coverage;
these modes are not claimed as newly accepted by this r7 System EGL UI record.
Run one managed desktop per App display bridge. App JNI connection-owner handling
is outside the scope of this Xserver release.

## Packaging

The r7 deb retains the complete r6 package payload and dependencies, replacing only
`$PREFIX/bin/Xlorie` with the verified binary and updating package version/size metadata.
`packaging/termux/repack-xorg-server.py` validates the fixed input hashes and rejects
unexpected payload changes. It reproduces packaging from the retained input binary;
it does not claim a bit-identical native rebuild from a different toolchain.
