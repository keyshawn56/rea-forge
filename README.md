# REA Harness (Android)

A clean-room, generic AArch64 user-mode harness for Android arm64 devices,
packaged as an apk with a strict storage-access-framework boot gate.

> **Read [`SCOPE.md`](SCOPE.md) first.** This project does not emulate Horizon
> OS or NVN, and it does not recompile or run the Switch title in this folder.
> The title binary is Horizon/NVN code; the harness here targets self-contained
> Linux AArch64 ELF images.

## What v0.1 does

- Presents a Compose settings and diagnostic UI (`com.rea.harness`).
- Requires a SAF folder containing `main.elf`, `assets.pak`, and
  `manifest.sha256` (`integrity.sha256` / `assets.sha256` are also accepted).
- Rejects missing, empty, truncated, non-AArch64, non-ELF64, `PT_INTERP`, bad
  RPAK-index, and digest-mismatched inputs before the native engine is created.
- Re-hashes both detached descriptors in native code before mapping, so a file
  swapped between the Kotlin gate and the engine does not pass.
- Parses the ELF, maps `PT_LOAD` segments into anonymous memory, applies
  AArch64 `RELA`/`RELR` relocations, and parses the RPAK index.
- Persists resolution, shadow quality, anisotropy, crowd density, upscaler,
  frame-generation target, and low-latency mode in DataStore.
- Shows native log output and engine statistics.

## What v0.1 does not do

- It does not execute the loaded image. The syscall-HLE bridge is not written.
- It does not create a Vulkan device, swapchain, or renderer. The seven SPIR-V
  modules are checked in, but there is no host-side renderer yet.
- It does not read title keys, decrypt NCAs, or parse Horizon IPC/NVN. Do not
  add those here.

The load button therefore means **"validate and map"**, not "play the game".
`SCOPE.md` explains why a Linux user-mode loader cannot boot Horizon code.

## Build

Prerequisites:

- JDK 17+ (JDK 21 is used for the build).
- Android SDK 36 with build-tools 36.0.0.
- Android NDK r27+ (`27.2.12479018` is pinned).
- CMake 3.22.1 (install through the SDK manager).

```sh
./gradlew assembleDebug
```

The apk is written to:

```
app/build/outputs/apk/debug/app-debug.apk
```

Only `arm64-v8a` is built. `minSdk` is 34 (Android 14) and `targetSdk` is 36.
The precompiled SPIR-V files are checked in, so `glslc` is not needed for an apk
build.

## Input layout

```
<folder chosen in the picker>/
  main.elf          statically linked AArch64 ELF64, ET_EXEC or ET_DYN
  assets.pak        RPAK container (see app/src/main/cpp/pak_archive.hpp)
  manifest.sha256   sha256sum-format digests for main.elf and assets.pak
```

`manifest.sha256` is user-supplied and unsigned. The gate detects corruption
and accidental mismatch; it cannot establish authenticity.

## Layout

```
app/src/main/java/com/rea/harness/       Compose UI, SAF gate, DataStore
app/src/main/cpp/                        ELF loader, RPAK reader, SHA-256, JNI
app/src/main/cpp/shaders/                GLSL compute/graphics sources
app/src/main/cpp/third_party/ffx/        AMD FSR 1 headers (MIT)
app/src/main/assets/shaders/             precompiled SPIR-V
```

## Legal

No Nintendo or Koei Tecmo code, keys, or assets are included. Do not commit game
dumps, extracted content, or key files; `.gitignore` excludes the usual names.
