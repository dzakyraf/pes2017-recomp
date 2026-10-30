package com.pes2017.recomp

import android.app.ActivityManager
import android.content.Context
import android.opengl.EGL14
import android.opengl.GLES20
import android.os.Build
import java.io.File

/**
 * What this phone can carry, as one of three tiers, and the settings each tier
 * starts from. The match is GPU-bound first (docs/performance.md), so the GPU
 * decides the tier; the CPU and the memory can only pull it down.
 *
 *   High    the Snapdragon 865's Adreno 650 and better: the defaults the port was tuned on.
 *   Medium  a step below (Adreno 640/642L/7xx entry, Mali-G68/G78/G610):
 *           a smaller scene with FSR to sharpen it back.
 *   Low     Helio G99 / Snapdragon 680-class (Mali-G57 MC2, Adreno 610-619):
 *           the smallest scene, no multisampling, and 30 frames a second.
 *           Medium is held to 45: steady frame times on a 60 Hz screen, and a lighter
 *           load than 60 (measured on a Snapdragon 865, which is High: 45 and 30 had the
 *           steadiest frames, 40 and 35 hitched).
 *
 * Nothing here is measured on a Low or Medium phone yet; the table is the
 * GPUs' relative throughput, not a benchmark.
 */
object DeviceProfile {

    enum class Tier { LOW, MEDIUM, HIGH }

    /** What the classifier saw, for the Settings line and the capture summary. */
    data class Specs(
        val gpu: String,
        val soc: String,
        val totalMemoryMB: Long,
        val fastCores: Int,
        val fastestMHz: Int,
    )

    data class Result(val tier: Tier, val specs: Specs, val reason: String)

    /** The settings a tier starts from; everything else keeps its own default. */
    data class Preset(
        val sceneScalePercent: Int,
        val multisampling: Boolean,
        val fsr: Boolean,
        val fpsLimit: Int,
        val lowMemory: Boolean,
        val heatScale: Boolean,
        val loadScale: Boolean,
    )

    // With the heat rule on, the scene scale is the most the match will be drawn at and the
    // runtime lowers it (to 50%) as the phone heats, so Medium and Low start higher than they
    // would be fixed. The heat rule is on everywhere: on a Snapdragon 865 it dropped the picture and
    // held 57-60 fps in a match at thermal status 4. The load rule was left out of every preset
    // because on that phone the GPU pass time never went over budget in a slow match, so it never
    // acted; a Mali-G52 in Low has no such headroom, and the rule is the quickest way to keep the
    // frame time steady when it does not.
    //
    // lowMemory is off in every preset: the runtime already turns the vertex shadow off by
    // itself when MemTotal is 6 GB or less (perf.cpp), and forcing it from the Low tier cost
    // CPU on 8 GB phones that have the RAM to spare.
    fun preset(tier: Tier): Preset = when (tier) {
        Tier.HIGH -> Preset(75, multisampling = true, fsr = false, fpsLimit = 60, lowMemory = false, heatScale = true, loadScale = false)
        Tier.MEDIUM -> Preset(75, multisampling = true, fsr = true, fpsLimit = 60, lowMemory = false, heatScale = true, loadScale = false)
        Tier.LOW -> Preset(60, multisampling = false, fsr = true, fpsLimit = 30, lowMemory = false, heatScale = true, loadScale = true)
    }

    fun apply(prefs: Prefs, tier: Tier) {
        val p = preset(tier)
        prefs.sceneScalePercent = p.sceneScalePercent
        prefs.multisampling = p.multisampling
        prefs.fsr = p.fsr
        prefs.fpsLimit = p.fpsLimit
        prefs.lowMemory = p.lowMemory
        prefs.heatScale = p.heatScale
        prefs.loadScale = p.loadScale
        prefs.preset = tier.name
    }

    /**
     * The first time a version with presets runs: a player who never touched the
     * performance settings gets the detected tier; one who did is left as they
     * are, marked custom.
     */
    fun ensurePreset(context: Context, prefs: Prefs) {
        if (prefs.preset.isNotEmpty()) return
        if (prefs.hasTunedPerformance()) {
            prefs.preset = PRESET_CUSTOM
            return
        }
        apply(prefs, detect(context, prefs).tier)
    }

    fun detect(context: Context, prefs: Prefs): Result {
        val specs = Specs(
            gpu = gpuName(prefs),
            soc = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) Build.SOC_MODEL else Build.HARDWARE,
            totalMemoryMB = totalMemoryMB(context),
            fastCores = fastCores(),
            fastestMHz = fastestMHz(),
        )
        return classify(specs)
    }

    /** Pure, so the table can be read and checked without a phone. */
    fun classify(specs: Specs): Result {
        val gpuTier = gpuTier(specs.gpu)
        // Two fast cores is a Helio G99's 2xA76: the guest's main thread and its
        // three spinning workers share them (docs/performance.md, the yield loops).
        val cpuCap = when {
            specs.fastestMHz in 1 until 2000 -> Tier.LOW
            specs.fastCores in 1..2 -> Tier.MEDIUM
            else -> Tier.HIGH
        }
        // The guest's 512 MB bank and the texture cache need room. totalMem is what
        // the kernel has after its own carve-outs: a 4 GB phone reports about
        // 3.6 GB, a 3 GB one about 2.8 GB, a 6 GB one about 5.5 GB.
        val memoryCap = when {
            specs.totalMemoryMB in 1 until 3200 -> Tier.LOW
            specs.totalMemoryMB in 1 until 4600 -> Tier.MEDIUM
            else -> Tier.HIGH
        }
        val gpuOrGuess = gpuTier ?: Tier.MEDIUM
        val tier = minOf(gpuOrGuess, cpuCap, memoryCap)
        val reason = when {
            tier == gpuOrGuess && gpuTier == null -> "GPU not in the table"
            tier == gpuOrGuess -> "GPU"
            tier == cpuCap -> "CPU, ${specs.fastCores} fast cores"
            else -> "memory"
        }
        return Result(tier, specs, reason)
    }

    /** The GPU's tier from its GL renderer string, or null for one not in the table. */
    fun gpuTier(renderer: String): Tier? {
        val name = renderer.lowercase()
        Regex("adreno[^0-9]*(\\d{3})").find(name)?.let {
            val model = it.groupValues[1].toInt()
            return when {
                model >= 730 -> Tier.HIGH              // 8 Gen 1 (730), 8 Gen 2 (740), 8s Gen 3 (735), 8 Gen 3 (750)
                model >= 710 -> Tier.MEDIUM            // 6 Gen 1 / 7s Gen 2 (710), 7 Gen 3 (720), 7+ Gen 2 (725), 7+ Gen 3 (732)
                model >= 650 -> Tier.HIGH              // 865 (650), 888 (660)
                model >= 640 -> Tier.MEDIUM            // 855 (640), 778G (642L), 7 Gen 1 (644)
                else -> Tier.LOW                       // 680 (610), 695 (619), 750G (619) and older
            }
        }
        if (name.contains("immortalis") || name.contains("xclipse")) return Tier.HIGH
        Regex("mali-g(\\d{2,3})").find(name)?.let {
            val model = it.groupValues[1].toInt()
            // Mali-G57/G52 come in two to six cores; the Helio and Dimensity 6xxx parts are MC2/MC3.
            val cores = Regex("mc(\\d+)|mp(\\d+)").find(name)?.groupValues?.drop(1)?.firstOrNull { c -> c.isNotEmpty() }?.toInt() ?: 0
            return when {
                model >= 710 -> Tier.HIGH              // G710, G715, G720, G925 (Dimensity 8100-9400, Tensor G2+)
                model in 610..699 -> Tier.MEDIUM       // G610, G615 (Dimensity 7xxx/8xxx)
                model >= 77 && model < 100 -> Tier.MEDIUM  // G77, G78 (Exynos 990/2100, Tensor G1)
                model == 68 -> Tier.MEDIUM             // Dimensity 1080, 7050 (MC4)
                model == 76 && cores >= 5 -> Tier.MEDIUM   // Kirin 980/990, Exynos 9820 (MP10-MP16)
                else -> Tier.LOW                       // G57 MC2 (Helio G99), G52, G51, G31, G76 MC4
            }
        }
        if (name.contains("powervr")) return Tier.LOW
        return null
    }

    // ---- reading the phone ------------------------------------------------

    /**
     * The GL renderer string ("Adreno (TM) 650", "Mali-G57 MC2"): the same GPU the
     * Vulkan device is, and a pbuffer context is the cheapest way to ask. Read
     * once and kept, keyed by the build, so a driver update reads it again.
     */
    private fun gpuName(prefs: Prefs): String {
        val key = Build.FINGERPRINT
        prefs.cachedGpu(key)?.let { return it }
        val name = try { readGlRenderer() } catch (_: Throwable) { "" }
        if (name.isNotEmpty()) prefs.cacheGpu(key, name)
        return name
    }

    private fun readGlRenderer(): String {
        val display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        if (display == EGL14.EGL_NO_DISPLAY) return ""
        val version = IntArray(2)
        if (!EGL14.eglInitialize(display, version, 0, version, 1)) return ""
        try {
            val configs = arrayOfNulls<android.opengl.EGLConfig>(1)
            val count = IntArray(1)
            val attributes = intArrayOf(
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
                EGL14.EGL_NONE,
            )
            if (!EGL14.eglChooseConfig(display, attributes, 0, configs, 0, 1, count, 0) || count[0] == 0) return ""
            val context = EGL14.eglCreateContext(
                display, configs[0], EGL14.EGL_NO_CONTEXT,
                intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0,
            )
            if (context == EGL14.EGL_NO_CONTEXT) return ""
            val surface = EGL14.eglCreatePbufferSurface(
                display, configs[0], intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE), 0,
            )
            try {
                if (!EGL14.eglMakeCurrent(display, surface, surface, context)) return ""
                return GLES20.glGetString(GLES20.GL_RENDERER) ?: ""
            } finally {
                EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
                if (surface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, surface)
                EGL14.eglDestroyContext(display, context)
            }
        } finally {
            EGL14.eglTerminate(display)
        }
    }

    private fun totalMemoryMB(context: Context): Long {
        val manager = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager ?: return 0
        val info = ActivityManager.MemoryInfo()
        manager.getMemoryInfo(info)
        return info.totalMem / (1024 * 1024)
    }

    private fun cpuNumber(cpu: Int, file: String): Long =
        try { File("/sys/devices/system/cpu/cpu$cpu/$file").readText().trim().toLong() } catch (_: Throwable) { 0 }

    /**
     * The cores that count as fast. cpu_capacity is the scheduler's own measure of
     * a core (1024 for the best) and it sees what a clock cannot: a Helio G99's
     * A55s clock at 2.0 GHz, within 10% of its A76s, at about half their speed.
     * Without it, the clock within 15% of the fastest.
     */
    private fun fastCores(): Int {
        val cores = Runtime.getRuntime().availableProcessors().coerceAtLeast(1)
        val capacity = (0 until cores).map { cpuNumber(it, "cpu_capacity") }
        if (capacity.any { it > 0 }) {
            val best = capacity.max()
            return capacity.count { it >= best * 6 / 10 }
        }
        val clock = (0 until cores).map { cpuNumber(it, "cpufreq/cpuinfo_max_freq") }
        val best = clock.max()
        return if (best > 0) clock.count { it >= best * 85 / 100 } else cores
    }

    private fun fastestMHz(): Int {
        val cores = Runtime.getRuntime().availableProcessors().coerceAtLeast(1)
        return ((0 until cores).maxOf { cpuNumber(it, "cpufreq/cpuinfo_max_freq") } / 1000).toInt()
    }

    const val PRESET_CUSTOM = "CUSTOM"
}
