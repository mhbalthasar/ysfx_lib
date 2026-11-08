# ysfx_lib

This is a fork of [ysfx](https://github.com/JoepVanlier/ysfx) packaged as a
self-contained shared C ABI library. TuneLab loads it from the x64 bridge
process to run JSFX effects; the host never executes EEL2 itself.

The upstream plugin target is disabled by default. Enabling it is possible,
but it is not part of the CI contract.

## Build contract

- Library target: `ysfx`
- Shared library artifact: `ysfx.tlhost.dll`, `libysfx.tlhost.so`, or
  `libysfx.tlhost.dylib` (the CI builds the generic `ysfx` target and renames
  the installed file during packaging; `YSFX_LIBRARY_OUTPUT_NAME` can also
  set the name directly on generators that preserve dots)
- Public header: `include/ysfx.h`
- Graphics: `YSFX_GFX=ON`, which provides the offscreen `@gfx` API
- State: slider state plus `@serialize` blobs through `ysfx_save_state` /
  `ysfx_load_state`
- Audio: 2-in/2-out float and double processing (the bridge chooses float)

The output name is left configurable so the repository can produce either a
generic `ysfx` library or a TuneLab-named `ysfx.tlhost` library:

```bash
cmake -S . -B build \
  -DYSFX_LIBRARY=ON \
  -DYSFX_LIBRARY_OUTPUT_NAME=ysfx.tlhost \
  -DYSFX_GFX=ON \
  -DYSFX_PLUGIN=OFF
cmake --build build --target ysfx
```

On x86-64, leave `YSFX_PORTABLE=OFF` to use the EEL2 assembly path. On
ARM64/LoongArch64, set `YSFX_PORTABLE=ON` so the portable EEL2 path is used.
The CI sets this per target.

### Windows

Use the Visual Studio generator so the ARM64 and x64 toolsets are selected
correctly:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DYSFX_LIBRARY=ON -DYSFX_LIBRARY_OUTPUT_NAME=ysfx.tlhost `
  -DYSFX_GFX=ON -DYSFX_PLUGIN=OFF -DYSFX_STATIC_CRT=ON
cmake --build build --config Release --target ysfx
```

The x64 build needs NASM for the EEL2 assembly path. `YSFX_PORTABLE=ON` avoids
that dependency, which is what the ARM64 job does.

### Linux

Native x64/ARM64 builds need a C++17 compiler, CMake, and optionally
Fontconfig/Freetype for font rendering. The LoongArch64 job cross-compiles with
the Ubuntu `gcc-14-loongarch64-linux-gnu` packages and the toolchain file in
`cmake/toolchains/linux-loongarch64.cmake`. Fontconfig/Freetype are disabled
for that cross build; `@gfx` itself does not require them.

### macOS

Build one architecture at a time with
`-DCMAKE_OSX_ARCHITECTURES=x86_64` or `arm64`. The shared library links the
system Cocoa/Carbon/Foundation/Metal frameworks used by LICE.

## CI matrix

`.github/workflows/build.yml` produces:

| Artifact | Runner | Architecture | EEL2 path |
|---|---|---|---|
| `ysfx-windows-x64` | `windows-2022` | x64 | assembly |
| `ysfx-windows-arm64` | `windows-2022` | ARM64 | portable |
| `ysfx-linux-x64` | `ubuntu-24.04` | x86-64 | assembly |
| `ysfx-linux-arm64` | `ubuntu-24.04-arm` | AArch64 | portable |
| `ysfx-linux-loongarch64` | `ubuntu-24.04` cross | LoongArch64 | portable |
| `ysfx-macos-x64` | `macos-15-intel` | x86-64 | assembly |
| `ysfx-macos-arm64` | `macos-15` | ARM64 | portable |

Each artifact contains the library, `ysfx.h`, `LICENSE`, and `NOTICE.md`.

The matrix intentionally does not produce macOS/LoongArch64 or
Windows/LoongArch64 binaries. Neither macOS nor Windows has a supported
LoongArch64 OS/toolchain target, so those entries would be fictional rather
than buildable. The Linux LoongArch64 target is the extra platform above
OwnVST3's six RIDs.

## Smoke test

`YSFX_SMOKE_TEST=ON` builds a plain C program that loads a JSFX, compiles all
sections, processes 2-in/2-out audio, round-trips slider and `@serialize`
state, calls the offscreen `@gfx` API, and resolves an `import` from the file's
directory. It runs on native jobs only; cross jobs compile the library but do
not execute target code.

```bash
ctest --test-dir build --output-on-failure
```

## Source hygiene

The repository contains source and license texts only. Build trees, object
files, libraries, and compiler caches are ignored by `.gitignore` and are not
committed by the CI jobs.
