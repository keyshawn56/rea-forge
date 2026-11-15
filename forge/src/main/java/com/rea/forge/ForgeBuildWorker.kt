package com.rea.forge

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import android.net.Uri
import androidx.work.CoroutineWorker
import androidx.work.Data
import androidx.work.ForegroundInfo
import androidx.work.WorkerParameters
import androidx.work.workDataOf
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withContext
import java.io.File

class ForgeBuildWorker(
    appContext: Context,
    params: WorkerParameters,
) : CoroutineWorker(appContext, params) {
    private val repository = ForgeRepository(appContext)
    private val settingsRepository = SettingsRepository(appContext)
    private val stages = mutableListOf<BuildStage>()
    private val warnings = mutableListOf<String>()
    private var pipelineCachePath: String? = null
    private var pipelineCacheBytes = 0L

    override suspend fun doWork(): Result = withContext(Dispatchers.IO) {
        val startedAt = System.currentTimeMillis()
        try {
            createChannel()
            setForeground(createForegroundInfo(2, "Reading local content selection"))

            val selection = settingsRepository.selection.first()
            val nspUri = selection.nspUri?.let(Uri::parse)
            val exefsUri = selection.exefsUri?.let(Uri::parse)
            val romfsUri = selection.romfsUri?.let(Uri::parse)

            reportProgress(5, "Validating local content")
            val nsp = nspUri?.let { repository.inspectNsp(it) }
            val exefs = exefsUri?.let { repository.inspectExefs(it) }
            val romfs = romfsUri?.let { repository.inspectRomfs(it) }

            val nspValid = nsp?.valid == true
            val exefsValid = exefs?.valid == true
            val romfsValid = romfs?.valid == true
            val gateOpen = nspValid || (exefsValid && romfsValid)
            val buildReady = exefsValid && romfsValid

            stages += BuildStage(
                id = "gate",
                title = "Local content gate",
                status = if (gateOpen) "pass" else "fail",
                detail = when {
                    !gateOpen ->
                        "Supply a structurally valid NSP, or an extracted romfs + exefs pair. No title keys or decryption are used."
                    buildReady ->
                        "The NSP container and/or the extracted exefs + romfs pair passed structural validation."
                    nspValid ->
                        "The NSP container is valid, but its retail NCA payloads remain encrypted. Select extracted romfs + exefs for build stages."
                    else ->
                        "The extracted content passed structural validation."
                },
                metrics = buildMap {
                    nsp?.let {
                        put("nspEntries", it.entryCount.toString())
                        put("ncaEntries", it.ncaCount.toString())
                        put("plaintextNcaHeaders", it.plaintextHeaders.toString())
                        put("encryptedNcaHeaders", it.encryptedHeaders.toString())
                    }
                    exefs?.let {
                        put("nsoFiles", it.nsoCount.toString())
                        put("npdmFiles", it.npdmCount.toString())
                        it.nso?.let { nso ->
                            put("mainModule", nso.moduleName.ifEmpty { "main" })
                            put("mainBytes", nso.fileBytes.toString())
                        }
                    }
                    romfs?.let {
                        put("romfsFiles", it.fileCount.toString())
                        put("romfsBytes", it.contentBytes.toString())
                        put("shaderPackages", it.shaderPackages.toString())
                    }
                },
            )
            if (!gateOpen) {
                warnings += "The build gate stayed locked; no analysis stages were run."
                return@withContext finish(startedAt, gateOpen = false, buildReady = false)
            }

            if (exefsValid) {
                reportProgress(18, "Checking the executable image")
                val npdm = exefs?.npdm
                stages += BuildStage(
                    id = "npdm",
                    title = "NPDM policy",
                    status = if (npdm?.valid == true) "pass" else "warn",
                    detail = if (npdm?.valid == true) {
                        "META/ACID were parsed. This confirms layout, not entitlement."
                    } else {
                        npdm?.error ?: npdm?.diagnostic ?: "NPDM metadata was unavailable."
                    },
                    metrics = buildMap {
                        npdm?.let {
                            put("name", it.name)
                            put("is64Bit", it.is64Bit.toString())
                            put("production", it.production.toString())
                            put("services", it.services.joinToString(", "))
                        }
                    },
                )

                reportProgress(32, "Statically scanning AArch64 text")
                val mainFile = repository.findMainNso(exefsUri!!)
                if (mainFile == null) {
                    stages += BuildStage(
                        id = "a64",
                        title = "AArch64 analysis",
                        status = "fail",
                        detail = "The main NSO could not be reopened from SAF.",
                    )
                } else {
                    val scan = repository.scanA64(mainFile.uri, mainFile.name ?: "main")
                    stages += BuildStage(
                        id = "a64",
                        title = "AArch64 analysis",
                        status = if (scan.valid) "pass" else "fail",
                        detail = if (scan.valid) {
                            "Text decompression, hash checks and static instruction scanning completed. No code was recompiled or executed."
                        } else {
                            scan.error ?: "AArch64 scan failed."
                        },
                        metrics = buildMap {
                            put("instructions", scan.instructions.toString())
                            put("svcSites", scan.svcSites.toString())
                            put("tpidrroReads", scan.tpidrroReads.toString())
                            put("tpidrroWrites", scan.tpidrroWrites.toString())
                            put("systemRegisters", scan.systemRegisters.joinToString(", "))
                            put("textSha256", scan.textSha256)
                        },
                    )
                    if (scan.svcSites > 0) {
                        warnings += "${scan.svcSites} SVC sites need Horizon kernel services that this app does not implement."
                    }
                    if (scan.tpidrroReads + scan.tpidrroWrites > 0) {
                        warnings += "TPIDRRO_EL0 accesses need a per-thread TLS remapping layer."
                    }
                }
            } else {
                stages += BuildStage(
                    id = "a64",
                    title = "AArch64 analysis",
                    status = "skip",
                    detail = "Skipped because no valid exefs was supplied.",
                )
            }

            reportProgress(52, "Indexing RomFS metadata")
            if (romfsValid) {
                stages += BuildStage(
                    id = "romfs",
                    title = "RomFS index",
                    status = "pass",
                    detail = "Directory metadata was indexed through SAF. File payloads were not copied.",
                    metrics = buildMap {
                        romfs?.let {
                            put("files", it.fileCount.toString())
                            put("directories", it.directoryCount.toString())
                            put("bytes", it.contentBytes.toString())
                            put("topLevel", it.topLevel.joinToString(", "))
                            put("truncated", it.truncated.toString())
                        }
                    },
                )
            } else {
                stages += BuildStage(
                    id = "romfs",
                    title = "RomFS index",
                    status = "skip",
                    detail = romfs?.error ?: "Skipped because no valid RomFS was supplied.",
                )
            }
            warnings += romfs?.warnings.orEmpty()

            reportProgress(68, "Inventorying shader packages")
            if (romfsValid) {
                val shaders = repository.scanShaders(romfsUri!!)
                stages += BuildStage(
                    id = "shaders",
                    title = "Shader inventory",
                    status = if (shaders.valid) "warn" else "fail",
                    detail = shaders.error ?: if (shaders.kpsFiles > 0) {
                        "${shaders.kpsFiles} KPS packages were counted. Maxwell bytecode translation is not implemented; no game SPIR-V was emitted."
                    } else {
                        "Shader inventory completed."
                    },
                    metrics = buildMap {
                        put("files", shaders.files.toString())
                        put("kps", shaders.kpsFiles.toString())
                        put("bfres", shaders.bfresFiles.toString())
                        put("spirv", shaders.spirvFiles.toString())
                    },
                )
                warnings += shaders.warnings
            } else {
                stages += BuildStage(
                    id = "shaders",
                    title = "Shader inventory",
                    status = "skip",
                    detail = "Skipped because no valid RomFS was supplied.",
                )
            }

            reportProgress(84, "Building the Adreno pipeline cache")
            val pipeline = repository.buildPipelineCache()
            pipelineCachePath = File(applicationContext.filesDir, "gpu/pipeline_cache.bin")
                .takeIf { it.isFile }?.absolutePath
            pipelineCacheBytes = pipeline.cacheBytes
            stages += BuildStage(
                id = "pipeline",
                title = "Adreno pipeline cache",
                status = when {
                    pipeline.cacheWritten && pipeline.pipelinesCompiled > 0 -> "pass"
                    pipeline.cacheWritten -> "warn"
                    else -> "fail"
                },
                detail = pipeline.error ?: pipeline.diagnostics.firstOrNull()
                    ?: "Pipeline cache stage completed.",
                metrics = buildMap {
                    put("device", pipeline.deviceName)
                    put("vendor", "0x%04x".format(pipeline.vendorId))
                    put("deviceId", "0x%04x".format(pipeline.deviceId))
                    put("apiVersion", pipeline.apiVersion.toString())
                    put("cacheBytes", pipeline.cacheBytes.toString())
                    put("shadersStaged", pipeline.shadersStaged.toString())
                    put("shadersSkipped", pipeline.shadersSkipped.toString())
                    put("pipelinesCompiled", pipeline.pipelinesCompiled.toString())
                    put("pipelinesFailed", pipeline.pipelinesFailed.toString())
                },
            )
            warnings += pipeline.diagnostics

            reportProgress(96, "Writing the build report")
            stages += BuildStage(
                id = "runtime",
                title = "Runtime link",
                status = "blocked",
                detail = "A playable Horizon OS/NVN runtime is not present. The analysis above is real; launch is intentionally disabled.",
            )
            finish(startedAt, gateOpen = true, buildReady = buildReady)
        } catch (error: Exception) {
            stages += BuildStage(
                id = "fatal",
                title = "Build worker",
                status = "fail",
                detail = error.message ?: error.javaClass.simpleName,
            )
            val path = writeReport(startedAt, gateOpen = false, buildReady = false)
            Result.failure(
                workDataOf(
                    "reportPath" to (path?.absolutePath ?: ""),
                    "error" to (error.message ?: "build failed"),
                )
            )
        }
    }

    private suspend fun finish(startedAt: Long, gateOpen: Boolean, buildReady: Boolean): Result {
        val path = writeReport(startedAt, gateOpen, buildReady)
        reportProgress(100, "Report ready")
        val output = Data.Builder()
            .putString("reportPath", path?.absolutePath ?: "")
            .putBoolean("gateOpen", gateOpen)
            .putBoolean("buildReady", buildReady)
            .putBoolean("runtimeReady", false)
            .build()
        return Result.success(output)
    }

    private fun writeReport(startedAt: Long, gateOpen: Boolean, buildReady: Boolean): File? {
        return try {
            val report = ForgeBuildReport(
                startedAt = startedAt,
                finishedAt = System.currentTimeMillis(),
                gateOpen = gateOpen,
                buildReady = buildReady,
                runtimeReady = false,
                stages = stages.toList(),
                pipelineCachePath = pipelineCachePath,
                pipelineCacheBytes = pipelineCacheBytes,
                blockers = listOf(
                    "Horizon OS kernel and IPC services are not implemented.",
                    "NVN command submission is not translated to Vulkan.",
                    "Maxwell game shaders are inventoried, not translated to SPIR-V.",
                    "A64 text is analyzed, not recompiled into an executable image.",
                    "Launch is disabled until a real Horizon/NVN runtime exists.",
                ),
                warnings = warnings.distinct(),
            )
            val directory = File(applicationContext.filesDir, "reports").apply { mkdirs() }
            val latest = File(directory, "latest.json")
            latest.writeText(report.toJson())
            val stamped = File(directory, "build-${report.finishedAt}.json")
            stamped.writeText(report.toJson())
            latest
        } catch (_: Exception) {
            null
        }
    }

    private suspend fun reportProgress(value: Int, text: String) {
        setProgress(workDataOf("progress" to value, "stage" to text))
        try {
            setForeground(createForegroundInfo(value, text))
        } catch (_: Exception) {
            // Foreground promotion can be rejected when notifications are disabled.
        }
    }

    private fun createChannel() {
        val manager = applicationContext.getSystemService(NotificationManager::class.java)
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Forge builds",
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
            description = "Validation, shader cache and analysis progress"
            setShowBadge(false)
        }
        manager.createNotificationChannel(channel)
    }

    private fun createForegroundInfo(progress: Int, text: String): ForegroundInfo {
        val notification = Notification.Builder(applicationContext, CHANNEL_ID)
            .setContentTitle("REA Forge")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setProgress(100, progress.coerceIn(0, 100), false)
            .build()
        return ForegroundInfo(NOTIFICATION_ID, notification)
    }

    companion object {
        const val UNIQUE_WORK = "rea-forge-build"
        const val CHANNEL_ID = "rea_forge_builds"
        private const val NOTIFICATION_ID = 0x0F0E
    }
}
