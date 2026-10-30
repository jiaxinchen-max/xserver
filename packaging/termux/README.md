# Xlorie r7 package inputs

`repack-xorg-server.py` produces the complete `xorg-server_21.1.16-7_aarch64.deb`
from the pinned r6 package and the device-validated internal v8 Xlorie. It changes
only the Xlorie payload bytes/mtime and package Version/Installed-Size fields.
Every other payload entry, mode, owner, symlink and timestamp must stay identical.
It does not install packages or replace the rest of the Termux package payload.

Run packaging on the designated Linux build host (Python 3.9+, dpkg-deb, xz):

```sh
python3 source/packaging/termux/repack-xorg-server.py \
  --base-deb base/xorg-server_21.1.16-6_aarch64.deb \
  --xlorie verified/Xlorie \
  --output-dir output \
  --version 21.1.16-7 \
  --source-date-epoch SOURCE_COMMIT_EPOCH
```

Use the recorded commit epoch from the release input manifest for the last
argument. The script requires every input explicitly, verifies fixed input
SHA256 values, refuses existing outputs, and writes a complete payload-diff
report. The release input archive retains the public r6 base deb so packaging
remains possible after the old release is removed. Two independent output
runs must produce the same deb SHA256.

The current source already includes the eight ordered patches in `patches/series`;
do not reapply them to this tree. Their original base is commit
`b08f1752300f3d594694e09f5d8f980978becc4a` on `termuxdc`.

This reproduces packaging from a fixed validated binary, not a bit-identical
native rebuild under an arbitrary toolchain. Native compilation used the user's
designated `/workspace/termux-packages/x11-packages/xorg-server/build.sh` environment
in an isolated build directory. Source regression harnesses are in `tests/lorie`.
No device logs, traces, caches, databases or captured pixels belong in public inputs.
