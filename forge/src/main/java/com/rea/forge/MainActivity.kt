package com.rea.forge

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import com.rea.forge.ui.ForgeApp
import com.rea.forge.ui.ForgeTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            ForgeTheme {
                ForgeApp()
            }
        }
    }
}
