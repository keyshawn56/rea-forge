package com.rea.forge.ui

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import com.rea.forge.BuildStage
import com.rea.forge.ContentKind
import com.rea.forge.ForgeBuildReport
import com.rea.forge.ForgeSettings
import com.rea.forge.ForgeViewModel
import com.rea.forge.FpsCap
import com.rea.forge.FrameGeneration
import com.rea.forge.FsrMode
import com.rea.forge.GateState
import com.rea.forge.InternalScale
import com.rea.forge.ShadowQuality
import com.rea.forge.TextureFiltering
import com.rea.forge.UpscaleTarget
import com.rea.forge.formatBytes
import java.text.NumberFormat
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

@Composable
fun ForgeApp(viewModel: ForgeViewModel = viewModel()) {
    val settings by viewModel.settings.collectAsStateWithLifecycle()
    val selection by viewModel.selection.collectAsStateWithLifecycle()
    val gate by viewModel.gate.collectAsStateWithLifecycle()
    val build by viewModel.build.collectAsStateWithLifecycle()
    val extract by viewModel.extract.collectAsStateWithLifecycle()
    val device by viewModel.deviceInfo.collectAsStateWithLifecycle()

    val context = LocalContext.current
    val snackbar = remember { SnackbarHostState() }
    var showExtractDialog by remember { mutableStateOf(false) }

    val notificationPermission = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { }

    val nspPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) {
            persistPermission(context, uri)
            viewModel.selectContent(ContentKind.NSP, uri)
        }
    }
    val exefsPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) {
            persistPermission(context, uri)
            viewModel.selectContent(ContentKind.EXEFS, uri)
        }
    }
    val romfsPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) {
            persistPermission(context, uri)
            viewModel.selectContent(ContentKind.ROMFS, uri)
        }
    }

    LaunchedEffect(build.error) {
        val message = build.error ?: return@LaunchedEffect
        snackbar.showSnackbar(message)
        viewModel.dismissBuildError()
    }
    LaunchedEffect(extract.message, extract.error) {
        val message = extract.message ?: extract.error ?: return@LaunchedEffect
        snackbar.showSnackbar(message)
        viewModel.dismissExtract()
    }

    if (showExtractDialog) {
        AlertDialog(
            onDismissRequest = { showExtractDialog = false },
            title = { Text("Raw container copy") },
            text = {
                Text(
                    "This copies PFS0 entries byte-for-byte into the app's external files directory. " +
                        "Retail NCA payloads stay encrypted because no title keys are present. " +
                        "The copy can be several gigabytes and may take a long time."
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    showExtractDialog = false
                    viewModel.extractNsp()
                }) { Text("Copy raw") }
            },
            dismissButton = {
                TextButton(onClick = { showExtractDialog = false }) { Text("Cancel") }
            },
        )
    }

    Scaffold(
        containerColor = ForgeBg,
        contentWindowInsets = WindowInsets.safeDrawing,
        snackbarHost = { SnackbarHost(snackbar) },
    ) { insets ->
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .padding(insets),
            contentPadding = PaddingValues(16.dp, 14.dp, 16.dp, 36.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            item {
                HeroHeader(gate = gate, device = device)
            }
            item {
                GateCard(
                    gate = gate,
                    selection = selection,
                    extractRunning = extract.running,
                    onPickNsp = { nspPicker.launch(arrayOf("*/*")) },
                    onPickExefs = { exefsPicker.launch(null) },
                    onPickRomfs = { romfsPicker.launch(null) },
                    onClear = { kind -> viewModel.selectContent(kind, null) },
                    onExtract = { showExtractDialog = true },
                )
            }
            item {
                BuildCard(
                    gate = gate,
                    settings = settings,
                    report = build.report,
                    running = build.running,
                    progress = build.progress,
                    stage = build.stage,
                    onStart = {
                        if (Build.VERSION.SDK_INT >= 33 &&
                            ContextCompat.checkSelfPermission(
                                context,
                                Manifest.permission.POST_NOTIFICATIONS
                            ) != PackageManager.PERMISSION_GRANTED
                        ) {
                            notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
                        }
                        viewModel.startBuild()
                    },
                    onCancel = viewModel::cancelBuild,
                )
            }
            item {
                GraphicsCard(settings = settings, viewModel = viewModel)
            }
            item {
                FrameGenerationCard(settings = settings, viewModel = viewModel)
            }
            item {
                PerformanceCard(settings = settings, viewModel = viewModel, report = build.report)
            }
            item {
                AudioInputCard(settings = settings, viewModel = viewModel)
            }
            item {
                LaunchCard(report = build.report)
            }
            item {
                ScopeFooter()
            }
        }
    }
}

@Composable
private fun rememberCoroutineScopeSafe() = androidx.compose.runtime.rememberCoroutineScope()

private fun persistPermission(context: Context, uri: Uri) {
    try {
        context.contentResolver.takePersistableUriPermission(
            uri,
            Intent.FLAG_GRANT_READ_URI_PERMISSION,
        )
    } catch (_: Exception) {
        // Some providers do not offer persistable grants; the selection still
        // works for this process.
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun HeroHeader(gate: GateState, device: Map<String, String>) {
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(28.dp))
            .background(
                Brush.linearGradient(
                    listOf(
                        Color(0xFF1B120D),
                        Color(0xFF0E1720),
                        Color(0xFF0A0E14),
                    )
                )
            )
            .border(1.dp, Color(0x33FF7043), RoundedCornerShape(28.dp))
            .padding(20.dp)
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    modifier = Modifier
                        .size(42.dp)
                        .clip(RoundedCornerShape(12.dp))
                        .background(ForgeOrange.copy(alpha = 0.16f))
                        .border(1.dp, ForgeOrange.copy(alpha = 0.55f), RoundedCornerShape(12.dp)),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        "R",
                        color = ForgeOrange,
                        fontWeight = FontWeight.Black,
                        fontSize = 22.sp,
                        fontFamily = FontFamily.Monospace,
                    )
                }
                Spacer(Modifier.width(12.dp))
                Column(Modifier.weight(1f)) {
                    Row(verticalAlignment = Alignment.Bottom) {
                        Text(
                            "REA",
                            color = ForgeOrange,
                            fontWeight = FontWeight.Black,
                            letterSpacing = 3.sp,
                            fontFamily = FontFamily.Monospace,
                            fontSize = 17.sp,
                        )
                        Spacer(Modifier.width(6.dp))
                        Text(
                            "FORGE",
                            color = ForgeText,
                            fontWeight = FontWeight.Black,
                            letterSpacing = 3.sp,
                            fontFamily = FontFamily.Monospace,
                            fontSize = 17.sp,
                        )
                    }
                    Text(
                        "on-device content pipeline",
                        color = ForgeMuted,
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
                StatusPill(
                    text = if (gate.gateOpen) "GATE OPEN" else "GATE LOCKED",
                    color = if (gate.gateOpen) ForgeMint else ForgeRed,
                )
            }

            Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(
                    "Hyrule Warriors",
                    color = ForgeText,
                    style = MaterialTheme.typography.headlineSmall,
                    fontSize = 30.sp,
                )
                Text(
                    "DEFINITIVE EDITION",
                    color = ForgeOrange,
                    style = MaterialTheme.typography.labelSmall,
                    letterSpacing = 3.5.sp,
                    fontWeight = FontWeight.Bold,
                )
                Text(
                    "Structural validation, AArch64 analysis, shader inventory and Adreno pipeline-cache tooling.",
                    color = ForgeMuted,
                    style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.padding(top = 6.dp),
                )
            }

            FlowRow(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                MiniTag("SNAPDRAGON 8 ELITE", ForgeMint)
                MiniTag("ADRENO 840", ForgeBlue)
                MiniTag("VULKAN", ForgeOrange)
                MiniTag(device["model"]?.take(18) ?: "ANDROID", ForgeMuted)
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                StatusPill("RUNTIME NOT IMPLEMENTED", ForgeAmber)
                StatusPill(device["abi"] ?: "arm64-v8a", ForgeMuted)
            }
        }
    }
}

@Composable
private fun MiniTag(text: String, color: Color) {
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(7.dp))
            .background(color.copy(alpha = 0.08f))
            .border(1.dp, color.copy(alpha = 0.25f), RoundedCornerShape(7.dp))
            .padding(horizontal = 8.dp, vertical = 4.dp)
    ) {
        Text(
            text,
            color = color.copy(alpha = 0.9f),
            style = MaterialTheme.typography.labelSmall,
            letterSpacing = 1.sp,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

@Composable
private fun GateCard(
    gate: GateState,
    selection: com.rea.forge.ContentSelection,
    extractRunning: Boolean,
    onPickNsp: () -> Unit,
    onPickExefs: () -> Unit,
    onPickRomfs: () -> Unit,
    onClear: (ContentKind) -> Unit,
    onExtract: () -> Unit,
) {
    SectionCard(
        title = "LOCAL CONTENT GATE",
        subtitle = "Read-only structural validation. No title keys, tickets, or decryption.",
        accent = if (gate.gateOpen) ForgeMint else ForgeOrange,
    ) {
        ContentSourceRow(
            title = "NSP package",
            path = uriLabel(selection.nspUri),
            status = when {
                gate.checking && selection.nspUri != null -> "Validating PFS0..."
                gate.nsp == null -> "No package selected"
                gate.nsp.valid ->
                    "${count(gate.nsp.entryCount.toLong())} entries • ${gate.nsp.ncaCount} NCA • ${gate.nsp.plaintextHeaders} plaintext headers"
                else -> gate.nsp.error ?: "Invalid container"
            },
            ok = gate.nsp?.valid == true,
            onPick = onPickNsp,
            onClear = if (selection.nspUri != null) ({ onClear(ContentKind.NSP) }) else null,
        )
        HorizontalDivider(color = ForgeLine)
        ContentSourceRow(
            title = "Exefs folder",
            path = uriLabel(selection.exefsUri),
            status = when {
                gate.checking && selection.exefsUri != null -> "Reading NSO and NPDM..."
                gate.exefs == null -> "No executable folder selected"
                gate.exefs.valid ->
                    "${gate.exefs.nsoCount} NSO • ${gate.exefs.npdmCount} NPDM • " +
                        when (gate.exefs.npdm?.is64Bit) {
                            true -> "64-bit"
                            false -> "32-bit"
                            null -> "bitness unknown"
                        }
                else -> gate.exefs.error ?: "Invalid exefs"
            },
            ok = gate.exefs?.valid == true,
            onPick = onPickExefs,
            onClear = if (selection.exefsUri != null) ({ onClear(ContentKind.EXEFS) }) else null,
        )
        HorizontalDivider(color = ForgeLine)
        ContentSourceRow(
            title = "RomFS folder",
            path = uriLabel(selection.romfsUri),
            status = when {
                gate.checking && selection.romfsUri != null -> "Indexing SAF tree..."
                gate.romfs == null -> "No RomFS folder selected"
                gate.romfs.valid ->
                    "${count(gate.romfs.fileCount)} files • ${formatBytes(gate.romfs.contentBytes)} • ${gate.romfs.shaderPackages} KPS"
                else -> gate.romfs.error ?: "Invalid RomFS tree"
            },
            ok = gate.romfs?.valid == true,
            onPick = onPickRomfs,
            onClear = if (selection.romfsUri != null) ({ onClear(ContentKind.ROMFS) }) else null,
        )

        if (gate.checking) {
            LinearProgressIndicator(
                modifier = Modifier.fillMaxWidth(),
                color = ForgeOrange,
                trackColor = ForgeSurfaceVariant,
            )
        }
        if (extractRunning) {
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                LinearProgressIndicator(
                    modifier = Modifier.fillMaxWidth(),
                    color = ForgeMint,
                    trackColor = ForgeSurfaceVariant,
                )
                Text(
                    "Copying PFS0 entries. Do not close the app.",
                    color = ForgeMuted,
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }

        GateBanner(gate)

        val encrypted = (gate.nsp?.encryptedHeaders ?: 0) > 0
        if (encrypted) {
            Text(
                "The NSP container is structurally valid, but ${gate.nsp?.encryptedHeaders} NCA headers are encrypted. " +
                    "This app will not derive or use title keys. Select an already-extracted exefs + romfs pair to unlock build stages.",
                color = ForgeMuted,
                style = MaterialTheme.typography.bodySmall,
            )
            OutlinedButton(onClick = onExtract) {
                Text("Raw container copy")
            }
        }
        if (gate.nsp?.valid == true && !gate.nsp.contentExtractable) {
            Text(
                "Notice: ${gate.nsp.notice}",
                color = ForgeMuted,
                style = MaterialTheme.typography.bodySmall,
            )
        }

        RequirementRow("NSP container OR extracted exefs + romfs", gate.gateOpen)
        RequirementRow("Extracted exefs + romfs for build stages", gate.buildReady)
        RequirementRow("Playable Horizon/NVN runtime", false)
    }
}

@Composable
private fun ContentSourceRow(
    title: String,
    path: String?,
    status: String,
    ok: Boolean,
    onPick: () -> Unit,
    onClear: (() -> Unit)?,
) {
    Column(verticalArrangement = Arrangement.spacedBy(7.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(
                modifier = Modifier
                    .size(8.dp)
                    .background(if (ok) ForgeMint else ForgeMuted.copy(alpha = 0.5f), CircleShape)
            )
            Spacer(Modifier.width(9.dp))
            Text(
                title,
                color = ForgeText,
                style = MaterialTheme.typography.titleSmall,
                modifier = Modifier.weight(1f),
            )
            TextButton(onClick = onPick, contentPadding = PaddingValues(horizontal = 8.dp)) {
                Text(if (path == null) "Choose" else "Change", color = ForgeOrange)
            }
            if (onClear != null) {
                TextButton(onClick = onClear, contentPadding = PaddingValues(horizontal = 8.dp)) {
                    Text("Clear", color = ForgeMuted)
                }
            }
        }
        Text(
            path ?: "Not selected",
            color = if (path == null) ForgeMuted else ForgeText.copy(alpha = 0.85f),
            style = MaterialTheme.typography.bodySmall,
            fontFamily = FontFamily.Monospace,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        Text(
            status,
            color = when {
                ok -> ForgeMint
                status.startsWith("No ") || status.startsWith("Not ") -> ForgeMuted
                else -> ForgeRed
            },
            style = MaterialTheme.typography.bodySmall,
        )
    }
}

@Composable
private fun GateBanner(gate: GateState) {
    val open = gate.gateOpen
    val color = if (open) ForgeMint else ForgeRed
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(14.dp))
            .background(color.copy(alpha = 0.08f))
            .border(1.dp, color.copy(alpha = 0.3f), RoundedCornerShape(14.dp))
            .padding(13.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(9.dp).background(color, CircleShape))
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(
                if (open) "GATE OPEN" else "GATE LOCKED",
                color = color,
                style = MaterialTheme.typography.labelSmall,
                letterSpacing = 1.4.sp,
                fontWeight = FontWeight.Bold,
            )
            Text(
                when {
                    gate.error != null -> gate.error
                    gate.buildReady -> "Local content passed. Build stages are available; launch remains blocked."
                    open -> "Container checks passed. Extracted exefs + romfs are still required for build stages."
                    else -> "Supply a valid NSP, or an extracted exefs and romfs pair."
                },
                color = ForgeMuted,
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}

@Composable
private fun RequirementRow(text: String, met: Boolean) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(
            if (met) "✓" else "○",
            color = if (met) ForgeMint else ForgeMuted,
            fontFamily = FontFamily.Monospace,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.width(22.dp),
        )
        Text(
            text,
            color = if (met) ForgeText else ForgeMuted,
            style = MaterialTheme.typography.bodySmall,
        )
    }
}

@Composable
private fun BuildCard(
    gate: GateState,
    settings: ForgeSettings,
    report: ForgeBuildReport?,
    running: Boolean,
    progress: Int,
    stage: String,
    onStart: () -> Unit,
    onCancel: () -> Unit,
) {
    SectionCard(
        title = "BUILD & ANALYSIS",
        subtitle = "The worker validates, decompresses NSO sections, scans A64 text and builds the Adreno cache.",
        accent = ForgeBlue,
    ) {
        if (!gate.buildReady) {
            Text(
                "Build is locked until an extracted exefs and romfs pair is selected. An NSP alone is only a container check.",
                color = ForgeMuted,
                style = MaterialTheme.typography.bodySmall,
            )
        }
        if (running) {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                LinearProgressIndicator(
                    progress = { progress / 100f },
                    modifier = Modifier.fillMaxWidth(),
                    color = ForgeBlue,
                    trackColor = ForgeSurfaceVariant,
                )
                Row {
                    Text(
                        stage.ifEmpty { "Working..." },
                        color = ForgeText,
                        style = MaterialTheme.typography.bodySmall,
                        modifier = Modifier.weight(1f),
                    )
                    Text(
                        "$progress%",
                        color = ForgeBlue,
                        style = MaterialTheme.typography.labelSmall,
                        fontFamily = FontFamily.Monospace,
                    )
                }
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Button(
                onClick = onStart,
                enabled = gate.buildReady && !running,
                colors = ButtonDefaults.buttonColors(
                    containerColor = ForgeOrange,
                    contentColor = Color(0xFF1A0C06),
                    disabledContainerColor = ForgeSurfaceVariant,
                    disabledContentColor = ForgeMuted,
                ),
            ) {
                Text(if (report == null) "Run local build" else "Run again")
            }
            if (running) {
                OutlinedButton(onClick = onCancel) { Text("Cancel") }
            }
        }

        if (report != null) {
            HorizontalDivider(color = ForgeLine)
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    "REPORT",
                    color = ForgeMuted,
                    style = MaterialTheme.typography.labelSmall,
                    letterSpacing = 1.2.sp,
                )
                Spacer(Modifier.weight(1f))
                Text(
                    SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(Date(report.finishedAt)),
                    color = ForgeMuted,
                    style = MaterialTheme.typography.labelSmall,
                    fontFamily = FontFamily.Monospace,
                )
            }
            report.stages.forEach { stageItem ->
                StageRow(stageItem)
            }
            if (report.pipelineCachePath != null) {
                MetricLine("Pipeline cache", formatBytes(report.pipelineCacheBytes))
                MetricLine("Cache file", report.pipelineCachePath)
            }
            if (report.blockers.isNotEmpty()) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(14.dp))
                        .background(ForgeRed.copy(alpha = 0.07f))
                        .border(1.dp, ForgeRed.copy(alpha = 0.25f), RoundedCornerShape(14.dp))
                        .padding(13.dp),
                    verticalArrangement = Arrangement.spacedBy(5.dp),
                ) {
                    Text(
                        "WHY LAUNCH IS DISABLED",
                        color = ForgeRed,
                        style = MaterialTheme.typography.labelSmall,
                        letterSpacing = 1.2.sp,
                        fontWeight = FontWeight.Bold,
                    )
                    report.blockers.forEach { blocker ->
                        Text("— $blocker", color = ForgeMuted, style = MaterialTheme.typography.bodySmall)
                    }
                }
            }
            if (report.warnings.isNotEmpty()) {
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(
                        "WARNINGS",
                        color = ForgeAmber,
                        style = MaterialTheme.typography.labelSmall,
                        letterSpacing = 1.2.sp,
                    )
                    report.warnings.take(8).forEach { warning ->
                        Text("• $warning", color = ForgeMuted, style = MaterialTheme.typography.bodySmall)
                    }
                }
            }
        } else {
            Text(
                "No report yet. The worker writes files to app-private storage and runs a foreground notification while active.",
                color = ForgeMuted,
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}

@Composable
private fun StageRow(stage: BuildStage) {
    val color = when (stage.status) {
        "pass" -> ForgeMint
        "warn" -> ForgeAmber
        "fail" -> ForgeRed
        "blocked" -> ForgeBlue
        else -> ForgeMuted
    }
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.Top,
    ) {
        Box(
            Modifier
                .padding(top = 5.dp)
                .size(8.dp)
                .background(color, CircleShape)
        )
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    stage.title,
                    color = ForgeText,
                    style = MaterialTheme.typography.titleSmall,
                    modifier = Modifier.weight(1f),
                )
                Text(
                    stage.status.uppercase(),
                    color = color,
                    style = MaterialTheme.typography.labelSmall,
                    letterSpacing = 0.8.sp,
                )
            }
            Text(stage.detail, color = ForgeMuted, style = MaterialTheme.typography.bodySmall)
            if (stage.metrics.isNotEmpty()) {
                Text(
                    stage.metrics.entries.take(4).joinToString("  •  ") { "${it.key}=${it.value}" },
                    color = ForgeMuted.copy(alpha = 0.75f),
                    style = MaterialTheme.typography.labelSmall,
                    fontFamily = FontFamily.Monospace,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun GraphicsCard(settings: ForgeSettings, viewModel: ForgeViewModel) {
    SectionCard(
        title = "GRAPHICS",
        subtitle = "Persisted renderer preferences for the planned Vulkan/Adreno path.",
        accent = ForgeOrange,
    ) {
        ChoiceRow(
            label = "Internal resolution",
            options = InternalScale.entries,
            selected = settings.internalScale,
            labelOf = { it.label },
            onSelect = viewModel::setInternalScale,
        )
        ChoiceRow(
            label = "FSR 1",
            options = FsrMode.entries,
            selected = settings.fsrMode,
            labelOf = { it.label },
            onSelect = viewModel::setFsrMode,
        )
        ChoiceRow(
            label = "Upscale target",
            options = UpscaleTarget.entries,
            selected = settings.upscaleTarget,
            labelOf = { it.label },
            onSelect = viewModel::setUpscaleTarget,
        )
        ChoiceRow(
            label = "Texture filtering",
            options = TextureFiltering.entries,
            selected = settings.textureFiltering,
            labelOf = { it.label },
            onSelect = viewModel::setTextureFiltering,
        )
        ChoiceRow(
            label = "Shadow quality",
            options = ShadowQuality.entries,
            selected = settings.shadows,
            labelOf = { it.label },
            onSelect = viewModel::setShadows,
        )
        SwitchRow(
            title = "Snapdragon GSR upscaler",
            subtitle = "Edge-aware GSR-style pass, staged as GLSL compute alongside FSR.",
            checked = settings.gsr,
            onCheckedChange = viewModel::setGsr,
        )
        SwitchRow(
            title = "Bloom",
            subtitle = "HDR-friendly bloom composite.",
            checked = settings.bloom,
            onCheckedChange = viewModel::setBloom,
        )
        SwitchRow(
            title = "Motion blur",
            subtitle = "Camera motion blur. Off is recommended for 60 fps.",
            checked = settings.motionBlur,
            onCheckedChange = viewModel::setMotionBlur,
        )
        SwitchRow(
            title = "V-Sync",
            subtitle = "Present with FIFO. Disable only when testing latency.",
            checked = settings.vsync,
            onCheckedChange = viewModel::setVsync,
        )
    }
}

@Composable
private fun FrameGenerationCard(settings: ForgeSettings, viewModel: ForgeViewModel) {
    SectionCard(
        title = "FRAME GENERATION",
        subtitle = "Optical-flow interpolation with a HUD mask; preferences only until a runtime exists.",
        accent = ForgeMint,
    ) {
        ChoiceRow(
            label = "Interpolation",
            options = FrameGeneration.entries,
            selected = settings.frameGeneration,
            labelOf = { it.label },
            onSelect = viewModel::setFrameGeneration,
        )
        SwitchRow(
            title = "HUD mask",
            subtitle = "Exclude UI regions from motion-vector interpolation.",
            checked = settings.hudMask,
            onCheckedChange = viewModel::setHudMask,
        )
        SliderRow(
            label = "Optical-flow sensitivity",
            value = settings.flowSensitivity,
            valueText = "${(settings.flowSensitivity * 100).toInt()}%",
            enabled = settings.frameGeneration != FrameGeneration.OFF,
            onValueChange = viewModel::setFlowSensitivity,
        )
        ChoiceRow(
            label = "Frame-rate cap",
            options = FpsCap.entries,
            selected = settings.fpsCap,
            labelOf = { it.label },
            onSelect = viewModel::setFpsCap,
        )
    }
}

@Composable
private fun PerformanceCard(
    settings: ForgeSettings,
    viewModel: ForgeViewModel,
    report: ForgeBuildReport?,
) {
    val pipeline = report?.stages?.firstOrNull { it.id == "pipeline" }
    SectionCard(
        title = "PERFORMANCE",
        subtitle = "Adreno cache policy and device facts.",
        accent = ForgeBlue,
    ) {
        SwitchRow(
            title = "Precompile bundled shaders",
            subtitle = "Validate and stage the FSR, GSR, frame interpolation and scene SPIR-V assets.",
            checked = settings.precompileShaders,
            onCheckedChange = viewModel::setPrecompileShaders,
        )
        SwitchRow(
            title = "Persistent pipeline cache",
            subtitle = "Keep pipeline_cache.bin between sessions.",
            checked = settings.persistentPipelineCache,
            onCheckedChange = viewModel::setPersistentPipelineCache,
        )
        SwitchRow(
            title = "Low-latency presentation",
            subtitle = "Prefer mailbox/Immediate present modes when a swapchain exists.",
            checked = settings.lowLatency,
            onCheckedChange = viewModel::setLowLatency,
        )
        HorizontalDivider(color = ForgeLine)
        MetricLine("Vulkan device", pipeline?.metrics?.get("device").orEmpty().ifEmpty { "Run a build to query" })
        MetricLine("Vendor / device", listOfNotNull(
            pipeline?.metrics?.get("vendor"),
            pipeline?.metrics?.get("deviceId"),
        ).joinToString(" / ").ifEmpty { "Unknown" })
        MetricLine("Pipelines compiled", pipeline?.metrics?.get("pipelinesCompiled") ?: "—")
        MetricLine("Pipelines failed", pipeline?.metrics?.get("pipelinesFailed") ?: "—")
        MetricLine("Non-compute stages skipped", pipeline?.metrics?.get("shadersSkipped") ?: "—")
        MetricLine("Cache size", pipeline?.metrics?.get("cacheBytes")?.toLongOrNull()?.let(::formatBytes) ?: "—")
    }
}

@Composable
private fun AudioInputCard(settings: ForgeSettings, viewModel: ForgeViewModel) {
    SectionCard(
        title = "AUDIO & INPUT",
        subtitle = "Persisted controller, haptic and overlay preferences.",
        accent = ForgeAmber,
    ) {
        SliderRow(
            label = "Master volume",
            value = settings.audioVolume,
            valueText = "${(settings.audioVolume * 100).toInt()}%",
            onValueChange = viewModel::setAudioVolume,
        )
        SliderRow(
            label = "Touch overlay opacity",
            value = settings.touchOpacity,
            valueText = "${(settings.touchOpacity * 100).toInt()}%",
            onValueChange = viewModel::setTouchOpacity,
        )
        SwitchRow(
            title = "Haptics",
            subtitle = "Rumble on supported controllers and touch controls.",
            checked = settings.haptics,
            onCheckedChange = viewModel::setHaptics,
        )
        SwitchRow(
            title = "Gamepad input",
            subtitle = "Map physical controllers before touch fallback.",
            checked = settings.gamepad,
            onCheckedChange = viewModel::setGamepad,
        )
    }
}

@Composable
private fun LaunchCard(report: ForgeBuildReport?) {
    SectionCard(
        title = "LAUNCH",
        subtitle = "Hard-blocked until a real Horizon OS and NVN runtime exists.",
        accent = ForgeRed,
    ) {
        Button(
            onClick = { },
            enabled = false,
            modifier = Modifier.fillMaxWidth(),
            colors = ButtonDefaults.buttonColors(
                disabledContainerColor = ForgeSurfaceVariant,
                disabledContentColor = ForgeMuted,
            ),
        ) {
            Text("LAUNCH GAME — RUNTIME NOT IMPLEMENTED", fontWeight = FontWeight.Bold)
        }
        Text(
            "This project does not boot the game. It validates local content, decompresses and hashes NSO sections, " +
                "statically inventories AArch64 instructions, counts KPS shader packages, and can build a Vulkan pipeline cache " +
                "from the bundled post-process shaders. Starting Hyrule Warriors would additionally require Horizon kernel/IPC services, " +
                "NVN-to-Vulkan command translation, title-specific HLE and a Maxwell shader translator.",
            color = ForgeMuted,
            style = MaterialTheme.typography.bodySmall,
        )
        if (report != null) {
            MetricLine("Build gate", if (report.gateOpen) "open" else "locked")
            MetricLine("Runtime ready", if (report.runtimeReady) "yes" else "no")
        }
    }
}

@Composable
private fun ScopeFooter() {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(
            "SCOPE",
            color = ForgeMuted,
            style = MaterialTheme.typography.labelSmall,
            letterSpacing = 1.5.sp,
        )
        Text(
            "Local content only. No title keys, ticket parsing, NCA decryption or entitlement bypass is included. " +
                "NSP validation is structural; retail payloads stay encrypted. Game files are never uploaded. " +
                "The graphics controls are persisted preferences for the planned runtime, not a claim that the game is playable today.",
            color = ForgeMuted.copy(alpha = 0.8f),
            style = MaterialTheme.typography.bodySmall,
        )
        Spacer(Modifier.height(4.dp))
        Text(
            "REA FORGE • 0.2.0-forge • arm64-v8a",
            color = ForgeOrange.copy(alpha = 0.7f),
            style = MaterialTheme.typography.labelSmall,
            fontFamily = FontFamily.Monospace,
            letterSpacing = 0.8.sp,
        )
    }
}

@Composable
private fun SectionCard(
    title: String,
    subtitle: String,
    accent: Color,
    content: @Composable ColumnScope.() -> Unit,
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(22.dp),
        colors = CardDefaults.cardColors(containerColor = ForgeSurface),
        border = BorderStroke(1.dp, ForgeLine),
    ) {
        Column(
            modifier = Modifier.padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    Modifier
                        .width(3.dp)
                        .height(30.dp)
                        .clip(RoundedCornerShape(2.dp))
                        .background(accent)
                )
                Spacer(Modifier.width(11.dp))
                Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text(
                        title,
                        color = accent,
                        style = MaterialTheme.typography.labelSmall,
                        letterSpacing = 1.6.sp,
                        fontWeight = FontWeight.Bold,
                    )
                    Text(
                        subtitle,
                        color = ForgeMuted,
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            }
            HorizontalDivider(color = ForgeLine)
            content()
        }
    }
}

@OptIn(ExperimentalLayoutApi::class, ExperimentalMaterial3Api::class)
@Composable
private fun <T> ChoiceRow(
    label: String,
    options: List<T>,
    selected: T,
    labelOf: (T) -> String,
    onSelect: (T) -> Unit,
) {
    Column(verticalArrangement = Arrangement.spacedBy(7.dp)) {
        Text(
            label.uppercase(),
            color = ForgeMuted,
            style = MaterialTheme.typography.labelSmall,
            letterSpacing = 0.9.sp,
        )
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(7.dp),
            verticalArrangement = Arrangement.spacedBy(7.dp),
        ) {
            options.forEach { option ->
                FilterChip(
                    selected = option == selected,
                    onClick = { onSelect(option) },
                    label = {
                        Text(
                            labelOf(option),
                            style = MaterialTheme.typography.labelSmall,
                        )
                    },
                    shape = RoundedCornerShape(9.dp),
                    colors = FilterChipDefaults.filterChipColors(
                        containerColor = ForgeSurfaceVariant,
                        labelColor = ForgeMuted,
                        selectedContainerColor = ForgeOrange.copy(alpha = 0.18f),
                        selectedLabelColor = ForgeOrange,
                    ),
                    border = FilterChipDefaults.filterChipBorder(
                        enabled = true,
                        selected = option == selected,
                        borderColor = ForgeLine,
                        selectedBorderColor = ForgeOrange.copy(alpha = 0.45f),
                    ),
                )
            }
        }
    }
}

@Composable
private fun SwitchRow(
    title: String,
    subtitle: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable { onCheckedChange(!checked) },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(title, color = ForgeText, style = MaterialTheme.typography.titleSmall)
            Text(subtitle, color = ForgeMuted, style = MaterialTheme.typography.bodySmall)
        }
        Spacer(Modifier.width(12.dp))
        Switch(
            checked = checked,
            onCheckedChange = onCheckedChange,
            colors = SwitchDefaults.colors(
                checkedThumbColor = Color(0xFF1A0C06),
                checkedTrackColor = ForgeOrange,
                uncheckedThumbColor = ForgeMuted,
                uncheckedTrackColor = ForgeSurfaceVariant,
                uncheckedBorderColor = ForgeLine,
            ),
        )
    }
}

@Composable
private fun SliderRow(
    label: String,
    value: Float,
    valueText: String,
    enabled: Boolean = true,
    onValueChange: (Float) -> Unit,
) {
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Row {
            Text(label, color = ForgeText, style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f))
            Text(valueText, color = ForgeOrange, style = MaterialTheme.typography.labelSmall)
        }
        Slider(
            value = value,
            onValueChange = onValueChange,
            enabled = enabled,
            colors = SliderDefaults.colors(
                thumbColor = ForgeOrange,
                activeTrackColor = ForgeOrange,
                inactiveTrackColor = ForgeSurfaceVariant,
            ),
        )
    }
}

@Composable
private fun MetricLine(label: String, value: String) {
    Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(
            label,
            color = ForgeMuted,
            style = MaterialTheme.typography.bodySmall,
            modifier = Modifier.weight(1f),
        )
        Text(
            value.ifEmpty { "—" },
            color = ForgeText.copy(alpha = 0.9f),
            style = MaterialTheme.typography.bodySmall,
            fontFamily = FontFamily.Monospace,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

@Composable
private fun StatusPill(text: String, color: Color) {
    Box(
        modifier = Modifier
            .clip(CircleShape)
            .background(color.copy(alpha = 0.1f))
            .border(1.dp, color.copy(alpha = 0.35f), CircleShape)
            .padding(horizontal = 10.dp, vertical = 5.dp)
    ) {
        Text(
            text,
            color = color,
            style = MaterialTheme.typography.labelSmall,
            letterSpacing = 0.9.sp,
            maxLines = 1,
        )
    }
}

private fun uriLabel(value: String?): String? {
    if (value.isNullOrBlank()) return null
    return try {
        val uri = Uri.parse(value)
        val segment = uri.lastPathSegment ?: return value.take(48)
        Uri.decode(segment).substringAfterLast('/').substringAfterLast(':').take(64)
    } catch (_: Exception) {
        value.take(64)
    }
}

private fun count(value: Long): String = NumberFormat.getIntegerInstance().format(value)
