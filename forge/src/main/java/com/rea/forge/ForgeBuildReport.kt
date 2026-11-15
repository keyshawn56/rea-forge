package com.rea.forge

import org.json.JSONArray
import org.json.JSONObject

data class BuildStage(
    val id: String,
    val title: String,
    val status: String,
    val detail: String,
    val metrics: Map<String, String> = emptyMap(),
)

data class ForgeBuildReport(
    val startedAt: Long,
    val finishedAt: Long,
    val gateOpen: Boolean,
    val buildReady: Boolean,
    val runtimeReady: Boolean,
    val stages: List<BuildStage>,
    val pipelineCachePath: String?,
    val pipelineCacheBytes: Long,
    val blockers: List<String>,
    val warnings: List<String>,
) {
    fun toJson(): String = JSONObject().apply {
        put("startedAt", startedAt)
        put("finishedAt", finishedAt)
        put("gateOpen", gateOpen)
        put("buildReady", buildReady)
        put("runtimeReady", runtimeReady)
        put("stages", JSONArray().apply {
            stages.forEach { stage ->
                put(JSONObject().apply {
                    put("id", stage.id)
                    put("title", stage.title)
                    put("status", stage.status)
                    put("detail", stage.detail)
                    put("metrics", JSONObject().apply {
                        stage.metrics.forEach { (key, value) -> put(key, value) }
                    })
                })
            }
        })
        put("pipelineCachePath", pipelineCachePath ?: "")
        put("pipelineCacheBytes", pipelineCacheBytes)
        put("blockers", JSONArray(blockers))
        put("warnings", JSONArray(warnings))
    }.toString()

    companion object {
        fun fromJson(text: String): ForgeBuildReport? = try {
            val root = JSONObject(text)
            val stages = buildList {
                val array = root.optJSONArray("stages") ?: JSONArray()
                for (i in 0 until array.length()) {
                    val item = array.optJSONObject(i) ?: continue
                    val metrics = mutableMapOf<String, String>()
                    item.optJSONObject("metrics")?.let { objectValue ->
                        objectValue.keys().asSequence().forEach { key ->
                            metrics[key] = objectValue.optString(key)
                        }
                    }
                    add(
                        BuildStage(
                            id = item.optString("id"),
                            title = item.optString("title"),
                            status = item.optString("status"),
                            detail = item.optString("detail"),
                            metrics = metrics,
                        )
                    )
                }
            }
            fun strings(key: String): List<String> {
                val array = root.optJSONArray(key) ?: return emptyList()
                return buildList {
                    for (i in 0 until array.length()) {
                        array.optString(i).takeIf { it.isNotEmpty() }?.let(::add)
                    }
                }
            }
            ForgeBuildReport(
                startedAt = root.optLong("startedAt"),
                finishedAt = root.optLong("finishedAt"),
                gateOpen = root.optBoolean("gateOpen"),
                buildReady = root.optBoolean("buildReady"),
                runtimeReady = root.optBoolean("runtimeReady"),
                stages = stages,
                pipelineCachePath = root.optString("pipelineCachePath").ifEmpty { null },
                pipelineCacheBytes = root.optLong("pipelineCacheBytes"),
                blockers = strings("blockers"),
                warnings = strings("warnings"),
            )
        } catch (_: Exception) {
            null
        }
    }
}
