package com.rea.harness

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.ParcelFileDescriptor
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

private val harnessColors = darkColorScheme(
    primary = Color(0xFFFF7A45),
    onPrimary = Color(0xFF1B0E07),
    secondary = Color(0xFF7FD1C0),
    onSecondary = Color(0xFF07201B),
    tertiary = Color(0xFFB9C7FF),
    background = Color(0xFF0C0E11),
    onBackground = Color(0xFFE7E9EE),
    surface = Color(0xFF151920),
    onSurface = Color(0xFFE7E9EE),
    surfaceVariant = Color(0xFF1E242D),
    onSurfaceVariant = Color(0xFFAAB3C0),
    error = Color(0xFFFF6B6B)
)

private val logTimeFormatter: DateTimeFormatter = DateTimeFormatter.ofPattern("HH:mm:ss")

class MainActivity : ComponentActivity() {
    private lateinit var store: SettingsStore

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        store = SettingsStore(applicationContext)
        setContent {
            MaterialTheme(colorScheme = harnessColors) {
                Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
                    HarnessScreen(store)
                }
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        if (isFinishing) {
            runCatching { NativeBridge.shutdown() }
        }
    }
}

private sealed interface GateState {
    data object Idle : GateState
    data object Checking : GateState
    data class Ready(val result: GateResult.Ready) : GateState
    data class Rejected(val reason: String) : GateState
}

@Composable
private fun HarnessScreen(store: SettingsStore) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val treeUri by store.treeUri.collectAsState(initial = null)
    val settings by store.settings.collectAsState(initial = GraphicsSettings())
    val logLines by LogBus.lines.collectAsState(initial = emptyList())

    var gate by remember { mutableStateOf<GateState>(GateState.Idle) }
    var booted by remember { mutableStateOf(false) }
    var booting by remember { mutableStateOf(false) }
    var stats by remember { mutableStateOf("") }
    var lastError by remember { mutableStateOf<String?>(null) }
    var validateTick by remember { mutableIntStateOf(0) }

    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) {
            try {
                context.contentResolver.takePersistableUriPermission(
                    uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION
                )
            } catch (error: SecurityException) {
                LogBus.append(LogBus.WARN, "gate", "persistable access not granted: ${error.message}")
            }
            scope.launch { store.setTreeUri(uri.toString()) }
        }
    }

    LaunchedEffect(treeUri, validateTick) {
        val uriString = treeUri
        if (uriString == null) {
            gate = GateState.Idle
            return@LaunchedEffect
        }
        if (booted) {
            runCatching { NativeBridge.shutdown() }
            booted = false
            stats = ""
        }
        gate = GateState.Checking
        val result = IntegrityGate.validate(context, Uri.parse(uriString))
        gate = when (result) {
            is GateResult.Ready -> GateState.Ready(result)
            is GateResult.Rejected -> GateState.Rejected(result.reason)
        }
    }

    LaunchedEffect(booted) {
        if (!booted) {
            stats = ""
            return@LaunchedEffect
        }
        while (true) {
            stats = try {
                NativeBridge.stats()
            } catch (error: Throwable) {
                "{\"error\":\"${error.message}\"}"
            }
            delay(1000)
        }
    }

    LaunchedEffect(booted, settings) {
        if (!booted) {
            return@LaunchedEffect
        }
        runCatching {
            NativeBridge.configure(
                settings.resolution.width,
                settings.resolution.height,
                settings.shadow.size,
                settings.anisotropy.level,
                settings.crowdPercent,
                settings.upscaler.id,
                settings.frameGen.id,
                settings.lowLatency
            )
        }.onFailure { error ->
            LogBus.append(LogBus.ERROR, "engine", "configure failed: ${error.message}")
        }
    }

    val ready = (gate as? GateState.Ready)?.result

    Column(
        modifier = Modifier
            .fillMaxSize()
            .safeDrawingPadding()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                "REA Harness",
                style = MaterialTheme.typography.headlineSmall,
                fontWeight = FontWeight.Bold
            )
            Box(modifier = Modifier.width(10.dp))
            Text(
                "v0.1 · arm64-v8a",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Box(modifier = Modifier.width(16.dp))
            Text(
                "Clean-room AArch64 loader · no Horizon/NVN emulation",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }

        Row(
            modifier = Modifier
                .fillMaxWidth()
                .weight(1f),
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Column(
                modifier = Modifier
                    .weight(1.05f)
                    .fillMaxHeight()
                    .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                GateCard(
                    treeUri = treeUri,
                    gate = gate,
                    booted = booted,
                    booting = booting,
                    stats = stats,
                    lastError = lastError,
                    onPick = { picker.launch(null) },
                    onRevalidate = { validateTick++ },
                    onBoot = {
                        if (ready != null && !booting) {
                            booting = true
                            lastError = null
                            scope.launch {
                                val accepted = withContext(Dispatchers.IO) {
                                    try {
                                        runCatching { NativeBridge.shutdown() }
                                        val result = bootNative(context, ready)
                                        if (result) {
                                            LogBus.append(LogBus.INFO, "engine", "native boot accepted the image")
                                        }
                                        result
                                    } catch (error: Throwable) {
                                        LogBus.append(LogBus.ERROR, "engine", "boot failed: ${error.message}")
                                        false
                                    }
                                }
                                booted = accepted
                                booting = false
                                if (!accepted) {
                                    lastError = "Native boot rejected the image. See the log for the exact reason."
                                }
                            }
                        }
                    }
                )

                SettingsPanel(
                    settings = settings,
                    enabled = !booting,
                    onChange = { updated -> scope.launch { store.update(updated) } }
                )
            }

            LogCard(
                lines = logLines,
                onClear = { LogBus.clear() },
                modifier = Modifier
                    .weight(0.95f)
                    .fillMaxHeight()
            )
        }
    }
}

@Composable
private fun GateCard(
    treeUri: String?,
    gate: GateState,
    booted: Boolean,
    booting: Boolean,
    stats: String,
    lastError: String?,
    onPick: () -> Unit,
    onRevalidate: () -> Unit,
    onBoot: () -> Unit
) {
    val ready = (gate as? GateState.Ready)?.result
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    "Boot gate",
                    modifier = Modifier.weight(1f),
                    style = MaterialTheme.typography.titleMedium
                )
                StatusPill(gate = gate, booted = booted)
            }

            Text("Folder", style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(
                treeUri ?: "No folder selected. Choose a folder containing main.elf, assets.pak, and manifest.sha256.",
                style = MaterialTheme.typography.bodySmall,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis
            )

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = onPick, enabled = !booting) {
                    Text("Choose folder")
                }
                OutlinedButton(onClick = onRevalidate, enabled = treeUri != null && !booting) {
                    Text("Re-check")
                }
            }

            HorizontalDivider(color = MaterialTheme.colorScheme.surfaceVariant)

            when (gate) {
                GateState.Idle -> Text(
                    "Waiting for a folder. The gate runs before any native object is created.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )

                GateState.Checking -> Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(10.dp)
                ) {
                    CircularProgressIndicator(modifier = Modifier.size(18.dp), strokeWidth = 2.dp)
                    Text("Reading headers and hashing payloads…", style = MaterialTheme.typography.bodySmall)
                }

                is GateState.Rejected -> Text(
                    gate.reason,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error
                )

                is GateState.Ready -> Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text("main.elf", style = MaterialTheme.typography.labelMedium)
                    Text(
                        "${IntegrityGate.formatBytes(gate.result.elfBytes)} · SHA-256 ${gate.result.elfSha256.take(16)}…",
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace
                    )
                    Text("assets.pak", style = MaterialTheme.typography.labelMedium)
                    Text(
                        "${IntegrityGate.formatBytes(gate.result.pakBytes)} · SHA-256 ${gate.result.pakSha256.take(16)}…",
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace
                    )
                    Text(
                        "Verified against ${gate.result.manifestName}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.secondary
                    )
                }
            }

            if (lastError != null) {
                Text(
                    lastError,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error
                )
            }

            Button(
                onClick = onBoot,
                enabled = ready != null && !booting,
                modifier = Modifier.fillMaxWidth()
            ) {
                Text(
                    when {
                        booting -> "Loading…"
                        booted -> "Reload image"
                        else -> "Validate & load image"
                    }
                )
            }

            if (booted) {
                Text(
                    "Image validated and mapped. Guest execution and Vulkan presentation are not wired in v0.1.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.tertiary
                )
            }

            if (stats.isNotEmpty()) {
                Text(
                    stats,
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 11.sp
                )
            }

            Text(
                "The manifest is user-supplied and unsigned: this gate detects corruption and accidental mismatch, not a malicious replacement.",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

@Composable
private fun StatusPill(gate: GateState, booted: Boolean) {
    val (label, color) = when {
        booted -> "LOADED" to MaterialTheme.colorScheme.tertiary
        gate is GateState.Ready -> "GATE PASS" to MaterialTheme.colorScheme.secondary
        gate is GateState.Rejected -> "REJECTED" to MaterialTheme.colorScheme.error
        gate is GateState.Checking -> "CHECKING" to MaterialTheme.colorScheme.primary
        else -> "NO FOLDER" to MaterialTheme.colorScheme.onSurfaceVariant
    }
    Text(
        label,
        modifier = Modifier
            .background(color.copy(alpha = 0.16f), RoundedCornerShape(50))
            .padding(horizontal = 10.dp, vertical = 4.dp),
        color = color,
        style = MaterialTheme.typography.labelSmall,
        fontWeight = FontWeight.Bold
    )
}

@Composable
private fun LogCard(lines: List<LogLine>, onClear: () -> Unit, modifier: Modifier = Modifier) {
    val listState = rememberLazyListState()
    LaunchedEffect(lines.size) {
        if (lines.isNotEmpty()) {
            listState.scrollToItem(lines.lastIndex)
        }
    }

    Card(modifier = modifier) {
        Column(modifier = Modifier.fillMaxSize()) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(start = 14.dp, end = 6.dp, top = 6.dp, bottom = 6.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    "Native log",
                    modifier = Modifier.weight(1f),
                    style = MaterialTheme.typography.titleSmall
                )
                TextButton(onClick = onClear) {
                    Text("Clear")
                }
            }
            HorizontalDivider(color = MaterialTheme.colorScheme.surfaceVariant)
            if (lines.isEmpty()) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .weight(1f),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        "No entries yet",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            } else {
                LazyColumn(
                    state = listState,
                    modifier = Modifier
                        .fillMaxWidth()
                        .weight(1f)
                        .padding(horizontal = 12.dp, vertical = 8.dp),
                    verticalArrangement = Arrangement.spacedBy(3.dp)
                ) {
                    items(lines, key = { it.sequence }) { line ->
                        LogRow(line)
                    }
                }
            }
        }
    }
}

@Composable
private fun LogRow(line: LogLine) {
    val color = when (line.level) {
        LogBus.DEBUG -> MaterialTheme.colorScheme.onSurfaceVariant
        LogBus.WARN -> Color(0xFFFFC46B)
        LogBus.ERROR -> MaterialTheme.colorScheme.error
        else -> MaterialTheme.colorScheme.onSurface
    }
    val timestamp = Instant.ofEpochMilli(line.timeMillis)
        .atZone(ZoneId.systemDefault())
        .format(logTimeFormatter)
    Text(
        "$timestamp  ${line.tag}: ${line.message}",
        style = MaterialTheme.typography.bodySmall,
        fontFamily = FontFamily.Monospace,
        fontSize = 11.sp,
        color = color
    )
}

private fun bootNative(context: Context, ready: GateResult.Ready): Boolean {
    val sandbox = File(context.filesDir, "rea-boot").apply { mkdirs() }.absolutePath
    val resolver = context.contentResolver

    val elf = resolver.openFileDescriptor(ready.elfUri, "r")
        ?: throw IOException("openFileDescriptor(main.elf) returned null")
    val pak = try {
        resolver.openFileDescriptor(ready.pakUri, "r")
            ?: throw IOException("openFileDescriptor(assets.pak) returned null")
    } catch (error: Throwable) {
        elf.close()
        throw error
    }

    val elfFd = try {
        elf.detachFd()
    } catch (error: Throwable) {
        pak.close()
        throw error
    }
    val pakFd = try {
        pak.detachFd()
    } catch (error: Throwable) {
        ParcelFileDescriptor.adoptFd(elfFd).close()
        throw error
    }

    return try {
        NativeBridge.boot(elfFd, pakFd, sandbox, ready.elfSha256, ready.pakSha256)
    } catch (error: Throwable) {
        // The JNI layer owns and closes both descriptors once it is entered;
        // this path only runs when the library or symbol lookup failed first.
        ParcelFileDescriptor.adoptFd(elfFd).close()
        ParcelFileDescriptor.adoptFd(pakFd).close()
        throw error
    }
}
