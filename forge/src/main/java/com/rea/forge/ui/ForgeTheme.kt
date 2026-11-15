package com.rea.forge.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp

val ForgeBg = Color(0xFF07090C)
val ForgeSurface = Color(0xFF0F141A)
val ForgeSurfaceVariant = Color(0xFF171F28)
val ForgeOrange = Color(0xFFFF7043)
val ForgeMint = Color(0xFF2DD4A7)
val ForgeBlue = Color(0xFF6EA8FF)
val ForgeRed = Color(0xFFFF5C70)
val ForgeAmber = Color(0xFFFFC857)
val ForgeText = Color(0xFFE9EEF5)
val ForgeMuted = Color(0xFF8B98A8)
val ForgeLine = Color(0xFF222C36)

private val ForgeColors = darkColorScheme(
    primary = ForgeOrange,
    onPrimary = Color(0xFF1A0C06),
    secondary = ForgeMint,
    onSecondary = Color(0xFF04140F),
    tertiary = ForgeBlue,
    background = ForgeBg,
    onBackground = ForgeText,
    surface = ForgeSurface,
    onSurface = ForgeText,
    surfaceVariant = ForgeSurfaceVariant,
    onSurfaceVariant = ForgeMuted,
    outline = ForgeLine,
    error = ForgeRed,
    onError = Color(0xFF1A0509),
)

private val ForgeTypography = Typography(
    headlineSmall = TextStyle(fontWeight = FontWeight.Bold, letterSpacing = (-0.5).sp),
    titleLarge = TextStyle(fontWeight = FontWeight.Bold, letterSpacing = (-0.3).sp),
    titleMedium = TextStyle(fontWeight = FontWeight.SemiBold),
    labelSmall = TextStyle(fontWeight = FontWeight.Medium, letterSpacing = 0.8.sp),
)

@Composable
fun ForgeTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = ForgeColors,
        typography = ForgeTypography,
        content = content,
    )
}
