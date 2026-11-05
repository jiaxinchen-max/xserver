# Xlorie 21.1.16 r9

Remove the unsupported GLX_INTEL_swap_event extension from the minimal GLX provider. Mutter must not wait for swap completion events that this provider never sends.

The aarch64 Xlorie binary was compiled on the Termux package builder with the xorg-server recipe and NDK android-r30-api-24-v0, in an isolated copy of the configured r8 build. Only lorie-glx.o was rebuilt before relinking Xlorie; the shared sysroot was not installed into.

The release build-inputs archive carries the complete Git source snapshot, actual configured source, recipe/configuration, published complete r8 base deb, verified r9 binary, and exact packaging inputs. The packaging script runs on the package builder and changes only Xlorie plus package version/size metadata. The pinned-binary deb packaging is reproducible; no bit-identical native rebuild is claimed.

Runtime validation through the App: Pixel Fold GNOME GL3 with explicit LLVMpipe, overview/app grid/Files/direct touch/audio; Xiaomi Mi 11 Ultra GNOME GL3 with explicit Zink/Turnip, overview/Files/clock redraw. Neither used renderer fallback or a drirc workaround. Fold validation used a temporarily disabled phantom process limit.
