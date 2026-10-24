package com.pes2017.recomp

import android.content.Context
import android.net.Uri
import org.json.JSONArray
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * Fetches the Turnip build that suits this phone's GPU, so nobody has to find
 * and import a zip by hand. Only Adreno has one: Mali and the rest run the
 * phone's own driver.
 *
 * The package comes from a public release feed, is checked and unpacked by
 * DriverStore like any imported zip, and is never selected here: the player
 * decides whether to run it.
 */
object DriverDownloader {

    /** Which feed and which asset suit an Adreno generation (6xx, 7xx, 8xx). */
    private data class Source(val api: String, val asset: Regex, val pinned: String)

    private val GEN6_7 = Source(
        "https://api.github.com/repos/K11MCH1/AdrenoToolsDrivers/releases?per_page=30",
        Regex("^Turnip_v[0-9.]+_R[0-9]+\\.zip$"),
        "https://github.com/K11MCH1/AdrenoToolsDrivers/releases/download/v26.0.0-rc08/Turnip_v26.0.0_R8.zip",
    )
    private val GEN8 = Source(
        "https://api.github.com/repos/StevenMXZ/freedreno_turnip-CI/releases?per_page=30",
        Regex("^Turnip_Gen8_V[0-9]+\\.zip$"),
        "https://github.com/StevenMXZ/freedreno_turnip-CI/releases/download/v27.0.0-R1/Turnip_Gen8_V38.zip",
    )

    /** One unified build for 6xx, 7xx and 8xx, from the MrPurple feed. */
    private val PURPLE = Source(
        "https://api.github.com/repos/MrPurple666/purple-turnip/releases?per_page=30",
        Regex("^turnip_mrpurple_T[0-9]+.*\\.zip$"),
        "https://github.com/MrPurple666/purple-turnip/releases/download/vturnip_mrpurple_T30-toasted.adpkg/turnip_mrpurple_T30-toasted.adpkg.zip",
    )

    enum class Choice { RECOMMENDED, PURPLE }

    /** The Adreno generation in a GL renderer string ("Adreno (TM) 830" is 8), or 0. */
    fun adrenoGeneration(renderer: String): Int {
        val model = Regex("adreno[^0-9]*(\\d{3})").find(renderer.lowercase())
            ?.groupValues?.get(1)?.toInt() ?: return 0
        return model / 100
    }

    /** Whether a driver is on offer for this phone at all. */
    fun suitable(renderer: String): Boolean = adrenoGeneration(renderer) in 6..8

    sealed class Result {
        data class Done(val driver: DriverStore.Driver) : Result()
        data class Failed(val reason: String) : Result()
    }

    /** Blocking: call from a worker thread. */
    fun download(
        context: Context, renderer: String, store: DriverStore, choice: Choice = Choice.RECOMMENDED,
    ): Result {
        val generation = adrenoGeneration(renderer)
        if (generation !in 6..8) return Result.Failed(context.getString(R.string.driver_none_for_gpu))
        val source = when {
            choice == Choice.PURPLE -> PURPLE
            generation == 8 -> GEN8
            else -> GEN6_7
        }
        val temp = File(context.cacheDir, "driver-download.zip")
        try {
            // The feed is unauthenticated and rate limited per address, so a
            // pinned link stands in when it will not answer.
            val url = try { findAsset(source) } catch (_: Throwable) { null } ?: source.pinned
            val connection = (URL(url).openConnection() as HttpURLConnection).apply {
                connectTimeout = 15000
                readTimeout = 30000
                instanceFollowRedirects = true
            }
            if (connection.responseCode != 200) {
                return Result.Failed(context.getString(R.string.driver_download_failed))
            }
            connection.inputStream.use { input -> temp.outputStream().use { input.copyTo(it) } }
            return when (val outcome = store.import(Uri.fromFile(temp), "turnip")) {
                is DriverStore.Outcome.Imported -> Result.Done(outcome.driver)
                is DriverStore.Outcome.Failed -> Result.Failed(outcome.reason)
            }
        } catch (error: Throwable) {
            return Result.Failed(error.message ?: context.getString(R.string.driver_download_failed))
        } finally {
            temp.delete()
        }
    }

    /** The newest release carrying a plain build for this generation. */
    private fun findAsset(source: Source): String? {
        val connection = (URL(source.api).openConnection() as HttpURLConnection).apply {
            connectTimeout = 15000
            readTimeout = 15000
            setRequestProperty("Accept", "application/vnd.github+json")
        }
        if (connection.responseCode != 200) return null
        val releases = JSONArray(connection.inputStream.bufferedReader().readText())
        for (i in 0 until releases.length()) {
            val release = releases.getJSONObject(i)
            if (release.optBoolean("draft")) continue
            val assets = release.optJSONArray("assets") ?: continue
            for (j in 0 until assets.length()) {
                val asset = assets.getJSONObject(j)
                if (source.asset.matches(asset.optString("name"))) {
                    return asset.optString("browser_download_url")
                }
            }
        }
        return null
    }
}
