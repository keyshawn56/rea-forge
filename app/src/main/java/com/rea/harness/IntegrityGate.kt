package com.rea.harness

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.security.MessageDigest

/**
 * Result of the directory-level boot gate. [Ready] means the three required
 * files exist, both payload headers are structurally valid, and both payloads
 * match the user-supplied SHA-256 manifest.
 */
sealed class GateResult {
    data class Ready(
        val elfUri: Uri,
        val pakUri: Uri,
        val elfSha256: String,
        val pakSha256: String,
        val elfBytes: Long,
        val pakBytes: Long,
        val manifestName: String
    ) : GateResult()

    data class Rejected(val reason: String) : GateResult()
}

/**
 * Implements the SAF gate documented in SCOPE.md.
 *
 * This is an integrity/consistency check, not an authenticity check: the
 * manifest is supplied by the same user who supplies the payloads, so an
 * attacker who can replace both can satisfy it.
 */
object IntegrityGate {
    private const val MAX_MANIFEST_BYTES = 1L shl 20
    private const val MAX_PAK_ENTRIES = 65536L
    private const val ELF_HEADER_SIZE = 64
    private const val ELF_PROGRAM_HEADER_SIZE = 56
    private const val PAK_HEADER_SIZE = 32
    private const val PAK_INDEX_STRIDE = 96L

    private val elfNames = listOf("main.elf")
    private val pakNames = listOf("assets.pak")
    private val manifestNames = listOf("manifest.sha256", "integrity.sha256", "assets.sha256")
    private val digestLine = Regex("^([0-9a-fA-F]{64})[ \\t]+\\*?(.+?)\\s*$")

    suspend fun validate(context: Context, treeUri: Uri): GateResult = withContext(Dispatchers.IO) {
        try {
            validateBlocking(context, treeUri)
        } catch (error: Exception) {
            GateResult.Rejected("Validation failed: ${error.message ?: error.javaClass.simpleName}")
        }
    }

    private fun validateBlocking(context: Context, treeUri: Uri): GateResult {
        LogBus.append(LogBus.INFO, "gate", "validating tree $treeUri")
        val tree = DocumentFile.fromTreeUri(context, treeUri)
            ?: return reject("The selected folder is not accessible through SAF.")

        if (!tree.canRead()) {
            return reject("The selected folder is not readable. Pick it again.")
        }

        val manifestFile = findDocument(tree, manifestNames)
            ?: return reject("Missing manifest.sha256 (also accepted: integrity.sha256, assets.sha256).")
        val elfFile = findDocument(tree, elfNames)
            ?: return reject("Missing main.elf in the selected folder.")
        val pakFile = findDocument(tree, pakNames)
            ?: return reject("Missing assets.pak in the selected folder.")

        val manifestName = manifestFile.name ?: "manifest.sha256"
        val manifest = try {
            parseManifest(readText(context, manifestFile, MAX_MANIFEST_BYTES))
        } catch (error: Exception) {
            return reject("Cannot read $manifestName: ${error.message}")
        }

        val elfDigest = manifest["main.elf"]
            ?: return reject("$manifestName has no SHA-256 entry for main.elf.")
        val pakDigest = manifest["assets.pak"]
            ?: return reject("$manifestName has no SHA-256 entry for assets.pak.")

        // 0 means "provider did not report a size"; the header/truncation checks
        // below still reject truly empty files.
        val elfSize = knownSize(context, elfFile)
        val pakSize = knownSize(context, pakFile)

        validateElf(context, elfFile, elfSize)?.let { return reject(it) }
        LogBus.append(LogBus.INFO, "gate", "main.elf header ok (${formatBytes(elfSize)})")

        validatePak(context, pakFile, pakSize)?.let { return reject(it) }
        LogBus.append(LogBus.INFO, "gate", "assets.pak index ok (${formatBytes(pakSize)})")

        LogBus.append(LogBus.INFO, "gate", "hashing main.elf")
        val (elfHash, elfBytes) = hashUri(context, elfFile.uri)
        if (elfHash != elfDigest) {
            return reject("main.elf SHA-256 mismatch.\nmanifest: $elfDigest\nfile:     $elfHash")
        }

        LogBus.append(LogBus.INFO, "gate", "hashing assets.pak")
        val (pakHash, pakBytes) = hashUri(context, pakFile.uri)
        if (pakHash != pakDigest) {
            return reject("assets.pak SHA-256 mismatch.\nmanifest: $pakDigest\nfile:     $pakHash")
        }

        LogBus.append(LogBus.INFO, "gate", "digest manifest verified ($manifestName)")
        return GateResult.Ready(
            elfUri = elfFile.uri,
            pakUri = pakFile.uri,
            elfSha256 = elfHash,
            pakSha256 = pakHash,
            elfBytes = elfBytes,
            pakBytes = pakBytes,
            manifestName = manifestName
        )
    }

    private fun reject(reason: String): GateResult.Rejected {
        LogBus.append(LogBus.ERROR, "gate", reason.replace('\n', ' '))
        return GateResult.Rejected(reason)
    }

    private fun findDocument(tree: DocumentFile, names: List<String>): DocumentFile? {
        for (name in names) {
            val direct = tree.findFile(name)
            if (direct != null && direct.isFile) {
                return direct
            }
        }
        val wanted = names.map { it.lowercase() }.toSet()
        val children = try {
            tree.listFiles()
        } catch (_: Exception) {
            return null
        }
        return children.firstOrNull { child ->
            child.isFile && child.name?.lowercase() in wanted
        }
    }

    private fun parseManifest(text: String): Map<String, String> {
        if (text.indexOf('\u0000') >= 0) {
            throw IllegalArgumentException("manifest contains NUL bytes")
        }
        val result = HashMap<String, String>()
        var accepted = 0
        for (rawLine in text.lineSequence()) {
            val line = rawLine.trim()
            if (line.isEmpty() || line.startsWith("#")) {
                continue
            }
            val match = digestLine.matchEntire(line)
                ?: throw IllegalArgumentException("malformed line: ${line.take(80)}")
            val digest = match.groupValues[1].lowercase()
            val rawName = match.groupValues[2].trim()
            val name = rawName.substringAfterLast('/').substringAfterLast('\\').lowercase()
            if (name.isEmpty()) {
                throw IllegalArgumentException("manifest entry has no file name")
            }
            val previous = result.put(name, digest)
            if (previous != null && previous != digest) {
                throw IllegalArgumentException("conflicting digests for $name")
            }
            accepted++
        }
        if (accepted == 0) {
            throw IllegalArgumentException("manifest has no sha256sum entries")
        }
        return result
    }

    private fun validateElf(context: Context, file: DocumentFile, sizeHint: Long): String? {
        val header = readPrefix(context, file.uri, ELF_HEADER_SIZE)
            ?: return "main.elf is not readable."
        if (header.size < ELF_HEADER_SIZE) {
            return "main.elf is truncated: ${header.size} bytes, needs a 64-byte ELF64 header."
        }
        if (header[0] != 0x7F.toByte() || header[1] != 'E'.code.toByte() ||
            header[2] != 'L'.code.toByte() || header[3] != 'F'.code.toByte()
        ) {
            return "main.elf does not start with the ELF magic."
        }
        if (header[4].toInt() != 2) {
            return "main.elf is not ELF64 (EI_CLASS=${header[4]})."
        }
        if (header[5].toInt() != 1) {
            return "main.elf is not little-endian (EI_DATA=${header[5]})."
        }
        if (header[6].toInt() != 1) {
            return "main.elf has an unsupported ELF version."
        }
        val type = u16(header, 16)
        if (type != 2 && type != 3) {
            return "main.elf has e_type=$type; only ET_EXEC (2) and ET_DYN (3) are accepted."
        }
        val machine = u16(header, 18)
        if (machine != 0xB7) {
            return "main.elf has e_machine=$machine; expected AArch64 (0xB7)."
        }
        if (u32(header, 20) != 1L) {
            return "main.elf has an unsupported e_version."
        }
        val entrySize = u16(header, 52)
        if (entrySize != ELF_HEADER_SIZE) {
            return "main.elf has e_ehsize=$entrySize; expected 64."
        }
        val programHeaderSize = u16(header, 54)
        val programHeaderCount = u16(header, 56)
        if (programHeaderSize != ELF_PROGRAM_HEADER_SIZE) {
            return "main.elf has e_phentsize=$programHeaderSize; expected 56."
        }
        if (programHeaderCount <= 0) {
            return "main.elf has no program headers."
        }
        if (programHeaderCount > 64) {
            return "main.elf has an unreasonable program-header count ($programHeaderCount)."
        }

        val programHeaderOffset = u64(header, 32) ?: return "main.elf has an unreadable e_phoff."
        if (programHeaderOffset < 0) {
            return "main.elf has a negative e_phoff."
        }
        val programHeaderBytes = programHeaderCount.toLong() * ELF_PROGRAM_HEADER_SIZE
        if (sizeHint > 0 && programHeaderOffset + programHeaderBytes > sizeHint) {
            return "main.elf program headers extend past end of file."
        }

        val table = readRange(context, file.uri, programHeaderOffset, programHeaderBytes.toInt())
            ?: return "main.elf program headers are not readable."
        if (table.size < programHeaderBytes) {
            return "main.elf is truncated inside its program-header table."
        }

        var loadSegments = 0
        for (index in 0 until programHeaderCount) {
            val base = index * ELF_PROGRAM_HEADER_SIZE
            val typeValue = u32(table, base)
            when (typeValue) {
                1L -> {
                    val fileOffset = u64(table, base + 8) ?: return "main.elf PT_LOAD has a bad p_offset."
                    val fileSize = u64(table, base + 32) ?: return "main.elf PT_LOAD has a bad p_filesz."
                    val memorySize = u64(table, base + 40) ?: return "main.elf PT_LOAD has a bad p_memsz."
                    if (fileOffset < 0 || fileSize < 0 || memorySize < 0) {
                        return "main.elf PT_LOAD has a negative size or offset."
                    }
                    if (fileSize > memorySize) {
                        return "main.elf PT_LOAD has p_filesz > p_memsz."
                    }
                    if (sizeHint > 0 && fileOffset + fileSize > sizeHint) {
                        return "main.elf PT_LOAD extends past end of file (segment $index)."
                    }
                    loadSegments++
                }
                3L -> return "main.elf requests PT_INTERP; the harness only hosts self-contained images."
                2L -> {
                    val dynamicSize = u64(table, base + 32) ?: 0L
                    if (dynamicSize <= 0) {
                        return "main.elf has an empty PT_DYNAMIC segment."
                    }
                }
            }
        }
        if (loadSegments == 0) {
            return "main.elf has no PT_LOAD segments."
        }
        return null
    }

    private fun validatePak(context: Context, file: DocumentFile, sizeHint: Long): String? {
        val header = readPrefix(context, file.uri, PAK_HEADER_SIZE)
            ?: return "assets.pak is not readable."
        if (header.size < PAK_HEADER_SIZE) {
            return "assets.pak is truncated: ${header.size} bytes, needs a 32-byte header."
        }
        if (header[0] != 'R'.code.toByte() || header[1] != 'P'.code.toByte() ||
            header[2] != 'A'.code.toByte() || header[3] != 'K'.code.toByte()
        ) {
            return "assets.pak does not start with the RPAK magic."
        }
        if (u32(header, 4) != 1L) {
            return "assets.pak uses unsupported version ${u32(header, 4)}."
        }
        if (u32(header, 8) != 0L) {
            return "assets.pak declares unsupported flags (${u32(header, 8)})."
        }
        val entryCount = u32(header, 12)
        if (entryCount <= 0L || entryCount > MAX_PAK_ENTRIES) {
            return "assets.pak has an invalid entry count ($entryCount)."
        }
        val indexOffset = u64(header, 16) ?: return "assets.pak has an unreadable index offset."
        if (indexOffset < PAK_HEADER_SIZE) {
            return "assets.pak index overlaps the header."
        }
        val indexBytes = entryCount * PAK_INDEX_STRIDE
        if (sizeHint > 0 && indexOffset + indexBytes > sizeHint) {
            return "assets.pak index extends past end of file."
        }
        if (indexBytes > Int.MAX_VALUE) {
            return "assets.pak index is too large to validate."
        }

        val index = readRange(context, file.uri, indexOffset, indexBytes.toInt())
            ?: return "assets.pak index is not readable."
        if (index.size < indexBytes) {
            return "assets.pak is truncated inside its index."
        }
        val names = HashSet<String>()
        for (entry in 0 until entryCount.toInt()) {
            val base = entry * PAK_INDEX_STRIDE.toInt()
            val nameBytes = index.copyOfRange(base, base + 64)
            val terminator = nameBytes.indexOf(0)
            if (terminator <= 0) {
                return "assets.pak entry $entry has an empty or unterminated name."
            }
            val name = String(nameBytes, 0, terminator, Charsets.UTF_8)
            if (!names.add(name)) {
                return "assets.pak contains a duplicate entry name: $name"
            }
            val offset = u64(index, base + 64) ?: return "assets.pak entry $name has a bad offset."
            val stored = u64(index, base + 72) ?: return "assets.pak entry $name has a bad stored size."
            val original = u64(index, base + 80) ?: return "assets.pak entry $name has a bad decoded size."
            val codec = u32(index, base + 88)
            if (offset < PAK_HEADER_SIZE || stored < 0 || original < 0 || offset < 0) {
                return "assets.pak entry $name has invalid bounds."
            }
            if (sizeHint > 0 && offset + stored > sizeHint) {
                return "assets.pak entry $name extends past end of file."
            }
            if (codec == 0L) {
                if (stored != original) {
                    return "assets.pak entry $name is raw but stored/decoded sizes differ."
                }
            } else if (codec != 1L) {
                return "assets.pak entry $name uses unknown codec $codec."
            }
        }
        return null
    }

    private fun knownSize(context: Context, file: DocumentFile): Long {
        val reported = file.length()
        if (reported > 0) {
            return reported
        }
        return try {
            context.contentResolver.openFileDescriptor(file.uri, "r")?.use { descriptor ->
                descriptor.statSize.takeIf { it > 0 } ?: 0L
            } ?: 0L
        } catch (_: Exception) {
            0L
        }
    }

    private fun hashUri(context: Context, uri: Uri): Pair<String, Long> {
        val digest = MessageDigest.getInstance("SHA-256")
        val stream = context.contentResolver.openInputStream(uri)
            ?: throw IOException("openInputStream returned null")
        stream.use { input ->
            val buffer = ByteArray(1 shl 20)
            var total = 0L
            while (true) {
                val count = input.read(buffer)
                if (count < 0) {
                    break
                }
                if (count == 0) {
                    continue
                }
                digest.update(buffer, 0, count)
                total += count
            }
            return hex(digest.digest()) to total
        }
    }

    private fun readText(context: Context, file: DocumentFile, limit: Long): String {
        if (file.length() > limit) {
            throw IOException("manifest is larger than ${formatBytes(limit)}")
        }
        val stream = context.contentResolver.openInputStream(file.uri)
            ?: throw IOException("openInputStream returned null")
        stream.use { input ->
            val output = ByteArrayOutputStream()
            val buffer = ByteArray(8192)
            var total = 0L
            while (true) {
                val count = input.read(buffer)
                if (count < 0) {
                    break
                }
                if (count == 0) {
                    continue
                }
                total += count
                if (total > limit) {
                    throw IOException("manifest is larger than ${formatBytes(limit)}")
                }
                output.write(buffer, 0, count)
            }
            return output.toString("UTF-8")
        }
    }

    private fun readPrefix(context: Context, uri: Uri, count: Int): ByteArray? =
        readRange(context, uri, 0, count)

    private fun readRange(context: Context, uri: Uri, offset: Long, count: Int): ByteArray? {
        val stream = context.contentResolver.openInputStream(uri) ?: return null
        stream.use { input ->
            var skipped = 0L
            while (skipped < offset) {
                val step = input.skip(offset - skipped)
                if (step <= 0) {
                    // Not all providers support skip efficiently; fall back to read.
                    if (input.read() < 0) {
                        return null
                    }
                    skipped += 1
                } else {
                    skipped += step
                }
            }
            val buffer = ByteArray(count)
            var total = 0
            while (total < count) {
                val got = input.read(buffer, total, count - total)
                if (got < 0) {
                    break
                }
                total += got
            }
            return if (total == count) buffer else buffer.copyOf(total)
        }
    }

    private fun u16(data: ByteArray, offset: Int): Int =
        (data[offset].toInt() and 0xFF) or ((data[offset + 1].toInt() and 0xFF) shl 8)

    private fun u32(data: ByteArray, offset: Int): Long =
        (data[offset].toLong() and 0xFF) or
            ((data[offset + 1].toLong() and 0xFF) shl 8) or
            ((data[offset + 2].toLong() and 0xFF) shl 16) or
            ((data[offset + 3].toLong() and 0xFF) shl 24)

    private fun u64(data: ByteArray, offset: Int): Long? {
        var value = 0L
        for (index in 0 until 8) {
            value = value or ((data[offset + index].toLong() and 0xFF) shl (index * 8))
        }
        if (value < 0) {
            return null
        }
        return value
    }

    private fun hex(bytes: ByteArray): String {
        val digits = "0123456789abcdef"
        val output = StringBuilder(bytes.size * 2)
        for (byte in bytes) {
            val value = byte.toInt() and 0xFF
            output.append(digits[value ushr 4])
            output.append(digits[value and 0x0F])
        }
        return output.toString()
    }

    fun formatBytes(bytes: Long): String {
        if (bytes < 0) {
            return "unknown size"
        }
        if (bytes < 1024) {
            return "$bytes B"
        }
        val units = arrayOf("KiB", "MiB", "GiB", "TiB")
        var value = bytes.toDouble()
        var unit = -1
        while (value >= 1024.0 && unit < units.lastIndex) {
            value /= 1024.0
            unit++
        }
        return String.format("%.1f %s", value, units[unit])
    }
}
