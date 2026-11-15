package com.rea.harness

import android.content.Context
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map

private val Context.reaDataStore by preferencesDataStore(name = "rea_settings")

enum class Resolution(val label: String, val width: Int, val height: Int) {
    P720("720p", 1280, 720),
    P900("900p", 1600, 900),
    P1080("1080p", 1920, 1080),
    P1440("1440p", 2560, 1440)
}

enum class ShadowQuality(val label: String, val size: Int) {
    LOW("Low 1K", 1024),
    MEDIUM("Medium 2K", 2048),
    HIGH("High 4K PCF", 4096)
}

enum class Anisotropy(val label: String, val level: Int) {
    X1("1x", 1),
    X4("4x", 4),
    X8("8x", 8),
    X16("16x", 16)
}

enum class UpscalerMode(val label: String, val id: Int) {
    OFF("Off", 0),
    FSR1("FSR 1", 1),
    GSR("GSR", 2)
}

enum class FrameGenMode(val label: String, val id: Int, val targetHz: Int) {
    OFF("Off", 0, 0),
    FPS90("90 Hz", 1, 90),
    FPS120("120 Hz", 2, 120)
}

data class GraphicsSettings(
    val resolution: Resolution = Resolution.P1080,
    val shadow: ShadowQuality = ShadowQuality.MEDIUM,
    val anisotropy: Anisotropy = Anisotropy.X8,
    val crowdPercent: Int = 100,
    val upscaler: UpscalerMode = UpscalerMode.FSR1,
    val frameGen: FrameGenMode = FrameGenMode.OFF,
    val lowLatency: Boolean = false
)

class SettingsStore(private val context: Context) {
    private object Keys {
        val resolution = stringPreferencesKey("resolution")
        val shadow = stringPreferencesKey("shadow")
        val anisotropy = stringPreferencesKey("anisotropy")
        val crowd = intPreferencesKey("crowd_percent")
        val upscaler = stringPreferencesKey("upscaler")
        val frameGen = stringPreferencesKey("frame_gen")
        val lowLatency = booleanPreferencesKey("low_latency")
        val treeUri = stringPreferencesKey("tree_uri")
    }

    val settings: Flow<GraphicsSettings> = context.reaDataStore.data.map { prefs ->
        GraphicsSettings(
            resolution = enumOr(prefs[Keys.resolution], Resolution.P1080),
            shadow = enumOr(prefs[Keys.shadow], ShadowQuality.MEDIUM),
            anisotropy = enumOr(prefs[Keys.anisotropy], Anisotropy.X8),
            crowdPercent = (prefs[Keys.crowd] ?: 100).coerceIn(25, 100),
            upscaler = enumOr(prefs[Keys.upscaler], UpscalerMode.FSR1),
            frameGen = enumOr(prefs[Keys.frameGen], FrameGenMode.OFF),
            lowLatency = prefs[Keys.lowLatency] ?: false
        )
    }

    val treeUri: Flow<String?> = context.reaDataStore.data.map { it[Keys.treeUri] }

    suspend fun update(settings: GraphicsSettings) {
        context.reaDataStore.edit { prefs ->
            prefs[Keys.resolution] = settings.resolution.name
            prefs[Keys.shadow] = settings.shadow.name
            prefs[Keys.anisotropy] = settings.anisotropy.name
            prefs[Keys.crowd] = settings.crowdPercent.coerceIn(25, 100)
            prefs[Keys.upscaler] = settings.upscaler.name
            prefs[Keys.frameGen] = settings.frameGen.name
            prefs[Keys.lowLatency] = settings.lowLatency
        }
    }

    suspend fun setTreeUri(uri: String?) {
        context.reaDataStore.edit { prefs ->
            if (uri == null) prefs.remove(Keys.treeUri) else prefs[Keys.treeUri] = uri
        }
    }

    private inline fun <reified T : Enum<T>> enumOr(value: String?, fallback: T): T {
        if (value == null) return fallback
        return runCatching { enumValueOf<T>(value) }.getOrDefault(fallback)
    }
}
