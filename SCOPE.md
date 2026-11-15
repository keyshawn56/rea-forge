# Scope, correctness boundaries, and the binary in this folder

## Short version

This tree is being built as a **generic AArch64 user-mode harness**: a strict
boot gate, an AArch64 ELF loader/relocator, a syscall-HLE bridge, and an
independent Vulkan 1.3 demo renderer with FSR 1 / GSR / frame interpolation.

It is **not a Nintendo Switch emulator and not a recompilation of the title in
this folder**. The NSP and the extracted `exefs`/`romfs` here belong to a
Horizon OS application. That code does not issue Linux syscalls and does not
call a custom Vulkan API: it is statically linked against NintendoSDK/NVN and
reaches system services through Horizon IPC. Nothing in this tree implements
NVN, KTGL, G1M/G1T, Horizon services, or title-specific HLE. The loader will
parse a well-formed AArch64 ELF, but the title will not boot, render, or play.

This distinction is not a disclaimer bolted onto the design; it drives it. A
Linux-syscall HLE cannot substitute for Horizon IPC, and a standalone Vulkan
renderer cannot substitute for NVN command-stream translation. Those are
separate, multi-year bodies of work.

## Boot gate semantics

The picker requires a directory containing:

- `main.elf` — AArch64 ELF64, ET_EXEC or ET_DYN, self-contained (no PT_INTERP,
  no DT_NEEDED).
- `assets.pak` — the RPAK container documented in `pak_archive.hpp`.
- `manifest.sha256` (or `integrity.sha256` / `assets.sha256`) — lowercase
  SHA-256 digests in `sha256sum` format.

The gate rejects missing, empty, truncated, wrong-machine, wrong-magic, or
hash-mismatched inputs before the renderer or runtime is created. The manifest
is **user-supplied and is not signed**. This is a corruption/consistency gate,
not an authenticity boundary: an attacker who can replace both the payload and
the manifest can satisfy it. Treat it accordingly.

No title keys, ticket parsing, NCA/NSP extraction, or decryption is present, and
none should be added to this repository.

## What is implemented in v0.1

- `app/src/main/cpp/shaders/` and `app/src/main/assets/shaders/`:
  - FSR 1 EASU and RCAS compute shaders using AMD's MIT-licensed
    `ffx_fsr1.h` (vendored under `third_party/ffx`).
  - A clean-room GSR-style single-pass upscaler.
  - A two-mode optical-flow estimator/blender compute shader.
  - Scene and depth-only shaders.
  - All seven SPIR-V modules compile with `glslc` 1.4.363 at
    `--target-env=vulkan1.3`.
- `elf_loader.hpp/.cpp`: ELF64 header/program-header/section/symbol parsing,
  PT_LOAD mapping, BSS zeroing, AArch64 RELA + RELR relocations, HLE symbol
  resolver hook, build-id extraction, and page-granular W^X protection.
- `pak_archive.hpp/.cpp`: strict RPAK index validation, raw and LZ4 block
  decoding, CRC-32 verification.
- Kotlin/Compose application (`com.rea.harness`): SAF folder picker with
  persisted read access, strict gate (ELF header/machine/type/`PT_INTERP`,
  RPAK header/index bounds, manifest parsing, SHA-256 hashing), DataStore
  settings, native log stream, and engine statistics.
- JNI bridge (`native_bridge.cpp`, `engine.cpp`, `sha256.cpp`): descriptor
  ownership, a second native SHA-256 pass over both detached descriptors, ELF
  and pak parsing, and engine lifecycle. Native sources compile with NDK r27c.
- A debug apk built by `./gradlew assembleDebug` for `arm64-v8a`.

## What is not implemented yet

- The syscall-HLE execution bridge (SVC rewriting, guest stack/auxv, a focused
  Linux ABI table). The design and its limits are documented as the bridge is
  added; threading, signals, and guest TLS are explicitly out of scope for the
  first version. Because of this, the apk validates and maps an image but does
  not run it.
- The Vulkan renderer host side (swapchain, pipelines, descriptors, indirect
  draw, display timing). The shader contracts are fixed by the files above.

## Build prerequisites

- Android NDK r27+ (this project targets `arm64-v8a`, API 34+).
- Android SDK 36, JDK 17+, Gradle 8.13.
- Vulkan SDK with `glslc` only if you want to regenerate SPIR-V. The compiled
  `.spv` files are checked in so the APK build does not require `glslc`.

The native sources and the apk build are verified in the authoring environment.
The ELF loader has **not** been device-tested against a running image; do not
treat it as a compatibility guarantee.
