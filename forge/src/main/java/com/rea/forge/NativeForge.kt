package com.rea.forge

/**
 * JNI surface. Every method returns a JSON object and never throws across the
 * boundary; a failure is `{"valid":false,"error":"..."}`.
 *
 * File descriptors are borrowed for the duration of the call. The native layer
 * duplicates them, so Kotlin keeps ownership and must close its ParcelFileDescriptor.
 */
object NativeForge {
    val isAvailable: Boolean = try {
        System.loadLibrary("rea_forge")
        true
    } catch (_: UnsatisfiedLinkError) {
        false
    }

    external fun nativeBuildInfo(): String
    external fun nativeValidateNsp(fd: Int, displayName: String): String
    external fun nativeExtractNsp(fd: Int, outputDirectory: String): String
    external fun nativeInspectNso(fd: Int, displayName: String): String
    external fun nativeScanA64(fd: Int, displayName: String): String
    external fun nativeInspectNpdm(fd: Int, displayName: String): String
    external fun nativeInspectRomFsImage(fd: Int): String
    external fun nativeIndexRomFs(path: String): String
    external fun nativeScanShaders(path: String): String
    external fun nativeValidateSpirv(path: String): String
    external fun nativeBuildPipelineCache(paths: Array<String>, outputPath: String): String
}
