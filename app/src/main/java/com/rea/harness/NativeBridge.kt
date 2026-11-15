package com.rea.harness

object NativeBridge {
    init {
        System.loadLibrary("rea_harness")
    }

    /**
     * Parses and validates the two descriptors. Ownership of both descriptors
     * transfers to native code; it closes them on success and on failure.
     */
    external fun boot(
        elfFd: Int,
        pakFd: Int,
        sandboxDir: String,
        elfSha256: String,
        pakSha256: String
    ): Boolean

    external fun configure(
        width: Int,
        height: Int,
        shadowSize: Int,
        anisotropy: Int,
        crowdPercent: Int,
        upscaler: Int,
        frameGen: Int,
        lowLatency: Boolean
    )

    external fun stats(): String
    external fun shutdown()

    @JvmStatic
    fun onNativeLog(level: Int, tag: String, message: String) {
        LogBus.append(level, tag, message)
    }
}
