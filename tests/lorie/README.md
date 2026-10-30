# Xlorie regression tests

Run on the designated Linux build host, with Python 3 and a C compiler:

```sh
for test in tests/lorie/test-xserver-*.py; do
    python3 "$test" "$PWD" --cc /usr/bin/cc
done
```

The tests extract and exercise actual source functions with deterministic API mocks and sanitizers. They cover strict renderer/ICD selection, GLX registration, readback/format conversion, GPU allocation rejection, Present scheduling/concurrency and A8 picture upload. They do not replace formal App UI validation or require any device data.

`gles-probes/gles-gradient-probe.c` is the separate real-GLES precision diagnostic. Compile it only on the build machine; its explicit System/precision arguments compare deterministic synthetic gradients. Device output is not included in this repository or release.
