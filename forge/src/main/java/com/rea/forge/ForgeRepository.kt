package com.rea.forge

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

class ForgeRepository(private val context: Context) {
    private val nativeProblem: String?
        get() = if (NativeForge.isAvailable) null else
            "native library unavailable for ${android.os.Build.SUPPORTED_ABIS.firstOrNull() ?: "this ABI"}"

    suspend fun inspectNsp(uri: Uri): NspInfo = withContext(Dispatchers.IO) {
        nativeProblem?.let {
            return@withContext NspInfo(false, 0, 0, 0, 0, false, "", emptyList(), it)
        }
        try {
            val file = Saf.document(context, uri)
                ?: return@withContext NspInfo(
                    false, 0, 0, 0, 0, false, "", emptyList(),
                    "the selected NSP is not accessible through SAF"
                )
            if (!file.isFile) {
                return@withContext NspInfo(
                    false, 0, 0, 0, 0, false, "", emptyList(),
                    "select the .nsp file itself, not a folder"
                )
            }
            Saf.withFd(context, file) { fd ->
                parseNsp(NativeForge.nativeValidateNsp(fd, file.name ?: "game.nsp"))
            }
        } catch (error: Exception) {
            NspInfo(
                false, 0, 0, 0, 0, false, "", emptyList(),
                error.message ?: error.javaClass.simpleName
            )
        }
    }

    suspend fun inspectExefs(uri: Uri): ExefsInfo = withContext(Dispatchers.IO) {
        nativeProblem?.let {
            return@withContext ExefsInfo(false, null, null, 0, 0, null, null, it)
        }
        try {
            val tree = Saf.tree(context, uri)
                ?: return@withContext ExefsInfo(
                    false, null, null, 0, 0, null, null,
                    "the selected folder is not accessible through SAF"
                )
            val root = Saf.resolveChild(context, tree, setOf("exefs"))
            val files = Saf.collectFiles(root, maxFiles = 2048, maxDepth = 8)
            val main = files.firstOrNull { (it.name ?: "").equals("main", true) }
                ?: files.firstOrNull { (it.name ?: "").equals("main.nso", true) }
                ?: files.firstOrNull { (it.name ?: "").endsWith(".nso", true) }
            val npdmFile = files.firstOrNull { (it.name ?: "").equals("main.npdm", true) }
                ?: files.firstOrNull { (it.name ?: "").endsWith(".npdm", true) }
            val nsoCount = files.count { (it.name ?: "").endsWith(".nso", true) }
            val npdmCount = files.count { (it.name ?: "").endsWith(".npdm", true) }

            if (main == null) {
                return@withContext ExefsInfo(
                    false, null, npdmFile?.name, nsoCount, npdmCount, null, null,
                    "no main NSO was found. Select the folder that contains main, main.npdm, rtld and sdk."
                )
            }

            val nso = try {
                Saf.withFd(context, main) { fd ->
                    parseNso(NativeForge.nativeInspectNso(fd, main.name ?: "main"))
                }
            } catch (error: Exception) {
                NsoInfo(
                    false, "", "", "", 0, 0, emptyList(),
                    error.message ?: error.javaClass.simpleName
                )
            }
            val npdm = npdmFile?.let { selected ->
                try {
                    Saf.withFd(context, selected) { fd ->
                        parseNpdm(NativeForge.nativeInspectNpdm(fd, selected.name ?: "main.npdm"))
                    }
                } catch (error: Exception) {
                    NpdmInfo(
                        false, "", 0, false, 0, 0, false, false, false, emptyList(),
                        null, error.message ?: error.javaClass.simpleName
                    )
                }
            }

            val valid = nso.valid && npdm?.valid == true
            val error = when {
                !nso.valid -> nso.error ?: "main NSO failed structural validation"
                npdm == null -> "main.npdm is missing next to the main NSO"
                npdm.valid.not() -> npdm.error ?: npdm.diagnostic ?: "main.npdm failed validation"
                else -> null
            }
            ExefsInfo(
                valid = valid,
                mainName = main.name,
                npdmName = npdmFile?.name,
                nsoCount = nsoCount,
                npdmCount = npdmCount,
                nso = nso,
                npdm = npdm,
                error = error,
            )
        } catch (error: Exception) {
            ExefsInfo(
                false, null, null, 0, 0, null, null,
                error.message ?: error.javaClass.simpleName
            )
        }
    }

    suspend fun inspectRomfs(uri: Uri): RomFsInfo = withContext(Dispatchers.IO) {
        try {
            val tree = Saf.tree(context, uri)
                ?: return@withContext RomFsInfo(
                    false, "saf", 0, 0, 0, false, emptyList(), emptyMap(), 0, 0, 0,
                    emptyList(), "the selected folder is not accessible through SAF"
                )
            val root = Saf.resolveChild(context, tree, setOf("romfs"))
            val scan = Saf.scanTree(
                context = context,
                root = root,
                maxFiles = 200_000,
                maxDepth = 24,
            )
            val shaderCount = scan.extensionCounts["kps"] ?: 0
            val valid = scan.error == null && scan.files > 0
            RomFsInfo(
                valid = valid,
                mode = "saf",
                directoryCount = scan.directories,
                fileCount = scan.files,
                contentBytes = scan.bytes,
                truncated = scan.truncated,
                topLevel = scan.topLevel,
                extensions = scan.extensionCounts,
                shaderPackages = shaderCount,
                ncaFiles = scan.extensionCounts["nca"] ?: 0,
                packFiles = scan.extensionCounts["pack"] ?: 0,
                warnings = buildList {
                    if (scan.truncated) add("The SAF walk reached its file limit.")
                    if (scan.topLevel.none { it.equals("data", true) || it.equals("sound", true) }) {
                        add("The folder does not look like a RomFS root; expected data/sound/movie directories.")
                    }
                },
                error = scan.error ?: if (scan.files == 0L) "the RomFS tree is empty" else null,
            )
        } catch (error: Exception) {
            RomFsInfo(
                false, "saf", 0, 0, 0, false, emptyList(), emptyMap(), 0, 0, 0,
                emptyList(), error.message ?: error.javaClass.simpleName
            )
        }
    }

    suspend fun scanA64(uri: Uri, displayName: String): A64Info = withContext(Dispatchers.IO) {
        nativeProblem?.let {
            return@withContext A64Info(
                false, 0, 0, 0, 0, 0, 0, emptyList(), "", emptyList(), emptyList(), null, it
            )
        }
        try {
            val file = Saf.document(context, uri)
                ?: return@withContext A64Info(
                    false, 0, 0, 0, 0, 0, 0, emptyList(), "", emptyList(), emptyList(), null,
                    "the NSO is no longer accessible"
                )
            Saf.withFd(context, file) { fd ->
                parseA64(NativeForge.nativeScanA64(fd, displayName))
            }
        } catch (error: Exception) {
            A64Info(
                false, 0, 0, 0, 0, 0, 0, emptyList(), "", emptyList(), emptyList(), null,
                error.message ?: error.javaClass.simpleName
            )
        }
    }

    suspend fun scanShaders(romfsUri: Uri): ShaderInfo = withContext(Dispatchers.IO) {
        try {
            val tree = Saf.tree(context, romfsUri)
                ?: return@withContext ShaderInfo(
                    false, 0, 0, 0, 0, 0, false, emptyList(), emptyList(),
                    "the RomFS folder is no longer accessible"
                )
            val root = Saf.resolveChild(context, tree, setOf("romfs"))
            val scan = Saf.scanTree(
                context = context,
                root = root,
                maxFiles = 200_000,
                maxDepth = 24,
                sampleExtensions = setOf("kps", "spv", "bfres"),
                maxSamples = 96,
                prefixBytes = 16,
            )
            val packages = scan.samples.map { sample ->
                val extension = Saf.extensionOf(sample.name)
                val magic = sample.prefix.joinToString("") { "%02x".format(it) }
                val kind = when (extension) {
                    "kps" -> "kps"
                    "spv" -> "spirv"
                    "bfres" -> "bfres"
                    else -> "other"
                }
                ShaderPackageInfo(
                    name = sample.name,
                    kind = kind,
                    size = sample.size,
                    magic = magic,
                    validSpirv = false,
                    diagnostic = when (kind) {
                        "kps" -> "_SPK4100 package; Maxwell bytecode translation is not implemented"
                        "bfres" -> "Nintendo resource container; shader extraction is not implemented"
                        else -> null
                    },
                )
            }
            ShaderInfo(
                valid = scan.error == null && scan.files > 0,
                files = scan.files,
                spirvFiles = scan.extensionCounts["spv"] ?: 0,
                kpsFiles = scan.extensionCounts["kps"] ?: 0,
                bfresFiles = scan.extensionCounts["bfres"] ?: 0,
                totalBytes = scan.bytes,
                truncated = scan.truncated,
                packages = packages,
                warnings = buildList {
                    if ((scan.extensionCounts["kps"] ?: 0) > 0) {
                        add("KPS shader packages use Maxwell-era GPU programs; they are counted, not translated.")
                    }
                    if (scan.truncated) add("The shader walk reached its file limit.")
                },
                error = scan.error,
            )
        } catch (error: Exception) {
            ShaderInfo(
                false, 0, 0, 0, 0, 0, false, emptyList(), emptyList(),
                error.message ?: error.javaClass.simpleName
            )
        }
    }

    suspend fun buildPipelineCache(): PipelineInfo = withContext(Dispatchers.IO) {
        nativeProblem?.let {
            return@withContext PipelineInfo(
                false, false, false, "", 0, 0, 0, 0, 0, 0, 0, 0, 0, emptyList(), it
            )
        }
        val assetNames = listOf(
            "fsr_easu.spv",
            "fsr_rcas.spv",
            "gsr_upscale.spv",
            "frame_interp.spv",
            "scene_vert.spv",
            "scene_frag.spv",
            "shadow_frag.spv",
        )
        try {
            val shaderDir = File(context.cacheDir, "spirv").apply { mkdirs() }
            val staged = ArrayList<String>()
            for (name in assetNames) {
                val target = File(shaderDir, name)
                context.assets.open("shaders/$name").use { input ->
                    target.outputStream().use { output -> input.copyTo(output) }
                }
                val validation = NativeForge.nativeValidateSpirv(target.absolutePath)
                if (validation.contains("\"valid\":true")) {
                    staged.add(target.absolutePath)
                }
            }
            if (staged.isEmpty()) {
                return@withContext PipelineInfo(
                    false, false, false, "", 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    emptyList(), "no bundled SPIR-V module passed validation"
                )
            }
            val output = File(context.filesDir, "gpu/pipeline_cache.bin")
            output.parentFile?.mkdirs()
            parsePipeline(
                NativeForge.nativeBuildPipelineCache(staged.toTypedArray(), output.absolutePath)
            )
        } catch (error: Exception) {
            PipelineInfo(
                false, false, false, "", 0, 0, 0, 0, 0, 0, 0, 0, 0,
                emptyList(), error.message ?: error.javaClass.simpleName
            )
        }
    }

    suspend fun extractNsp(uri: Uri, outputDirectory: File): ExtractInfo =
        withContext(Dispatchers.IO) {
            nativeProblem?.let {
                return@withContext ExtractInfo(false, 0, emptyList(), "", it)
            }
            try {
                val file = Saf.document(context, uri)
                    ?: return@withContext ExtractInfo(
                        false, 0, emptyList(), "", "the NSP is no longer accessible"
                    )
                outputDirectory.mkdirs()
                Saf.withFd(context, file) { fd ->
                    parseExtract(NativeForge.nativeExtractNsp(fd, outputDirectory.absolutePath))
                }
            } catch (error: Exception) {
                ExtractInfo(
                    false, 0, emptyList(), "",
                    error.message ?: error.javaClass.simpleName
                )
            }
        }

    fun findMainNso(uri: Uri): DocumentFile? {
        val tree = Saf.tree(context, uri) ?: return null
        val root = Saf.resolveChild(context, tree, setOf("exefs"))
        val files = Saf.collectFiles(root, maxFiles = 2048, maxDepth = 8)
        return files.firstOrNull { (it.name ?: "").equals("main", true) }
            ?: files.firstOrNull { (it.name ?: "").equals("main.nso", true) }
    }
}
