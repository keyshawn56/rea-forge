package com.rea.harness

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import kotlin.math.roundToInt

@Composable
fun SettingsPanel(
    settings: GraphicsSettings,
    enabled: Boolean,
    onChange: (GraphicsSettings) -> Unit,
    modifier: Modifier = Modifier
) {
    Card(modifier = modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            Text("Graphics", style = MaterialTheme.typography.titleMedium)
            Text(
                "Stored with DataStore and applied when the native engine starts.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            Selector(
                label = "Render resolution",
                value = settings.resolution,
                options = Resolution.entries,
                display = { it.label },
                enabled = enabled
            ) { onChange(settings.copy(resolution = it)) }

            Selector(
                label = "Shadow map",
                value = settings.shadow,
                options = ShadowQuality.entries,
                display = { it.label },
                enabled = enabled
            ) { onChange(settings.copy(shadow = it)) }

            Selector(
                label = "Anisotropic filtering",
                value = settings.anisotropy,
                options = Anisotropy.entries,
                display = { it.label },
                enabled = enabled
            ) { onChange(settings.copy(anisotropy = it)) }

            Text(
                "Crowd density: ${settings.crowdPercent}%",
                style = MaterialTheme.typography.bodyMedium
            )
            Slider(
                value = settings.crowdPercent.toFloat(),
                onValueChange = { value ->
                    val snapped = ((value / 25f).roundToInt() * 25).coerceIn(25, 100)
                    if (snapped != settings.crowdPercent) {
                        onChange(settings.copy(crowdPercent = snapped))
                    }
                },
                valueRange = 25f..100f,
                steps = 2,
                enabled = enabled
            )

            Selector(
                label = "Upscaler",
                value = settings.upscaler,
                options = UpscalerMode.entries,
                display = { it.label },
                enabled = enabled
            ) { onChange(settings.copy(upscaler = it)) }

            Selector(
                label = "Frame generation",
                value = settings.frameGen,
                options = FrameGenMode.entries,
                display = { it.label },
                enabled = enabled
            ) { onChange(settings.copy(frameGen = it)) }

            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("Low-latency mode", style = MaterialTheme.typography.bodyMedium)
                    Text(
                        "Applies to 90/120 Hz frame generation; ignored when frame generation is off.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
                Switch(
                    checked = settings.lowLatency,
                    onCheckedChange = { onChange(settings.copy(lowLatency = it)) },
                    enabled = enabled
                )
            }
        }
    }
}

@Composable
private fun <T> Selector(
    label: String,
    value: T,
    options: List<T>,
    display: (T) -> String,
    enabled: Boolean,
    onSelected: (T) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(label, modifier = Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
        Box {
            OutlinedButton(
                onClick = { expanded = true },
                enabled = enabled,
                modifier = Modifier.widthIn(min = 132.dp)
            ) {
                Text(display(value))
            }
            DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                options.forEach { option ->
                    DropdownMenuItem(
                        text = { Text(display(option)) },
                        onClick = {
                            expanded = false
                            onSelected(option)
                        }
                    )
                }
            }
        }
    }
}
