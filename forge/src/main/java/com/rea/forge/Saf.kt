package com.rea.forge

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import java.io.FileNotFoundException
import java.io.IOException
import java.util.ArrayDeque

data class SafSample(
    val name: String,
    val size: Long,
    val prefix: ByteArray,
)

data class TreeScan(
    val files: Long,
    val directories: Long,
    val bytes: Long,
    val topLevel: List<String>,
    val truncated: Boolean,
    val extensionCounts: Map<String, Long>,
    val samples: List<SafSample>,
    val error: String? = null,
)

/**
 * SAF traversal helpers. Everything here is read-only: the selected tree is
 * never copied or modified. Header reads use openInputStream and bounded
 * prefixes so cloud-backed providers are not forced to download whole files.
 */
object Saf {
    fun tree(context: Context, uri: Uri): DocumentFile? {
        val tree = DocumentFile.fromTreeUri(context, uri) ?: return null
        return if (tree.canRead()) tree else null
    }

    fun document(context: Context, uri: Uri): DocumentFile? {
        val single = DocumentFile.fromSingleUri(context, uri)
        if (single != null && single.canRead()) return single
        return tree(context, uri)
    }

    fun resolveChild(context: Context, root: DocumentFile, names: Set<String>): DocumentFile {
        val wanted = names.map { it.lowercase() }.toSet()
        return root.listFiles().firstOrNull { file ->
            file.isDirectory && (file.name ?: "").lowercase() in wanted
        } ?: root
    }

    fun collectFiles(root: DocumentFile, maxFiles: Int = 4096, maxDepth: Int = 8): List<DocumentFile> {
        val output = ArrayList<DocumentFile>()
        val queue = ArrayDeque<Pair<DocumentFile, Int>>()
        queue.add(root to 0)
        while (queue.isNotEmpty() && output.size < maxFiles) {
            val (node, depth) = queue.removeFirst()
            val children = try {
                node.listFiles()
            } catch (_: Exception) {
                continue
            }
            for (child in children) {
                if (child.isDirectory) {
                    if (depth < maxDepth) queue.add(child to depth + 1)
                } else if (child.isFile) {
                    output.add(child)
                    if (output.size >= maxFiles) break
                }
            }
        }
        return output
    }

    fun scanTree(
        context: Context,
        root: DocumentFile,
        maxFiles: Long = 100_000,
        maxDepth: Int = 16,
        sampleExtensions: Set<String> = emptySet(),
        maxSamples: Int = 0,
        prefixBytes: Int = 16,
    ): TreeScan {
        var files = 0L
        var directories = 0L
        var bytes = 0L
        var truncated = false
        val topLevel = ArrayList<String>()
        val extensions = HashMap<String, Long>()
        val samples = ArrayList<SafSample>()

        try {
            root.listFiles().forEach { child ->
                val name = child.name ?: return@forEach
                if (topLevel.size < 128) topLevel.add(name)
            }
        } catch (_: Exception) {
            // A provider can reject listing the root; the queue below reports it.
        }

        val queue = ArrayDeque<Pair<DocumentFile, Int>>()
        queue.add(root to 0)
        while (queue.isNotEmpty()) {
            if (files >= maxFiles) {
                truncated = true
                break
            }
            val (node, depth) = queue.removeFirst()
            val children = try {
                node.listFiles()
            } catch (error: Exception) {
                return TreeScan(
                    files, directories, bytes, topLevel.sorted(), truncated, extensions, samples,
                    error.message ?: "SAF listing failed"
                )
            }
            for (child in children) {
                val name = child.name ?: continue
                if (child.isDirectory) {
                    ++directories
                    if (depth < maxDepth) queue.add(child to depth + 1)
                } else if (child.isFile) {
                    ++files
                    val size = child.length().coerceAtLeast(0)
                    bytes += size
                    val extension = extensionOf(name)
                    extensions[extension] = (extensions[extension] ?: 0) + 1
                    if (samples.size < maxSamples && extension in sampleExtensions) {
                        val prefix = try {
                            readPrefix(context, child, prefixBytes)
                        } catch (_: Exception) {
                            ByteArray(0)
                        }
                        samples.add(SafSample(name, size, prefix))
                    }
                    if (files >= maxFiles) {
                        truncated = true
                        break
                    }
                }
            }
        }
        topLevel.sort()
        return TreeScan(
            files = files,
            directories = directories,
            bytes = bytes,
            topLevel = topLevel.distinct(),
            truncated = truncated,
            extensionCounts = extensions,
            samples = samples,
        )
    }

    fun <T> withFd(context: Context, file: DocumentFile, block: (Int) -> T): T {
        val descriptor = context.contentResolver.openFileDescriptor(file.uri, "r")
            ?: throw FileNotFoundException("cannot open ${file.name ?: file.uri}")
        descriptor.use {
            return block(it.fd)
        }
    }

    fun readPrefix(context: Context, file: DocumentFile, maxBytes: Int): ByteArray {
        val input = context.contentResolver.openInputStream(file.uri)
            ?: throw IOException("cannot open ${file.name ?: file.uri}")
        input.use { stream ->
            val output = ByteArray(maxBytes)
            var total = 0
            while (total < maxBytes) {
                val read = stream.read(output, total, maxBytes - total)
                if (read <= 0) break
                total += read
            }
            return output.copyOf(total)
        }
    }

    fun extensionOf(name: String): String {
        val dot = name.lastIndexOf('.')
        if (dot <= 0 || dot + 1 >= name.length) return "(none)"
        return name.substring(dot + 1).lowercase().take(12)
    }
}
