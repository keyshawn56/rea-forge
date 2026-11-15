package com.rea.forge

import android.content.Context
import android.net.Uri
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.emptyPreferences
import androidx.datastore.preferences.core.floatPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.map
import java.io.IOException

private val Context.forgeDataStore by preferencesDataStore(name = "forge_settings")

enum class FsrMode(val label: String) {
    OFF("Off"),
    EASU("EASU"),
    EASU_RCAS("EASU + RCAS"),
}

enum class InternalScale(val label: String) {
    HALF("0.5x"),
    THREE_QUARTER("0.75x"),
    NATIVE("1.0x"),
    ONE_AND_HALF("1.5x"),
    DOUBLE("2.0x"),
}

enum class UpscaleTarget(val label: String) {
    TARGET_720("720p"),
    TARGET_900("900p"),
    TARGET_1080("1080p"),
    TARGET_1440("1440p"),
    NATIVE("Native"),
}

enum class FrameGeneration(val label: String) {
    OFF("Off"),
    X2("2x"),
}

enum class TextureFiltering(val label: String) {
    BILINEAR("Bilinear"),
    TRILINEAR("Trilinear"),
    ANISO_4("4x AF"),
    ANISO_8("8x AF"),
    ANISO_16("16x AF"),
}

enum class ShadowQuality(val label: String) {
    LOW("Low"),
    MEDIUM("Medium"),
    HIGH("High"),
    ULTRA("Ultra"),
}

enum class FpsCap(val label: String) {
    FPS30("30"),
    FPS60("60"),
    FPS120("120"),
    UNCAPPED("Uncapped"),
}

enum class ContentKind {
    NSP,
    ROMFS,
    EXEFS,
}

data class ContentSelection(
    val nspUri: String? = null,
    val romfsUri: String? = null,
    val exefsUri: String? = null,
)

data class ForgeSettings(
    val fsrMode: FsrMode = FsrMode.EASU_RCAS,
    val internalScale: InternalScale = InternalScale.NATIVE,
    val upscaleTarget: UpscaleTarget = UpscaleTarget.TARGET_1080,
    val gsr: Boolean = true,
    val frameGeneration: FrameGeneration = FrameGeneration.OFF,
    val hudMask: Boolean = true,
    val flowSensitivity: Float = 0.5f,
    val textureFiltering: TextureFiltering = TextureFiltering.ANISO_8,
    val shadows: ShadowQuality = ShadowQuality.HIGH,
    val bloom: Boolean = true,
    val motionBlur: Boolean = false,
    val vsync: Boolean = true,
    val fpsCap: FpsCap = FpsCap.FPS60,
    val precompileShaders: Boolean = true,
    val persistentPipelineCache: Boolean = true,
    val lowLatency: Boolean = true,
    val audioVolume: Float = 0.85f,
    val touchOpacity: Float = 0.55f,
    val haptics: Boolean = true,
    val gamepad: Boolean = true,
)

class SettingsRepository(private val context: Context) {
    private object Keys {
        val nspUri = stringPreferencesKey("content_nsp_uri")
        val romfsUri = stringPreferencesKey("content_romfs_uri")
        val exefsUri = stringPreferencesKey("content_exefs_uri")

        val fsrMode = stringPreferencesKey("graphics_fsr_mode")
        val internalScale = stringPreferencesKey("graphics_internal_scale")
        val upscaleTarget = stringPreferencesKey("graphics_upscale_target")
        val gsr = booleanPreferencesKey("graphics_gsr")
        val frameGeneration = stringPreferencesKey("graphics_frame_generation")
        val hudMask = booleanPreferencesKey("graphics_hud_mask")
        val flowSensitivity = floatPreferencesKey("graphics_flow_sensitivity")
        val textureFiltering = stringPreferencesKey("graphics_texture_filtering")
        val shadows = stringPreferencesKey("graphics_shadows")
        val bloom = booleanPreferencesKey("graphics_bloom")
        val motionBlur = booleanPreferencesKey("graphics_motion_blur")
        val vsync = booleanPreferencesKey("graphics_vsync")
        val fpsCap = stringPreferencesKey("graphics_fps_cap")
        val precompileShaders = booleanPreferencesKey("perf_precompile_shaders")
        val persistentPipelineCache = booleanPreferencesKey("perf_persistent_pipeline_cache")
        val lowLatency = booleanPreferencesKey("perf_low_latency")
        val audioVolume = floatPreferencesKey("input_audio_volume")
        val touchOpacity = floatPreferencesKey("input_touch_opacity")
        val haptics = booleanPreferencesKey("input_haptics")
        val gamepad = booleanPreferencesKey("input_gamepad")
    }

    private val preferences: Flow<Preferences> = context.forgeDataStore.data.catch { error ->
        if (error is IOException) emit(emptyPreferences()) else throw error
    }

    val settings: Flow<ForgeSettings> = preferences.map { prefs ->
        ForgeSettings(
            fsrMode = prefs.enumValue(Keys.fsrMode, FsrMode.EASU_RCAS),
            internalScale = prefs.enumValue(Keys.internalScale, InternalScale.NATIVE),
            upscaleTarget = prefs.enumValue(Keys.upscaleTarget, UpscaleTarget.TARGET_1080),
            gsr = prefs[Keys.gsr] ?: true,
            frameGeneration = prefs.enumValue(Keys.frameGeneration, FrameGeneration.OFF),
            hudMask = prefs[Keys.hudMask] ?: true,
            flowSensitivity = (prefs[Keys.flowSensitivity] ?: 0.5f).coerceIn(0f, 1f),
            textureFiltering = prefs.enumValue(Keys.textureFiltering, TextureFiltering.ANISO_8),
            shadows = prefs.enumValue(Keys.shadows, ShadowQuality.HIGH),
            bloom = prefs[Keys.bloom] ?: true,
            motionBlur = prefs[Keys.motionBlur] ?: false,
            vsync = prefs[Keys.vsync] ?: true,
            fpsCap = prefs.enumValue(Keys.fpsCap, FpsCap.FPS60),
            precompileShaders = prefs[Keys.precompileShaders] ?: true,
            persistentPipelineCache = prefs[Keys.persistentPipelineCache] ?: true,
            lowLatency = prefs[Keys.lowLatency] ?: true,
            audioVolume = (prefs[Keys.audioVolume] ?: 0.85f).coerceIn(0f, 1f),
            touchOpacity = (prefs[Keys.touchOpacity] ?: 0.55f).coerceIn(0.15f, 1f),
            haptics = prefs[Keys.haptics] ?: true,
            gamepad = prefs[Keys.gamepad] ?: true,
        )
    }

    val selection: Flow<ContentSelection> = preferences.map { prefs ->
        ContentSelection(
            nspUri = prefs[Keys.nspUri],
            romfsUri = prefs[Keys.romfsUri],
            exefsUri = prefs[Keys.exefsUri],
        )
    }

    suspend fun setContentUri(kind: ContentKind, uri: Uri?) {
        context.forgeDataStore.edit { prefs ->
            val key = when (kind) {
                ContentKind.NSP -> Keys.nspUri
                ContentKind.ROMFS -> Keys.romfsUri
                ContentKind.EXEFS -> Keys.exefsUri
            }
            if (uri == null) prefs.remove(key) else prefs[key] = uri.toString()
        }
    }

    suspend fun setFsrMode(value: FsrMode) = putEnum(Keys.fsrMode, value)
    suspend fun setInternalScale(value: InternalScale) = putEnum(Keys.internalScale, value)
    suspend fun setUpscaleTarget(value: UpscaleTarget) = putEnum(Keys.upscaleTarget, value)
    suspend fun setGsr(value: Boolean) = put(Keys.gsr, value)
    suspend fun setFrameGeneration(value: FrameGeneration) = putEnum(Keys.frameGeneration, value)
    suspend fun setHudMask(value: Boolean) = put(Keys.hudMask, value)
    suspend fun setFlowSensitivity(value: Float) = put(Keys.flowSensitivity, value.coerceIn(0f, 1f))
    suspend fun setTextureFiltering(value: TextureFiltering) = putEnum(Keys.textureFiltering, value)
    suspend fun setShadows(value: ShadowQuality) = putEnum(Keys.shadows, value)
    suspend fun setBloom(value: Boolean) = put(Keys.bloom, value)
    suspend fun setMotionBlur(value: Boolean) = put(Keys.motionBlur, value)
    suspend fun setVsync(value: Boolean) = put(Keys.vsync, value)
    suspend fun setFpsCap(value: FpsCap) = putEnum(Keys.fpsCap, value)
    suspend fun setPrecompileShaders(value: Boolean) = put(Keys.precompileShaders, value)
    suspend fun setPersistentPipelineCache(value: Boolean) = put(Keys.persistentPipelineCache, value)
    suspend fun setLowLatency(value: Boolean) = put(Keys.lowLatency, value)
    suspend fun setAudioVolume(value: Float) = put(Keys.audioVolume, value.coerceIn(0f, 1f))
    suspend fun setTouchOpacity(value: Float) = put(Keys.touchOpacity, value.coerceIn(0.15f, 1f))
    suspend fun setHaptics(value: Boolean) = put(Keys.haptics, value)
    suspend fun setGamepad(value: Boolean) = put(Keys.gamepad, value)

    private suspend fun put(key: Preferences.Key<Boolean>, value: Boolean) {
        context.forgeDataStore.edit { it[key] = value }
    }

    private suspend fun put(key: Preferences.Key<Float>, value: Float) {
        context.forgeDataStore.edit { it[key] = value }
    }

    private suspend inline fun <reified T : Enum<T>> putEnum(key: Preferences.Key<String>, value: T) {
        context.forgeDataStore.edit { it[key] = value.name }
    }
}

private inline fun <reified T : Enum<T>> Preferences.enumValue(
    key: Preferences.Key<String>,
    default: T,
): T {
    val stored = this[key] ?: return default
    return enumValues<T>().firstOrNull { it.name == stored } ?: default
}
