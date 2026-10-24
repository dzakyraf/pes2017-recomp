package com.pes2017.recomp

import android.content.Context
import android.content.Intent
import androidx.core.content.FileProvider
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * The captures CaptureSession writes (files/captures/<stamp>-<name>/): listing them, zipping one,
 * sharing it, saving it to Downloads, and deleting. Used by CapturesActivity in the
 * launcher and by the share button after a capture.
 */
object Captures {

    class Item(val folder: File, val bytes: Long, val files: Int) {
        val name: String get() = folder.name
    }

    fun directory(context: Context): File =
        File(context.getExternalFilesDir(null) ?: context.filesDir, "captures")

    /** Newest first; the folder a running capture writes into is not one yet. */
    fun list(context: Context): List<Item> =
        (directory(context).listFiles() ?: emptyArray())
            .filter { it.isDirectory && it.name != "pending" }
            .sortedByDescending { it.name }
            .map { dir ->
                val files = dir.walkTopDown().filter { it.isFile }.toList()
                Item(dir, files.sumOf { it.length() }, files.size)
            }

    /** The zip beside the folder (frames included), made again each time. Null when the folder is gone. */
    fun zip(folder: File): File? {
        if (!folder.isDirectory) return null
        val zip = File(folder.parentFile, folder.name + ".zip")
        ZipOutputStream(zip.outputStream().buffered()).use { out ->
            folder.walkTopDown().filter { it.isFile }.forEach { file ->
                out.putNextEntry(ZipEntry(folder.name + "/" + file.relativeTo(folder).path))
                file.inputStream().use { it.copyTo(out) }
                out.closeEntry()
            }
        }
        return zip
    }

    /** The share sheet with the zip attached (a chat, mail or a drive). */
    fun share(context: Context, folder: File): Boolean {
        val zip = try { zip(folder) } catch (_: Throwable) { null } ?: return false
        val uri = FileProvider.getUriForFile(context, "${context.packageName}.captures", zip)
        val send = Intent(Intent.ACTION_SEND).apply {
            type = "application/zip"
            putExtra(Intent.EXTRA_STREAM, uri)
            putExtra(Intent.EXTRA_SUBJECT, "PES17 capture ${folder.name}")
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        context.startActivity(Intent.createChooser(send, context.getString(R.string.capture_share)).apply {
            if (context !is android.app.Activity) addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        })
        return true
    }

    /**
     * Copies the capture's zip to the phone's Downloads folder (Download/PES17/ on Android 10+,
     * through MediaStore, which needs no permission). Returns where it went, or null on failure.
     */
    fun saveToDownloads(context: Context, folder: File): String? {
    return try {
        val zip = zip(folder) ?: return null
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.Q) {
            val values = android.content.ContentValues().apply {
                put(android.provider.MediaStore.Downloads.DISPLAY_NAME, zip.name)
                put(android.provider.MediaStore.Downloads.MIME_TYPE, "application/zip")
                put(android.provider.MediaStore.Downloads.RELATIVE_PATH, "Download/PES17")
            }
            val resolver = context.contentResolver
            val uri = resolver.insert(android.provider.MediaStore.Downloads.EXTERNAL_CONTENT_URI, values) ?: return null
            resolver.openOutputStream(uri)?.use { out -> zip.inputStream().use { it.copyTo(out) } } ?: return null
            "Download/PES17/${zip.name}"
        } else {
            val target = File(android.os.Environment.getExternalStoragePublicDirectory(
                android.os.Environment.DIRECTORY_DOWNLOADS), "PES17").apply { mkdirs() }
            zip.copyTo(File(target, zip.name), overwrite = true)
            "Download/PES17/${zip.name}"
        }
    } catch (_: Throwable) {
        null
    }
    }

    /** The folder and the zip made from it. */
    fun delete(folder: File) {
        try {
            folder.deleteRecursively()
            File(folder.parentFile, folder.name + ".zip").delete()
        } catch (_: Throwable) {
        }
    }

    fun deleteAll(context: Context) {
        val root = directory(context)
        (root.listFiles() ?: emptyArray()).forEach { it.deleteRecursively() }
    }

    /** Zips are only for sending; they are removed once the screen is left. */
    fun deleteZips(context: Context) {
        (directory(context).listFiles() ?: emptyArray()).filter { it.isFile && it.extension == "zip" }.forEach { it.delete() }
    }

    fun size(bytes: Long): String = when {
        bytes >= 1 shl 20 -> "%.1f MB".format(java.util.Locale.US, bytes / 1048576.0)
        bytes >= 1 shl 10 -> "%d KB".format(bytes shr 10)
        else -> "$bytes B"
    }
}
