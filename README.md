# REA Harness + Forge (Android)

Two arm64 Android apps in one Gradle build:

- **REA Harness (`:app`, `com.rea.harness`)** — a clean-room, generic AArch64
  user-mode harness with a strict storage-access-framework boot gate.
- **REA Forge (`:forge`, `com.rea.forge`)** — an on-device analysis workbench
  for Hyrule Warriors: Definitive Edition: NSP/NSO/NPDM/RomFS validation,
  AArch64 static scanning, shader inventory, and Vulkan pipeline-cache creation
  for Snapdragon 8 Elite Gen 5 / Adreno 840.

> **Read [`SCOPE.md`](SCOPE.md) first.** Neither module emulates Horizon OS or
> NVN, recompiles the Switch title, or makes it playable. The title binary is
> Horizon/NVN code; the harness targets self-contained Linux AArch64 ELF images,
> and Forge stops after structural analysis. Launch is intentionally disabled.

## REA Forge (`:forge`)

Forge is the title-specific workbench. It requires a structurally valid `.nsp`
**or** an extracted `exefs` + `romfs` pair. Build stages need the extracted
pair; an NSP alone is validated as a container.

- PFS0/NSP validation, NCA header classification, and byte-for-byte payload
  copying. Retail NCA bodies remain encrypted and are **never decrypted**.
- NSO LZ4 decompression, section SHA-256 verification, NPDM META/ACID/ACI0
  parsing and SAC service extraction.
- RomFS SAF indexing, KPS/BFRES shader inventory, and AArch64 text scanning
  (SVC, TPIDRRO_EL0, branch/system registers). Nothing is recompiled or run.
- Real `VkPipelineCache` generation from the bundled FSR 1 / GSR / frame
  interpolation compute SPIR-V. Game shaders are not translated.
- Dark Compose dashboard with gate/build status, blockers, and graphics,
  performance, and input settings.

Verified against the retail `main` NSO and `main.npdm`; the APK builds, but it
has not been device-tested on Adreno 840 hardware. See
[`forge/README.md`](forge/README.md) for details, outputs, and limits.

```sh
./gradlew :forge:assembleDebug
# forge/build/outputs/apk/debug/forge-debug.apk
```

## Harness (`:app`) v0.1

### What v0.1 does

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

### What v0.1 does not do

- It does not execute the loaded image. The syscall-HLE bridge is not written.
- It does not create a Vulkan device, swapchain, or renderer. The seven SPIR-V
  modules are checked in, but there is no host-side renderer yet.
- It does not read title keys, decrypt NCAs, or parse Horizon IPC/NVN. Do not
  add those here. Forge's NSP action is a raw PFS0 byte copy, not decryption.

The load button therefore means **"validate and map"**, not "play the game".
`SCOPE.md` explains why a Linux user-mode loader cannot boot Horizon code.

## Build

Prerequisites:

- JDK 21 (build verified with `21.0.13+9`; newer default JDKs are rejected by
  this Gradle 8.13 setup).
- Android SDK 36 with build-tools 36.0.0.
- Android NDK r27+ (`27.2.12479018` is pinned).
- CMake 3.22.1 (install through the SDK manager).

```sh
./gradlew assembleDebug            # both modules
./gradlew :app:assembleDebug       # harness only
./gradlew :forge:assembleDebug     # forge only
```

The apks are written to:

```
app/build/outputs/apk/debug/app-debug.apk
forge/build/outputs/apk/debug/forge-debug.apk
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

forge/src/main/java/com/rea/forge/       Forge Compose UI, SAF helpers, worker
forge/src/main/cpp/format/               PFS0/NCA, NSO, NPDM, RomFS parsers
forge/src/main/cpp/recomp/               AArch64 static scanner
forge/src/main/cpp/gfx/                  Vulkan compute pipeline-cache builder
forge/src/main/assets/shaders/           bundled post-process SPIR-V
```

Host-side verification helpers live under `tools/` (for example,
`tools/verify_nso.py` parses and hash-checks the real NSO without Android).

## Legal

No Nintendo or Koei Tecmo code, keys, or assets are included. Do not commit game
dumps, extracted content, or key files; `.gitignore` excludes the usual names.
