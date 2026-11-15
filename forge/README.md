# REA Forge

`com.rea.forge` is an Android analysis workbench for **Hyrule Warriors:
Definitive Edition** (`0100AE00096EA000`) on Snapdragon 8 Elite Gen 5 /
Adreno 840 hardware. It validates local game containers, decompresses and
scans the executable, indexes RomFS, inventories shader packages, and builds a
real Vulkan `VkPipelineCache` from bundled post-process SPIR-V.

It is **not** an emulator, a Horizon OS runtime, or a recompiler. Analysis is
real; launching the title is intentionally disabled. See
[`../SCOPE.md`](../SCOPE.md) for the hardware/runtime boundary.

## What the gate accepts

Select one of these through the storage picker:

| Selection | Result |
| --- | --- |
| A `.nsp` file | PFS0 + plaintext NCA headers are validated. Build stages stay locked. |
| An extracted `exefs` folder | `main` (NSO), `main.npdm`, and sibling modules are validated. |
| An extracted `romfs` folder | The tree is indexed through SAF. |
| `exefs` + `romfs` | The gate opens **and** build stages are enabled. |

The gate is a structural/content gate. It does not prove ownership, and it does
not validate Nintendo signatures.

## Implemented

### NSP / PFS0

- Parses the PFS0 header, entry table, and string table with bounds checks.
- Classifies `.nca`, `.tik`, `.cert`, and other entries.
- Reads the first `0x300` bytes of each NCA and reports magic, content type,
  content size, and program ID when the NCA header is plaintext.
- Distinguishes plaintext from retail-encrypted NCA headers; an encrypted header
  is reported as encrypted, not guessed at.
- Raw copy: every PFS0 payload is copied **byte for byte**. Encrypted NCA bodies
  remain encrypted. No title key, ticket key, or NCA decryption is used.

### NSO

- Reads the `NSO0` header, module name, flags, BSS size, and all three segments.
- LZ4-decompresses `text`, `rodata`, and `data`.
- Verifies the stored SHA-256 for each section when the header marks the hash as
  present.
- Reports per-section file/memory sizes and decoded digests.

Verified against the retail `main` NSO:

| Field | Value |
| --- | --- |
| File size | `6,488,514` bytes |
| Flags | `0x3F` (all three sections LZ4-compressed, all hashes present) |
| text | `0x601A88` memory / `0x3D9798` file bytes |
| rodata | `0x58A022` memory / `0x22678A` file bytes |
| data | `0xA1050` memory / `0x3019F` file bytes |
| BSS | `810,928` bytes |
| text SHA-256 | `75c43364…ee2d` |
| Text instructions | `1,574,562` |

### NPDM

- Parses the `META` block, `ACID` signature block at `+0x200`, and `ACI0`.
- Reports production flag, 64-bit/address-space flags, stack size, system
  resource size, program-ID range, and the ACI0 service-access table.
- `valid` requires both `ACID` and `ACI0`; a META-only file is not accepted.
- Verified against `main.npdm`: flags `0x7`, ACID at `0x80`, ACI0 at `0x430`,
  and 42 services parsed from the SAC table.

### AArch64 static analysis

- Decompresses the text segment and scans every 4-byte instruction.
- Counts `SVC`, `TPIDRRO_EL0` reads/writes, `BR`/`BLR`, and system-register
  accesses.
- Emits the text SHA-256 and instruction count.
- The retail title currently reports `0` SVC sites and `0` TPIDRRO_EL0 accesses
  in `main`; the report still marks the runtime as blocked because Horizon IPC
  and NVN cannot be reconstructed from a syscall count.
- **No instructions are rewritten, recompiled, or executed.**

### RomFS

- Walks the selected SAF tree without copying file payloads, with depth and file
  limits plus a truncation flag.
- Counts directories, files, bytes, extensions, KPS shader packages, BFRES
  files, and top-level entries.
- A native parser for a raw extracted `romfs.bin` also exists
  (`nativeInspectRomFsImage`), including directory-entry metadata.

### Shader inventory

- Counts `.kps`, `.bfres`, and `.spv` files and samples their first bytes.
- Reports KPS `_SPK4100` packages as Maxwell-era containers. Translation from
  Maxwell bytecode to SPIR-V is **not implemented**, so no game shader is
  emitted.

### Adreno pipeline cache

- Validates the seven bundled `.spv` modules, then stages only valid modules.
- Reflects SPIR-V entry-point model, descriptor set/binding usage, and push
  constants.
- Creates a Vulkan 1.3 instance (the bundled modules are SPIR-V 1.6), compiles
  **compute** pipelines through the device's Vulkan driver, and serializes
  `VkPipelineCache` to `files/gpu/pipeline_cache.bin`.
- The four compute assets (`fsr_easu`, `fsr_rcas`, `gsr_upscale`,
  `frame_interp`) are compiled; the three graphics assets (`scene_vert`,
  `scene_frag`, `shadow_frag`) are skipped and counted explicitly.
- The cache contains **no game shaders**. It is a warm-up cache for the bundled
  post-process stages, not evidence that title shaders were translated.

### Compose dashboard

- Gate cards for NSP, NSO/NPDM, and RomFS with per-field diagnostics.
- Ownership/build status surface: gate state, build readiness, report stages,
  blockers, warnings, and device/native facts.
- Graphics settings: FSR mode, internal scale, output target, GSR toggle, frame
  generation, HUD mask, flow sensitivity, texture filtering, shadows, bloom,
  motion blur, VSync, and FPS cap.
- Performance/input settings: shader precompile, persistent pipeline cache, low
  latency, audio volume, touch opacity, haptics, and gamepad.
- Foreground WorkManager build with progress stages, cancellation, and JSON
  reports at `files/reports/latest.json` plus timestamped copies.

## Build

Requirements:

- JDK 21 (the project is verified with `jdk-21.0.13+9`; Gradle 8.13 rejects the
  newer default JDK 27).
- Android SDK 36, build-tools 36.0.0.
- Android NDK `27.2.12479018` (r27c).
- CMake 3.22.1.

```sh
export JAVA_HOME=/path/to/jdk-21
./gradlew :forge:assembleDebug
```

The APK is written to:

```
forge/build/outputs/apk/debug/forge-debug.apk
```

Only `arm64-v8a` is built. `minSdk` is 34, `targetSdk` is 36. Bundled SPIR-V is
checked in under `src/main/assets/shaders/`; `glslc` is not needed for an APK
build. The APK is compile-verified, but this repository has **not** been run on
a physical Galaxy S26 / Adreno 840 device.

## Install and use

```sh
adb install -r forge/build/outputs/apk/debug/forge-debug.apk
```

1. Grant notification permission so the foreground build can report progress.
2. Pick the `.nsp`, the extracted `exefs` folder, and/or the extracted `romfs`
   folder. Selecting the parent that contains `exefs/` and `romfs/` is fine.
3. Wait for validation. The build button requires a valid `exefs` + `romfs`
   pair.
4. Run **Build**. Review the stage report, warnings, and the generated pipeline
   cache. Launch remains disabled.

## Reports and output

```
files/gpu/pipeline_cache.bin     serialized Vulkan pipeline cache
files/reports/latest.json        most recent build report
files/reports/build-<time>.json  timestamped report
cache/spirv/*.spv                staged copies of bundled assets
```

Raw NSP copies go to the app-specific external directory
`nsp_extract/` and never leave the device through this app. That action copies
PFS0 payloads verbatim; it does not decrypt or install content.

## Layout

```
src/main/java/com/rea/forge/        Compose UI, SAF helpers, DataStore, worker
src/main/cpp/format/                PFS0/NCA, NSO, NPDM, RomFS parsers
src/main/cpp/recomp/                AArch64 static scanner
src/main/cpp/shader/                SPIR-V validation and inventory
src/main/cpp/gfx/                   Vulkan compute pipeline-cache builder
src/main/cpp/shaders/               GLSL sources for bundled assets
src/main/cpp/third_party/ffx/       AMD FSR 1 headers (MIT)
src/main/assets/shaders/            precompiled SPIR-V
src/main/cpp/forge_jni.cpp          JNI surface
```

## Explicit limits

- No Horizon kernel, IPC services, NVN command translation, KTGL, G1M/G1T, or
  title-specific HLE. The game cannot boot from this app.
- No title keys, ticket decryption, NCA content decryption, or signature bypass.
- No Maxwell-to-SPIR-V shader translator. KPS/BFRES payloads are counted, not
  decoded into game pipelines.
- No A64-to-ARM64 recompilation or executable image emission.
- Protection-domain and filesystem semantics of Horizon are not modeled.
- The pipeline cache is built from bundled post-process shaders only.

## Legal

No Nintendo or Koei Tecmo code, keys, tickets, or game assets are included.
`.gitignore` excludes NSP/NCA/NSO dumps, extracted content, keys, and build
output. Do not commit game data.
