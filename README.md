# runtime

A lean 3DS system for the recompiled Pokémon ports (Solstice, Duality,
Tidefall): the kernel calls and system services these games ask for, made
to measure, in place of a whole emulator. The ports link it with the games'
code as C (the decomp's, else 3dsrecomp's) and, later, a native Vulkan
renderer; Zakuro stays the emulator and the decomp's checker.

What each game uses and why the lists are what they are:
`Solstice/docs/lean-runtime.md` (measured with Zakuro's `ZAKURO_USAGE` and
read from the code with `decomp-tools/ida/ipc_survey.py`).

## Status

Skeleton: the lists of syscalls and services (`crates/runtime`). Next: the
kernel and the native services, adapted from Zakuro's, run under Zakuro's
CPU first so that their answers can be compared with Zakuro's on the same
session.

## Using it from a port

```toml
[dependencies]
recomp-runtime = { path = "../runtime/crates/runtime" }
```

## License

MIT. Parts are adapted from Zakuro (MIT, Copyright (c) 2026 fearkov); each
such file says so at its top.

Nothing derived from the games is ever committed here.
