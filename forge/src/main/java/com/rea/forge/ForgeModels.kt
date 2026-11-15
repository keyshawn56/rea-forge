package com.rea.forge

import org.json.JSONArray
import org.json.JSONObject

data class NsoSectionInfo(
    val name: String,
    val fileSize: Long,
    val memorySize: Long,
    val compressed: Boolean,
    val hashVerified: Boolean,
    val sha256: String,
)

data class NsoInfo(
    val valid: Boolean,
    val moduleName: String,
    val version: String,
    val flags: String,
    val fileBytes: Long,
    val bssBytes: Long,
    val sections: List<NsoSectionInfo>,
    val error: String?,
)

data class NpdmInfo(
    val valid: Boolean,
    val name: String,
    val version: Long,
    val is64Bit: Boolean,
    val mainThreadStackSize: Long,
    val systemResourceSize: Long,
    val production: Boolean,
    val aciMagic: Boolean,
    val acidMagic: Boolean,
    val services: List<String>,
    val diagnostic: String?,
    val error: String?,
)

data class NspEntryInfo(
    val name: String,
    val size: Long,
    val kind: String,
    val plaintextHeader: Boolean,
    val contentType: Int,
    val programId: String,
    val diagnostic: String?,
)

data class NspInfo(
    val valid: Boolean,
    val entryCount: Int,
    val ncaCount: Int,
    val plaintextHeaders: Int,
    val encryptedHeaders: Int,
    val contentExtractable: Boolean,
    val notice: String,
    val entries: List<NspEntryInfo>,
    val error: String?,
)

data class A64Info(
    val valid: Boolean,
    val instructions: Long,
    val svcSites: Long,
    val tpidrroReads: Long,
    val tpidrroWrites: Long,
    val branchRegister: Long,
    val callRegister: Long,
    val systemRegisters: List<String>,
    val textSha256: String,
    val blockers: List<String>,
    val notes: List<String>,
    val nso: NsoInfo?,
    val error: String?,
)

data class RomFsInfo(
    val valid: Boolean,
    val mode: String,
    val directoryCount: Long,
    val fileCount: Long,
    val contentBytes: Long,
    val truncated: Boolean,
    val topLevel: List<String>,
    val extensions: Map<String, Long>,
    val shaderPackages: Long,
    val ncaFiles: Long,
    val packFiles: Long,
    val warnings: List<String>,
    val error: String?,
)

data class ShaderPackageInfo(
    val name: String,
    val kind: String,
    val size: Long,
    val magic: String,
    val validSpirv: Boolean,
    val diagnostic: String?,
)

data class ShaderInfo(
    val valid: Boolean,
    val files: Long,
    val spirvFiles: Long,
    val kpsFiles: Long,
    val bfresFiles: Long,
    val totalBytes: Long,
    val truncated: Boolean,
    val packages: List<ShaderPackageInfo>,
    val warnings: List<String>,
    val error: String?,
)

data class PipelineInfo(
    val valid: Boolean,
    val vulkanAvailable: Boolean,
    val cacheWritten: Boolean,
    val deviceName: String,
    val vendorId: Long,
    val deviceId: Long,
    val driverVersion: Long,
    val apiVersion: Long,
    val cacheBytes: Long,
    val shadersStaged: Int,
    val shadersSkipped: Int,
    val pipelinesCompiled: Int,
    val pipelinesFailed: Int,
    val diagnostics: List<String>,
    val error: String?,
)

data class ExtractInfo(
    val valid: Boolean,
    val bytes: Long,
    val entries: List<String>,
    val warning: String,
    val error: String?,
)

data class ExefsInfo(
    val valid: Boolean,
    val mainName: String?,
    val npdmName: String?,
    val nsoCount: Int,
    val npdmCount: Int,
    val nso: NsoInfo?,
    val npdm: NpdmInfo?,
    val error: String?,
)

private fun JSONObject.text(key: String): String =
    if (has(key) && !isNull(key)) optString(key) else ""

private fun JSONObject.strings(key: String): List<String> {
    val array = optJSONArray(key) ?: return emptyList()
    return buildList {
        for (i in 0 until array.length()) {
            val value = array.optString(i)
            if (value.isNotEmpty()) add(value)
        }
    }
}

private fun parseObject(json: String): JSONObject? = try {
    JSONObject(json)
} catch (_: Exception) {
    null
}

fun parseBuildInfo(json: String): Map<String, String> {
    val objectValue = parseObject(json) ?: return emptyMap()
    return objectValue.keys().asSequence().associateWith { objectValue.text(it) }
}

fun parseNso(json: String): NsoInfo {
    val root = parseObject(json)
        ?: return NsoInfo(false, "", "", "", 0, 0, emptyList(), "native response was not valid JSON")
    val sections = buildList {
        val array = root.optJSONArray("sections") ?: JSONArray()
        for (i in 0 until array.length()) {
            val item = array.optJSONObject(i) ?: continue
            add(
                NsoSectionInfo(
                    name = item.text("name"),
                    fileSize = item.optLong("fileSize"),
                    memorySize = item.optLong("memorySize"),
                    compressed = item.optBoolean("compressed"),
                    hashVerified = item.optBoolean("hashRequired"),
                    sha256 = item.text("sha256"),
                )
            )
        }
    }
    return NsoInfo(
        valid = root.optBoolean("valid"),
        moduleName = root.text("moduleName"),
        version = root.text("version"),
        flags = root.text("flags"),
        fileBytes = root.optLong("fileBytes"),
        bssBytes = root.optLong("bssBytes"),
        sections = sections,
        error = root.text("error").ifEmpty { null },
    )
}

fun parseNpdm(json: String): NpdmInfo {
    val root = parseObject(json)
        ?: return NpdmInfo(false, "", 0, false, 0, 0, false, false, false, emptyList(), null, "invalid JSON")
    return NpdmInfo(
        valid = root.optBoolean("valid"),
        name = root.text("name"),
        version = root.optLong("version"),
        is64Bit = root.optBoolean("is64Bit"),
        mainThreadStackSize = root.optLong("mainThreadStackSize"),
        systemResourceSize = root.optLong("systemResourceSize"),
        production = root.optBoolean("production"),
        aciMagic = root.optBoolean("aciMagic"),
        acidMagic = root.optBoolean("acidMagic"),
        services = root.strings("services"),
        diagnostic = root.text("diagnostic").ifEmpty { null },
        error = root.text("error").ifEmpty { null },
    )
}

fun parseNsp(json: String): NspInfo {
    val root = parseObject(json)
        ?: return NspInfo(false, 0, 0, 0, 0, false, "", emptyList(), "invalid JSON")
    val entries = buildList {
        val array = root.optJSONArray("entries") ?: JSONArray()
        for (i in 0 until array.length()) {
            val item = array.optJSONObject(i) ?: continue
            val nca = item.optJSONObject("nca")
            add(
                NspEntryInfo(
                    name = item.text("name"),
                    size = item.optLong("size"),
                    kind = item.text("kind"),
                    plaintextHeader = nca?.optBoolean("plaintextHeader") ?: false,
                    contentType = nca?.optInt("contentType") ?: -1,
                    programId = nca?.text("programId").orEmpty(),
                    diagnostic = nca?.text("diagnostic")?.ifEmpty { null },
                )
            )
        }
    }
    return NspInfo(
        valid = root.optBoolean("valid"),
        entryCount = root.optInt("entryCount"),
        ncaCount = root.optInt("ncaCount"),
        plaintextHeaders = root.optInt("ncaPlaintextHeaders"),
        encryptedHeaders = root.optInt("ncaEncryptedHeaders"),
        contentExtractable = root.optBoolean("contentExtractable"),
        notice = root.text("notice"),
        entries = entries,
        error = root.text("error").ifEmpty { null },
    )
}

fun parseA64(json: String): A64Info {
    val root = parseObject(json) ?: return A64Info(
        false, 0, 0, 0, 0, 0, 0, emptyList(), "", emptyList(), emptyList(), null, "invalid JSON"
    )
    val scan = root.optJSONObject("scan")
    val nso = root.optJSONObject("nso")?.let { parseNso(it.toString()) }
    return A64Info(
        valid = root.optBoolean("valid"),
        instructions = scan?.optLong("instructions") ?: 0,
        svcSites = scan?.optLong("svcSites") ?: 0,
        tpidrroReads = scan?.optLong("tpidrroReads") ?: 0,
        tpidrroWrites = scan?.optLong("tpidrroWrites") ?: 0,
        branchRegister = scan?.optLong("branchRegister") ?: 0,
        callRegister = scan?.optLong("callRegister") ?: 0,
        systemRegisters = scan?.strings("systemRegisters") ?: emptyList(),
        textSha256 = scan?.text("textSha256").orEmpty(),
        blockers = root.strings("blockers"),
        notes = root.strings("notes"),
        nso = nso,
        error = root.text("error").ifEmpty { null },
    )
}

fun parseRomFs(json: String): RomFsInfo {
    val root = parseObject(json)
        ?: return RomFsInfo(false, "", 0, 0, 0, false, emptyList(), emptyMap(), 0, 0, 0, emptyList(), "invalid JSON")
    val extensions = mutableMapOf<String, Long>()
    root.optJSONObject("extensions")?.let { objectValue ->
        objectValue.keys().asSequence().forEach { key ->
            extensions[key] = objectValue.optLong(key)
        }
    }
    return RomFsInfo(
        valid = root.optBoolean("valid"),
        mode = root.text("mode"),
        directoryCount = root.optLong("directoryCount"),
        fileCount = root.optLong("fileCount"),
        contentBytes = root.optLong("contentBytes"),
        truncated = root.optBoolean("truncated"),
        topLevel = root.strings("topLevel"),
        extensions = extensions,
        shaderPackages = root.optLong("shaderPackages"),
        ncaFiles = root.optLong("ncaFiles"),
        packFiles = root.optLong("packFiles"),
        warnings = root.strings("warnings"),
        error = root.text("error").ifEmpty { root.text("diagnostic").ifEmpty { null } },
    )
}

fun parseShaders(json: String): ShaderInfo {
    val root = parseObject(json)
        ?: return ShaderInfo(false, 0, 0, 0, 0, 0, false, emptyList(), emptyList(), "invalid JSON")
    val packages = buildList {
        val array = root.optJSONArray("packages") ?: JSONArray()
        for (i in 0 until array.length()) {
            val item = array.optJSONObject(i) ?: continue
            add(
                ShaderPackageInfo(
                    name = item.text("name"),
                    kind = item.text("kind"),
                    size = item.optLong("size"),
                    magic = item.text("magic"),
                    validSpirv = item.optBoolean("validSpirv"),
                    diagnostic = item.text("diagnostic").ifEmpty { null },
                )
            )
        }
    }
    return ShaderInfo(
        valid = root.optBoolean("valid"),
        files = root.optLong("files"),
        spirvFiles = root.optLong("spirvFiles"),
        kpsFiles = root.optLong("kpsFiles"),
        bfresFiles = root.optLong("bfresFiles"),
        totalBytes = root.optLong("totalBytes"),
        truncated = root.optBoolean("truncated"),
        packages = packages,
        warnings = root.strings("warnings"),
        error = root.text("error").ifEmpty { null },
    )
}

fun parsePipeline(json: String): PipelineInfo {
    val root = parseObject(json)
        ?: return PipelineInfo(
            false, false, false, "", 0, 0, 0, 0, 0, 0, 0, 0, 0,
            emptyList(), "invalid JSON"
        )
    return PipelineInfo(
        valid = root.optBoolean("valid"),
        vulkanAvailable = root.optBoolean("vulkanAvailable"),
        cacheWritten = root.optBoolean("cacheWritten"),
        deviceName = root.text("deviceName"),
        vendorId = root.optLong("vendorId"),
        deviceId = root.optLong("deviceId"),
        driverVersion = root.optLong("driverVersion"),
        apiVersion = root.optLong("apiVersion"),
        cacheBytes = root.optLong("cacheBytes"),
        shadersStaged = root.optInt("shadersStaged"),
        shadersSkipped = root.optInt("shadersSkipped"),
        pipelinesCompiled = root.optInt("pipelinesCompiled"),
        pipelinesFailed = root.optInt("pipelinesFailed"),
        diagnostics = root.strings("diagnostics"),
        error = root.text("error").ifEmpty { null },
    )
}

fun parseExtract(json: String): ExtractInfo {
    val root = parseObject(json)
        ?: return ExtractInfo(false, 0, emptyList(), "", "invalid JSON")
    return ExtractInfo(
        valid = root.optBoolean("valid"),
        bytes = root.optLong("bytes"),
        entries = root.strings("entries"),
        warning = root.text("warning"),
        error = root.text("error").ifEmpty { null },
    )
}

fun jsonError(message: String): String =
    JSONObject().put("valid", false).put("error", message).toString()
