package com.rea.forge

import android.app.Application
import android.net.Uri
import android.os.Build
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.Observer
import androidx.lifecycle.viewModelScope
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.WorkInfo
import androidx.work.WorkManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

data class GateState(
    val nsp: NspInfo? = null,
    val exefs: ExefsInfo? = null,
    val romfs: RomFsInfo? = null,
    val nspValid: Boolean = false,
    val exefsValid: Boolean = false,
    val romfsValid: Boolean = false,
    val gateOpen: Boolean = false,
    val buildReady: Boolean = false,
    val checking: Boolean = false,
    val error: String? = null,
)

data class BuildUiState(
    val running: Boolean = false,
    val progress: Int = 0,
    val stage: String = "",
    val report: ForgeBuildReport? = null,
    val error: String? = null,
    val hasRun: Boolean = false,
)

data class ExtractUiState(
    val running: Boolean = false,
    val message: String? = null,
    val error: String? = null,
)

class ForgeViewModel(application: Application) : AndroidViewModel(application) {
    private val settingsRepository = SettingsRepository(application)
    private val repository = ForgeRepository(application)
    private val workManager = WorkManager.getInstance(application)

    val settings: StateFlow<ForgeSettings> = settingsRepository.settings.stateIn(
        viewModelScope,
        SharingStarted.WhileSubscribed(5_000),
        ForgeSettings(),
    )

    val selection: StateFlow<ContentSelection> = settingsRepository.selection.stateIn(
        viewModelScope,
        SharingStarted.Eagerly,
        ContentSelection(),
    )

    private val _gate = MutableStateFlow(GateState())
    val gate: StateFlow<GateState> = _gate.asStateFlow()

    private val _build = MutableStateFlow(BuildUiState())
    val build: StateFlow<BuildUiState> = _build.asStateFlow()

    private val _extract = MutableStateFlow(ExtractUiState())
    val extract: StateFlow<ExtractUiState> = _extract.asStateFlow()

    private val _deviceInfo = MutableStateFlow(
        mapOf(
            "model" to Build.MODEL,
            "android" to Build.VERSION.RELEASE,
            "abi" to Build.SUPPORTED_ABIS.firstOrNull().orEmpty(),
            "native" to if (NativeForge.isAvailable) "loaded" else "unavailable",
        )
    )
    val deviceInfo: StateFlow<Map<String, String>> = _deviceInfo.asStateFlow()

    private val workInfos =
        workManager.getWorkInfosForUniqueWorkLiveData(ForgeBuildWorker.UNIQUE_WORK)

    private val workObserver = Observer<List<WorkInfo>> { infos ->
        val info = infos.firstOrNull() ?: return@Observer
        when (info.state) {
            WorkInfo.State.ENQUEUED, WorkInfo.State.RUNNING, WorkInfo.State.BLOCKED -> {
                _build.update {
                    it.copy(
                        running = true,
                        hasRun = true,
                        progress = info.progress.getInt("progress", 0),
                        stage = info.progress.getString("stage") ?: "Queued",
                        error = null,
                    )
                }
            }
            WorkInfo.State.SUCCEEDED -> {
                val path = info.outputData.getString("reportPath").orEmpty()
                viewModelScope.launch {
                    _build.update {
                        it.copy(
                            running = false,
                            progress = 100,
                            stage = "Report ready",
                            report = loadReport(path) ?: it.report,
                            hasRun = true,
                            error = null,
                        )
                    }
                }
            }
            WorkInfo.State.FAILED -> {
                val path = info.outputData.getString("reportPath").orEmpty()
                viewModelScope.launch {
                    _build.update {
                        it.copy(
                            running = false,
                            hasRun = true,
                            error = info.outputData.getString("error")
                                ?: "The build worker failed.",
                            report = loadReport(path) ?: it.report,
                        )
                    }
                }
            }
            WorkInfo.State.CANCELLED -> {
                _build.update {
                    it.copy(running = false, stage = "Cancelled", hasRun = true)
                }
            }
            else -> Unit
        }
    }

    init {
        viewModelScope.launch {
            settingsRepository.selection
                .distinctUntilChanged()
                .collectLatest { selected -> validateSelection(selected) }
        }
        workInfos.observeForever(workObserver)
        viewModelScope.launch(Dispatchers.IO) {
            if (NativeForge.isAvailable) {
                val info = parseBuildInfo(NativeForge.nativeBuildInfo())
                _deviceInfo.update { it + info }
            }
            val latest = File(getApplication<Application>().filesDir, "reports/latest.json")
            if (latest.isFile) {
                ForgeBuildReport.fromJson(latest.readText())?.let { report ->
                    _build.update { it.copy(report = report, hasRun = true) }
                }
            }
        }
    }

    override fun onCleared() {
        workInfos.removeObserver(workObserver)
        super.onCleared()
    }

    private suspend fun validateSelection(selected: ContentSelection) {
        _gate.update { it.copy(checking = true, error = null) }
        val nspUri = selected.nspUri?.let(Uri::parse)
        val exefsUri = selected.exefsUri?.let(Uri::parse)
        val romfsUri = selected.romfsUri?.let(Uri::parse)

        val nsp = nspUri?.let { repository.inspectNsp(it) }
        val exefs = exefsUri?.let { repository.inspectExefs(it) }
        val romfs = romfsUri?.let { repository.inspectRomfs(it) }
        val nspValid = nsp?.valid == true
        val exefsValid = exefs?.valid == true
        val romfsValid = romfs?.valid == true
        _gate.value = GateState(
            nsp = nsp,
            exefs = exefs,
            romfs = romfs,
            nspValid = nspValid,
            exefsValid = exefsValid,
            romfsValid = romfsValid,
            gateOpen = nspValid || (exefsValid && romfsValid),
            buildReady = exefsValid && romfsValid,
            checking = false,
            error = when {
                nspUri == null && exefsUri == null && romfsUri == null -> null
                nspUri != null && !nspValid -> nsp?.error ?: "NSP validation failed"
                exefsUri != null && !exefsValid -> exefs?.error ?: "exefs validation failed"
                romfsUri != null && !romfsValid -> romfs?.error ?: "RomFS validation failed"
                else -> null
            },
        )
    }

    private suspend fun loadReport(path: String): ForgeBuildReport? = withContext(Dispatchers.IO) {
        val file = when {
            path.isNotEmpty() -> File(path)
            else -> File(getApplication<Application>().filesDir, "reports/latest.json")
        }
        if (!file.isFile) return@withContext null
        ForgeBuildReport.fromJson(file.readText())
    }

    fun selectContent(kind: ContentKind, uri: Uri?) {
        viewModelScope.launch {
            settingsRepository.setContentUri(kind, uri)
        }
    }

    fun startBuild() {
        val gate = _gate.value
        if (!gate.buildReady) {
            _build.update {
                it.copy(
                    error = "Build stages need both extracted exefs and romfs. An NSP alone is validated as a container only.",
                    hasRun = true,
                )
            }
            return
        }
        val request = OneTimeWorkRequestBuilder<ForgeBuildWorker>().build()
        workManager.enqueueUniqueWork(
            ForgeBuildWorker.UNIQUE_WORK,
            androidx.work.ExistingWorkPolicy.REPLACE,
            request,
        )
        _build.update {
            it.copy(running = true, progress = 0, stage = "Queued", error = null, hasRun = true)
        }
    }

    fun cancelBuild() {
        workManager.cancelUniqueWork(ForgeBuildWorker.UNIQUE_WORK)
    }

    fun dismissBuildError() {
        _build.update { it.copy(error = null) }
    }

    fun extractNsp() {
        val selected = selection.value.nspUri?.let(Uri::parse)
        if (selected == null) {
            _extract.value = ExtractUiState(error = "Select an NSP first.")
            return
        }
        val nsp = _gate.value.nsp
        val expected = nsp?.entries?.sumOf { it.size } ?: 0L
        val base = getApplication<Application>().getExternalFilesDir(null)
            ?: getApplication<Application>().filesDir
        val output = File(base, "nsp_extract")
        if (expected > 0 && output.usableSpace < expected + (64L shl 20)) {
            _extract.value = ExtractUiState(
                error = "Not enough free space for a raw container copy. Need about ${formatBytes(expected)}."
            )
            return
        }
        _extract.value = ExtractUiState(running = true)
        viewModelScope.launch {
            val result = repository.extractNsp(selected, output)
            _extract.value = ExtractUiState(
                running = false,
                message = if (result.valid) {
                    "Raw copy finished: ${formatBytes(result.bytes)} in ${output.absolutePath}. ${result.warning}"
                } else {
                    null
                },
                error = result.error,
            )
        }
    }

    fun dismissExtract() {
        _extract.value = ExtractUiState()
    }

    fun setFsrMode(value: FsrMode) = viewModelScope.launch { settingsRepository.setFsrMode(value) }
    fun setInternalScale(value: InternalScale) =
        viewModelScope.launch { settingsRepository.setInternalScale(value) }
    fun setUpscaleTarget(value: UpscaleTarget) =
        viewModelScope.launch { settingsRepository.setUpscaleTarget(value) }
    fun setGsr(value: Boolean) = viewModelScope.launch { settingsRepository.setGsr(value) }
    fun setFrameGeneration(value: FrameGeneration) =
        viewModelScope.launch { settingsRepository.setFrameGeneration(value) }
    fun setHudMask(value: Boolean) = viewModelScope.launch { settingsRepository.setHudMask(value) }
    fun setFlowSensitivity(value: Float) =
        viewModelScope.launch { settingsRepository.setFlowSensitivity(value) }
    fun setTextureFiltering(value: TextureFiltering) =
        viewModelScope.launch { settingsRepository.setTextureFiltering(value) }
    fun setShadows(value: ShadowQuality) =
        viewModelScope.launch { settingsRepository.setShadows(value) }
    fun setBloom(value: Boolean) = viewModelScope.launch { settingsRepository.setBloom(value) }
    fun setMotionBlur(value: Boolean) =
        viewModelScope.launch { settingsRepository.setMotionBlur(value) }
    fun setVsync(value: Boolean) = viewModelScope.launch { settingsRepository.setVsync(value) }
    fun setFpsCap(value: FpsCap) = viewModelScope.launch { settingsRepository.setFpsCap(value) }
    fun setPrecompileShaders(value: Boolean) =
        viewModelScope.launch { settingsRepository.setPrecompileShaders(value) }
    fun setPersistentPipelineCache(value: Boolean) =
        viewModelScope.launch { settingsRepository.setPersistentPipelineCache(value) }
    fun setLowLatency(value: Boolean) =
        viewModelScope.launch { settingsRepository.setLowLatency(value) }
    fun setAudioVolume(value: Float) =
        viewModelScope.launch { settingsRepository.setAudioVolume(value) }
    fun setTouchOpacity(value: Float) =
        viewModelScope.launch { settingsRepository.setTouchOpacity(value) }
    fun setHaptics(value: Boolean) = viewModelScope.launch { settingsRepository.setHaptics(value) }
    fun setGamepad(value: Boolean) = viewModelScope.launch { settingsRepository.setGamepad(value) }
}

fun formatBytes(bytes: Long): String {
    if (bytes < 1024) return "$bytes B"
    val units = listOf("KiB", "MiB", "GiB", "TiB")
    var value = bytes.toDouble()
    var unit = -1
    while (value >= 1024 && unit < units.lastIndex) {
        value /= 1024
        unit++
    }
    return "%.2f %s".format(value, units[unit])
}
