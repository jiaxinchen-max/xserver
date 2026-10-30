# Xlorie r8: GNOME GLX and exact RandR refresh

Based on `termuxdc` commit `d9d2bd1d32a4965a07c4b1c5ed27cef84167fcb3`.

- Add client Mesa depth24/stencil8 FBConfigs required by Mutter/Cogl GL3, retaining existing configurations.
- Initialize GLXWindow/GLXPixmap identity with the GLX resource ID, so drawable attributes and paired-resource destruction work with distinct X/GLX IDs.
- Publish exact virtual-output RandR timings and refresh-only changes. Reject invalid explicit framerates and unrepresentable pixel clocks.
- Abort on GLX provider initialization failure; do not fall through to swrast.

Regression commands (Linux C compiler with ASan/UBSan):

```sh
python3 tests/lorie/test-xserver-randr-timings.py .
python3 tests/lorie/test-xserver-glx-fbconfigs.py .
python3 tests/lorie/test-xserver-glx-drawable-id.py .
python3 tests/lorie/test-xserver-glx-provider-failfast.py .
```

On Xiaomi Mi 11 Ultra / Adreno 660, App-launched PRoot GNOME uses explicit GL3/GLX, Mesa Zink and the KGSL Turnip ICD. The validated path imports 1280x720 DRI3 buffers, presents actual GNOME/Files frames and reports 30 Hz. Files navigation, touch scrolling and portrait/landscape Surface recreation were exercised through the App.

`r8-BUILD-INPUTS.json` pins the source hashes, binary and packaging inputs. `repack-xorg-server-r8.py` keeps the complete approved r6 payload and verifies that exactly Xlorie and necessary control metadata change. The r7 packaging script remains unchanged for historical reproduction.
