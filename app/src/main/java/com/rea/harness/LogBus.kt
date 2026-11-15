package com.rea.harness

import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import java.util.concurrent.atomic.AtomicLong

data class LogLine(
    val sequence: Long,
    val level: Int,
    val tag: String,
    val message: String,
    val timeMillis: Long
)

object LogBus {
    const val DEBUG = 0
    const val INFO = 1
    const val WARN = 2
    const val ERROR = 3

    private val sequence = AtomicLong()
    private val _lines = MutableStateFlow<List<LogLine>>(emptyList())
    val lines: StateFlow<List<LogLine>> = _lines.asStateFlow()

    @JvmStatic
    fun append(level: Int, tag: String, message: String) {
        val priority = when (level) {
            DEBUG -> Log.DEBUG
            WARN -> Log.WARN
            ERROR -> Log.ERROR
            else -> Log.INFO
        }
        Log.println(priority, tag, message)
        val line = LogLine(
            sequence = sequence.incrementAndGet(),
            level = level.coerceIn(DEBUG, ERROR),
            tag = tag,
            message = message,
            timeMillis = System.currentTimeMillis()
        )
        _lines.update { current -> (current + line).takeLast(2000) }
    }

    fun clear() {
        _lines.value = emptyList()
    }
}
