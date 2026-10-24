package com.pes2017.recomp

import android.annotation.SuppressLint
import android.app.Activity
import android.app.ActivityManager
import android.content.ComponentCallbacks2
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.res.Configuration
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.BatteryManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.os.SystemClock
import java.io.File
import java.util.Locale
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.TextView
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.SwitchCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import kotlin.math.max
import kotlin.math.roundToInt

/**
 * The run itself: a surface for the renderer, the pad drawn over it, and the
 * few decisions only the app can make -- when to pause, when to let go of
 * the surface, and when the whole thing ends.
 *
 * It lives in its own process (":game" in the manifest). Ending a run means
 * killing that process: the guest is inside recompiled code that cannot be
 * unwound and a driver mid-frame does not survive its device disappearing,
 * so there is nothing to tidy up and everything to lose by trying. Killing
 * the process is instant, cannot fail, and cannot crash -- and the launcher,
 * in the other process, is still there afterwards.
 */
class GameActivity : AppCompatActivity(), NativeListener {

    private lateinit var prefs: Prefs
    private lateinit var surfaceView: SurfaceView
    private lateinit var overlay: TouchOverlayView
    private lateinit var root: FrameLayout
    private lateinit var hudContainer: FrameLayout
    private lateinit var statsText: TextView
    private lateinit var hudExpandedCard: LinearLayout
    private lateinit var hudExpandedText: TextView
    private var hudIsExpanded = false
    private lateinit var menuButton: View
    private lateinit var captureButton: Button
    private lateinit var macroButton: Button
    private var editPanel: ControlEditPanel? = null

    private val pads = GamepadInput()
    private val handler = Handler(Looper.getMainLooper())
    private var vibrator: Vibrator? = null
    private var lastRumble = 0

    private val statsValues = FloatArray(9)
    private var started = false
    private var cachedRendererInfo = ""

    private var cpuUsagePercent = -1f
    private var lastCpuTotal = 0L
    private var lastCpuIdle = 0L
    private var lastProcTime = 0L
    private var lastProcSampleTime = 0L
    @Volatile private var monitoringActive = true
    private var monitorThread: Thread? = null

    private fun startHardwareMonitor() {
        monitoringActive = true
        monitorThread = Thread {
            while (monitoringActive) {
                sampleCpu()
                try {
                    Thread.sleep(1000)
                } catch (_: InterruptedException) {
                    break
                }
            }
        }.apply {
            isDaemon = true
            name = "pes17-hw-monitor"
            start()
        }
    }

    private fun sampleCpu() {
        try {
            val statFile = File("/proc/stat")
            if (statFile.exists()) {
                val line = statFile.bufferedReader().use { it.readLine() }
                if (line != null && line.startsWith("cpu ")) {
                    val parts = line.split("\\s+".toRegex())
                    if (parts.size >= 5) {
                        val user = parts[1].toLong()
                        val nice = parts[2].toLong()
                        val sys = parts[3].toLong()
                        val idle = parts[4].toLong()
                        val iowait = if (parts.size > 5) parts[5].toLong() else 0L
                        val total = user + nice + sys + idle + iowait
                        val idleTotal = idle + iowait
                        if (lastCpuTotal > 0 && total > lastCpuTotal) {
                            val dTotal = total - lastCpuTotal
                            val dIdle = idleTotal - lastCpuIdle
                            cpuUsagePercent = ((dTotal - dIdle).toFloat() / dTotal.toFloat()) * 100f
                        }
                        lastCpuTotal = total
                        lastCpuIdle = idleTotal
                        return
                    }
                }
            }
        } catch (_: Exception) {}

        try {
            val statFile = File("/proc/self/stat")
            if (statFile.exists()) {
                val line = statFile.readText().trim()
                val rParen = line.lastIndexOf(')')
                if (rParen > 0) {
                    val rest = line.substring(rParen + 2).split(" ")
                    val utime = rest[11].toLong()
                    val stime = rest[12].toLong()
                    val procTime = utime + stime
                    val now = SystemClock.elapsedRealtime()
                    if (lastProcSampleTime > 0 && now > lastProcSampleTime) {
                        val dTimeMs = now - lastProcSampleTime
                        val dTicks = procTime - lastProcTime
                        val dProcMs = dTicks * 10L
                        val cores = Runtime.getRuntime().availableProcessors().coerceAtLeast(1)
                        val pct = (dProcMs.toFloat() / (dTimeMs * cores).toFloat()) * 100f
                        cpuUsagePercent = pct.coerceIn(0f, 100f)
                    }
                    lastProcTime = procTime
                    lastProcSampleTime = now
                    return
                }
            }
        } catch (_: Exception) {}

        cpuUsagePercent = -1f
    }

    private fun getDeviceTemp(): Float {
        var fallbackTemp = 0f
        for (i in 0..19) {
            val typeFile = File("/sys/class/thermal/thermal_zone$i/type")
            val tempFile = File("/sys/class/thermal/thermal_zone$i/temp")
            if (tempFile.exists()) {
                try {
                    val raw = tempFile.readText().trim().toLongOrNull() ?: continue
                    val c = if (raw > 1000) raw / 1000f else raw.toFloat()
                    if (c in 20.0f..105.0f) {
                        val typeName = if (typeFile.exists()) typeFile.readText().trim().lowercase() else ""
                        if (typeName.contains("cpu") || typeName.contains("soc") ||
                            typeName.contains("ap") || typeName.contains("tsens") ||
                            typeName.contains("mtktscpu")) {
                            return c
                        }
                        if (fallbackTemp == 0f) fallbackTemp = c
                    }
                } catch (_: Exception) {}
            }
        }
        if (fallbackTemp > 0f) return fallbackTemp

        try {
            val filter = IntentFilter(Intent.ACTION_BATTERY_CHANGED)
            val batteryIntent = registerReceiver(null, filter)
            val temp = batteryIntent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) ?: 0
            if (temp > 0) return temp / 10.0f
        } catch (_: Exception) {}
        return 0f
    }

    private fun getMemoryStats(): Pair<Int, Int> {
        return try {
            val actManager = getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager
            val memInfo = ActivityManager.MemoryInfo()
            actManager?.getMemoryInfo(memInfo)
            val totalMb = (memInfo.totalMem / (1024 * 1024)).toInt()
            val jvm = Runtime.getRuntime().totalMemory() - Runtime.getRuntime().freeMemory()
            val nativeMem = android.os.Debug.getNativeHeapAllocatedSize()
            val appMb = ((jvm + nativeMem) / (1024 * 1024)).toInt()
            Pair(appMb, totalMb)
        } catch (_: Exception) {
            Pair(0, 0)
        }
    }

    private fun getBatteryStats(): Triple<Float, Boolean, Int> {
        return try {
            val filter = IntentFilter(Intent.ACTION_BATTERY_CHANGED)
            val intent = registerReceiver(null, filter)
            val raw = intent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) ?: 0
            val tempC = if (raw > 0) raw / 10.0f else 0.0f
            val status = intent?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
            val isCharging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
                             status == BatteryManager.BATTERY_STATUS_FULL
            val level = intent?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
            Triple(tempC, isCharging, level)
        } catch (_: Exception) {
            Triple(0f, false, -1)
        }
    }

    private val statsTick = object : Runnable {
        override fun run() {
            if (prefs.showStats && NativeBridge.isLoaded()) {
                NativeBridge.nativeStats(statsValues)
                if (cachedRendererInfo.isEmpty()) {
                    try {
                        cachedRendererInfo = NativeBridge.nativeRendererInfo()
                    } catch (_: Throwable) {
                        cachedRendererInfo = "Vulkan | Mali Fallback | Native"
                    }
                }

                val fps = statsValues[0].roundToInt()
                val texMb = statsValues[1].roundToInt()
                val resW = statsValues[2].roundToInt()
                val resH = statsValues[3].roundToInt()
                val frameTimeMs = if (statsValues[4] > 0.01f) statsValues[4] else if (fps > 0) 1000f / fps else 0f
                val framesPresented = statsValues[5].toLong()

                val temp = getDeviceTemp()
                val (appMb, totalMb) = getMemoryStats()
                val (battTemp, isCharging, battPct) = getBatteryStats()
                val cpuStr = if (cpuUsagePercent >= 0f) String.format(Locale.US, "%.0f%%", cpuUsagePercent) else "N/A"

                val warmupQueued = if (statsValues.size >= 7) statsValues[6].toInt() else 0
                val warmupCompleted = if (statsValues.size >= 8) statsValues[7].toInt() else 0
                val warmupLastMs = if (statsValues.size >= 9) statsValues[8].toInt() else 0

                val tempPart = if (temp > 0f) String.format(Locale.US, " · %.0f°C", temp) else ""
                val ramPart = if (appMb > 0) String.format(Locale.US, " · RAM %dM", appMb) else ""
                val ftPart = if (frameTimeMs > 0f) String.format(Locale.US, " · %.1fms", frameTimeMs) else ""
                val warmupPart = if (warmupQueued > 0 && warmupCompleted < warmupQueued) {
                    val pct = (warmupCompleted * 100) / warmupQueued
                    String.format(Locale.US, "  •  ⚡ Shaders %d%% (%d/%d)", pct, warmupCompleted, warmupQueued)
                } else ""

                // As much as the player asked for (Prefs.hudLevel), kept to two short
                // lines so it stays clear of the shoulder buttons beneath it.
                val level = prefs.hudLevel
                val expand = if (prefs.developerTools) "  [▼]" else ""
                statsText.text = when (level) {
                    Prefs.HUD_MINIMAL -> String.format(Locale.US, "%d FPS%s", fps, expand)
                    Prefs.HUD_NORMAL -> String.format(Locale.US, "%d FPS%s%s%s", fps, ftPart, tempPart, expand)
                    else -> String.format(
                        Locale.US, "%d FPS%s%s%s\nCPU %s%s · Tex %dMB%s",
                        fps, ftPart, tempPart, expand, cpuStr, ramPart, texMb, warmupPart,
                    )
                }

                if (hudIsExpanded) {
                    val parts = cachedRendererInfo.split("|")
                    val devName = if (parts.isNotEmpty() && parts[0].isNotEmpty()) parts[0] else "Vulkan Device"
                    val renderMode = if (parts.size > 1) parts[1] else "Vulkan 1.1 Legacy RenderPass"
                    val tcMode = if (parts.size > 2) parts[2] else "CPU Decompress Fallback"

                    val battStr = if (battTemp > 0f) {
                        val chg = if (isCharging) " (Chg $battPct%)" else if (battPct >= 0) " ($battPct%)" else ""
                        String.format(Locale.US, "%.1f°C%s", battTemp, chg)
                    } else "N/A"

                    val ramStr = if (totalMb > 0) String.format(Locale.US, "%d MB (App) / %d MB (Total)", appMb, totalMb)
                                 else if (appMb > 0) "$appMb MB" else "N/A"
                    val cpuTempStr = if (temp > 0f) String.format(Locale.US, "%.0f°C", temp) else "N/A"

                    hudExpandedText.text = buildString {
                        append("FPS / FRAME TIME\n")
                        append(String.format(Locale.US, "  ▶ %d FPS   (%.1f ms)\n", fps, frameTimeMs))
                        append(String.format(Locale.US, "  Presented: %,d frames\n\n", framesPresented))

                        append("CPU & SYSTEM\n")
                        append("  Utilization: $cpuStr (${Runtime.getRuntime().availableProcessors()} Cores)\n")
                        append("  SoC/Thermal: $cpuTempStr  ·  Battery: $battStr\n\n")

                        append("GPU & VULKAN\n")
                        append("  Device: $devName\n")
                        append("  Backend: $renderMode\n")
                        append("  Textures: $texMb MB  ·  $tcMode\n")
                        if (warmupQueued > 0) {
                            val pct = (warmupCompleted * 100) / warmupQueued
                            append("  Prewarm: $warmupCompleted / $warmupQueued ($pct%)  ·  Last: ${warmupLastMs}ms\n\n")
                        } else {
                            append("\n")
                        }

                        append("MEMORY & VIEWPORT\n")
                        append("  App / System Total RAM: $ramStr\n")
                        append("  Internal Render: ${resW}×${resH}  •  Refresh: ${refreshHz().roundToInt()} Hz\n")
                    }
                }
            }
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = Prefs(this)

        registerMacroControl()
        Trail.note(this, "game: starting")
        if (!NativeBridge.load()) {
            fail(getString(R.string.error_library, NativeBridge.loadError() ?: "?"))
            return
        }

        buildUi()
        goFullscreen()
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        // Sustained performance mode locks CPU/GPU governors to steady frequencies
        // to prevent erratic thermal throttling mid-gameplay. On some phones that
        // steady level is a low ceiling, so files/no_sustained turns it off for A/B runs.
        val noSustained = getExternalFilesDir(null)?.let { File(it, "no_sustained") }?.isFile == true
        window.setSustainedPerformanceMode(!noSustained)
        Trail.note(this, "game: sustained performance mode ${if (noSustained) "off" else "on"}")
        watchThermalStatus()
        if (prefs.prefer60Hz) {
            val attributes = window.attributes
            attributes.preferredRefreshRate = 60f
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                val displayObj = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) display
                else {
                    @Suppress("DEPRECATION")
                    windowManager.defaultDisplay
                }
                if (displayObj != null) {
                    val mode60 = displayObj.supportedModes.firstOrNull { kotlin.math.abs(it.refreshRate - 60f) < 1f }
                    if (mode60 != null) {
                        attributes.preferredDisplayModeId = mode60.modeId
                    }
                }
            }
            window.attributes = attributes
        }

        vibrator = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            (getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? VibratorManager)?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
        }

        NativeBridge.listener = this
        NativeBridge.nativeInit(
            filesDir.absolutePath,
            (getExternalFilesDir(null) ?: filesDir).absolutePath,
            cacheDir.absolutePath,
            applicationInfo.nativeLibraryDir,
            this,
        )

        applyOptions()
        applyDriver()

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (overlay.editing) endEditing(save = true) else showMenu()
            }
        })

        val gameDirectory = prefs.gameDirectory
        if (gameDirectory.isEmpty() || !NativeBridge.nativeGameInstalled(gameDirectory)) {
            fail(getString(R.string.error_not_installed))
            return
        }
        Trail.note(this, "game: handing over to the runtime")
        if (!NativeBridge.nativeStart(gameDirectory)) {
            fail(NativeBridge.nativeStatus().ifEmpty { getString(R.string.error_start) })
            return
        }
        started = true
        // The run is up, so ask the system to treat its process as one still
        // wanted: stepping out to a message then costs a pause, not the whole
        // load again (GameService.kt).
        GameService.start(this)
        Trail.clear(this)
        pads.firstSlot = if (prefs.padAsPlayer2 && prefs.developerTools) 1 else 0
        pads.setEnabled(prefs.gamepadEnabled)
        pads.refreshDevices()
        startHardwareMonitor()
        handler.post(statsTick)
    }

    // ---- the screen ---------------------------------------------------------

    @SuppressLint("SetTextI18n")
    private fun buildUi() {
        root = FrameLayout(this)
        root.setBackgroundColor(Color.BLACK)

        surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) = Unit

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                // The renderer draws at a fraction of the screen and the
                // display scales it up for free -- the single biggest thing
                // a weak phone can be given. setFixedSize comes back here
                // with the new size, which is when the runtime is told.
                val scale = prefs.resolutionScale
                val wantedWidth = max(320, (surfaceView.width * scale).roundToInt())
                val wantedHeight = max(240, (surfaceView.height * scale).roundToInt())
                if (width != wantedWidth || height != wantedHeight) {
                    holder.setFixedSize(wantedWidth, wantedHeight)
                    return
                }
                NativeBridge.nativeSurfaceChanged(holder.surface, width, height, refreshHz())
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                // Blocking on purpose: the presenter has to let go of this
                // window before Android takes it away, and the runtime waits
                // for the acknowledgement (runtime/android/window.cpp).
                NativeBridge.nativeSurfaceDestroyed()
            }
        })
        root.addView(
            surfaceView,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT,
            )
        )

        overlay = TouchOverlayView(this)
        overlay.layout = ControlLayout.load(this)
        overlay.visibility = if (prefs.touchControls) View.VISIBLE else View.GONE
        overlay.onLayoutEdited = { overlay.layout.save(this) }
        overlay.onSelectionChanged = { editPanel?.show(it) }
        root.addView(
            overlay,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT,
            )
        )

        hudContainer = FrameLayout(this)
        hudContainer.visibility = if (prefs.showStats) View.VISIBLE else View.GONE
        overlay.playStation = prefs.playStationButtons

        statsText = TextView(this)
        statsText.setTextColor(Color.argb(245, 224, 242, 254))
        statsText.textSize = 10f
        statsText.typeface = Typeface.MONOSPACE
        statsText.setPadding(pad(10), pad(4), pad(10), pad(4))
        val pillBg = GradientDrawable().apply {
            setColor(Color.argb(215, 15, 23, 42))
            cornerRadius = pad(10).toFloat()
            setStroke(pad(1), Color.argb(80, 56, 189, 248))
        }
        statsText.background = pillBg
        statsText.setOnClickListener {
            // The detail card and its console are developer tools.
            if (!prefs.developerTools) return@setOnClickListener
            hudIsExpanded = true
            statsText.visibility = View.GONE
            hudExpandedCard.visibility = View.VISIBLE
        }
        hudContainer.addView(
            statsText,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.START,
            )
        )

        hudExpandedCard = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val cardBg = GradientDrawable().apply {
                setColor(Color.argb(235, 15, 23, 42))
                cornerRadius = pad(12).toFloat()
                setStroke(pad(1), Color.argb(120, 56, 189, 248))
            }
            background = cardBg
            setPadding(pad(14), pad(10), pad(14), pad(10))
            visibility = View.GONE
            setOnClickListener {
                hudIsExpanded = false
                hudExpandedCard.visibility = View.GONE
                statsText.visibility = View.VISIBLE
            }
        }

        val hudHeader = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        val hudTitle = TextView(this).apply {
            text = "PES 2017 ANDROID · PERFORMANCE"
            setTextColor(Color.argb(255, 56, 189, 248))
            textSize = 11f
            typeface = Typeface.DEFAULT_BOLD
        }
        val hudCollapseBtn = TextView(this).apply {
            text = "  [▲ Close]"
            setTextColor(Color.argb(200, 148, 163, 184))
            textSize = 10f
            typeface = Typeface.MONOSPACE
            setOnClickListener {
                hudIsExpanded = false
                hudExpandedCard.visibility = View.GONE
                statsText.visibility = View.VISIBLE
            }
        }
        val hudConsoleBtn = TextView(this).apply {
            text = " [⚡ CMD]"
            setTextColor(Color.argb(230, 245, 158, 11))
            textSize = 10f
            typeface = Typeface.MONOSPACE
            setOnClickListener { showConsoleDialog() }
        }
        hudHeader.addView(hudTitle, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        hudHeader.addView(hudConsoleBtn, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        hudHeader.addView(hudCollapseBtn, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        hudExpandedCard.addView(hudHeader)

        hudExpandedText = TextView(this).apply {
            setTextColor(Color.argb(240, 226, 232, 240))
            textSize = 9.5f
            typeface = Typeface.MONOSPACE
            setPadding(0, pad(6), 0, 0)
        }
        hudExpandedCard.addView(hudExpandedText)

        hudContainer.addView(
            hudExpandedCard,
            FrameLayout.LayoutParams(
                pad(320), ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.START,
            )
        )

        root.addView(
            hudContainer,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.START,
            ).apply { topMargin = pad(6); marginStart = pad(8) }
        )

        // Icon only, a square of its own: a text Button keeps its minimum width.
        menuButton = android.widget.ImageButton(this).apply {
            setImageResource(R.drawable.ic_menu_hamburger)
            setBackgroundColor(Color.argb(140, 60, 60, 60))
            scaleType = android.widget.ImageView.ScaleType.CENTER
            alpha = 0.55f
            contentDescription = getString(R.string.menu)
            setOnClickListener { showMenu() }
        }
        root.addView(
            menuButton,
            FrameLayout.LayoutParams(pad(38), pad(34), Gravity.TOP or Gravity.END)
                .apply { topMargin = pad(6); marginEnd = pad(6) }
        )

        // Debug: one press starts a capture, the next stops it and saves a folder with
        // everything needed to look at what went wrong (CaptureSession).
        captureButton = smallButton("CAPTURE") { toggleCapture() }
        root.addView(
            captureButton,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.END,
            ).apply { topMargin = pad(6); marginEnd = pad(50) }
        )
        // Debug: record a controller macro without opening the menu; the next press stops it.
        macroButton = smallButton("REC MACRO") { toggleRecording() }
        root.addView(
            macroButton,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.END,
            ).apply { topMargin = pad(6); marginEnd = pad(130) }
        )
        applyDeveloperTools()

        setContentView(root)
    }

    private fun smallButton(label: String, onClick: () -> Unit): Button {
        val button = Button(this)
        button.text = label
        button.textSize = 11f
        button.alpha = 0.55f
        button.setPadding(pad(12), pad(4), pad(12), pad(4))
        button.minHeight = pad(34)
        button.minWidth = pad(64)
        button.setOnClickListener { onClick() }
        return button
    }

    private fun showConsoleDialog() {
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad(16), pad(12), pad(16), pad(8))
        }

        val input = EditText(this).apply {
            hint = "e.g. god, give all, cg_fov 85, cg_drawFPS 1..."
            setTextColor(Color.WHITE)
            setHintTextColor(Color.GRAY)
            setPadding(pad(12), pad(10), pad(12), pad(10))
        }
        container.addView(input)

        val scroll = HorizontalScrollView(this).apply {
            isFillViewport = true
            setPadding(0, pad(8), 0, 0)
        }
        val chipRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
        }

        val presets = listOf(
            "cg_fov 85" to "Ultrawide (85°)",
            "cg_fov 65" to "Default (65°)",
            "cg_drawFPS 1" to "Show FPS",
            "god" to "God Mode",
            "give all" to "All Weapons",
            "ufo" to "UFO / Fly",
            "r_fullbright 1" to "Full Bright",
            "cg_drawGun 0" to "Hide Gun",
        )

        for ((cmd, label) in presets) {
            val chip = Button(this).apply {
                text = label
                textSize = 9.5f
                alpha = 0.85f
                setPadding(pad(8), pad(2), pad(8), pad(2))
                minHeight = pad(28)
                minWidth = pad(48)
                setOnClickListener {
                    input.setText(cmd)
                    input.setSelection(cmd.length)
                }
            }
            chipRow.addView(chip, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                marginEnd = pad(6)
            })
        }
        scroll.addView(chipRow)
        container.addView(scroll)

        AlertDialog.Builder(this)
            .setTitle("IW4 Console Command")
            .setView(container)
            .setPositiveButton("Run") { _, _ ->
                val cmd = input.text.toString().trim()
                if (cmd.isNotEmpty()) {
                    NativeBridge.nativeRunConsoleCommand(cmd)
                }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun pad(dp: Int): Int = (dp * resources.displayMetrics.density).roundToInt()

    private fun refreshHz(): Float {
        val shown = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) display
        else {
            @Suppress("DEPRECATION")
            windowManager.defaultDisplay
        }
        val hz = shown?.refreshRate ?: 60f
        return if (hz < 20f) 60f else hz
    }

    /** The whole screen's long side over its short side, cutout included (the game is laid out into it). */
    private fun screenAspect(): Float {
        val (w, h) = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val bounds = windowManager.maximumWindowMetrics.bounds
            bounds.width() to bounds.height()
        } else {
            val metrics = android.util.DisplayMetrics()
            @Suppress("DEPRECATION")
            windowManager.defaultDisplay.getRealMetrics(metrics)
            metrics.widthPixels to metrics.heightPixels
        }
        val long = maxOf(w, h).toFloat()
        val short = minOf(w, h).toFloat()
        return if (short > 0f) long / short else 16f / 9f
    }

    private fun goFullscreen() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val controller = WindowInsetsControllerCompat(window, window.decorView)
        controller.hide(WindowInsetsCompat.Type.systemBars())
        controller.systemBarsBehavior =
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            // The attributes have to be handed back for a change to take.
            val attributes = window.attributes
            if (attributes.layoutInDisplayCutoutMode !=
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            ) {
                attributes.layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
                window.attributes = attributes
            }
        }
    }

    // ---- what the runtime is told before it starts -----------------------------

    private fun applyOptions() {
        // Everything here is one of the runtime's own switches
        // (docs/switches.md); the app just fills them in.
        NativeBridge.nativeSetOption("PES17_FULLSCREEN", "1")
        NativeBridge.nativeSetOption("PES17_NO_AUDIO", if (prefs.audioEnabled) "" else "1")
        NativeBridge.nativeSetOption("PES17_NO_MSAA", if (prefs.multisampling) "0" else "1")   // "0", not unset: perf.cpp defaults an unset one to 1
        if (prefs.renderScale in 1..3) {
            NativeBridge.nativeSetOption("PES17_SCALE", prefs.renderScale.toString())
        }
        NativeBridge.nativeSetOption("PES17_STUTTERS", "1")
        // The port's own switches, from Settings > Performance. Always given,
        // so a player's choice wins over the runtime's per-device defaults.
        NativeBridge.nativeSetOption("PES17_SCENE_SCALE", String.format(Locale.US, "%.4f", prefs.sceneScalePercent / 100.0))
        NativeBridge.nativeSetOption("PES17_BALL_SCALE", String.format(Locale.US, "%.2f", prefs.ballScalePercent / 100.0))
        NativeBridge.nativeSetOption("PES17_HEAT_SCALE", if (prefs.heatScale) "1" else "")
        NativeBridge.nativeSetOption("PES17_LOAD_SCALE", if (prefs.loadScale) "1" else "")
        NativeBridge.nativeSetOption("PES17_HINT", if (prefs.perfHint) "" else "0")
        NativeBridge.nativeSetOption("PES17_NO_CROWD", if (prefs.crowd) "" else "1")
        NativeBridge.nativeSetOption("PES17_UPSCALER", if (prefs.fsr) "fsr" else "")
        NativeBridge.nativeSetOption("PES17_FSR_SHARPNESS", String.format(Locale.US, "%.1f", prefs.fsrSharpness))
        // For trying other caps without a rebuild: a number in files/fps_limit
        // (adb shell "echo 40 > /sdcard/Android/data/com.pes2017.recomp/files/fps_limit") wins over the switch.
        val fpsOverride = try {
            getExternalFilesDir(null)?.let { File(it, "fps_limit") }?.takeIf { it.isFile }?.readText()?.trim()
        } catch (_: Exception) { null }
        NativeBridge.nativeSetOption("PES17_FPS_LIMIT", fpsOverride?.takeIf { it.isNotEmpty() } ?: if (prefs.fpsLimit < 60) prefs.fpsLimit.toString() else "")
        NativeBridge.nativeSetOption(
            "PES17_ASPECT",
            if (prefs.widescreen) String.format(Locale.US, "%.4f", screenAspect()) else "",
        )
        NativeBridge.nativeSetOption("PES17_NO_SHADOW", if (prefs.lowMemory) "1" else "")
        // Full synchronous precompile at boot when the player asked for it in
        // Settings; otherwise the tiered instant-boot budget + background warmup.
        NativeBridge.nativeSetOption("PES17_SHADER_PREWARM_ALL", if (prefs.prewarmAllShaders) "1" else "")
        NativeBridge.nativeSetOption(
            "PES17_TEXTURE_BUDGET_MB",
            if (prefs.textureBudgetMB > 0) prefs.textureBudgetMB.toString() else "",
        )
        // Both caches live in the app's own folder and make the second start
        // of a level far quicker than the first.
        val extDir = getExternalFilesDir(null)
        val shaderCacheFile = if (extDir != null) File(extDir, "shader_cache.bin") else File(filesDir, "shader_cache.bin")
        if (!shaderCacheFile.exists()) {
            val internalCache = File(filesDir, "shader_cache.bin")
            val legacyShaders = File(filesDir, "shaders")
            if (internalCache.exists() && internalCache.length() > 0) {
                internalCache.copyTo(shaderCacheFile, overwrite = true)
            } else if (legacyShaders.exists() && legacyShaders.length() > 0) {
                legacyShaders.copyTo(shaderCacheFile, overwrite = true)
            } else {
                adoptRicherSeed(shaderCacheFile)
            }
        } else {
            // A fuller bundled seed replaces a thinner installed cache, so a
            // recorded playthrough shipped in the APK actually reaches play.
            adoptRicherSeed(shaderCacheFile)
        }
        NativeBridge.nativeSetOption("PES17_PIPELINE_CACHE", "${filesDir.absolutePath}/pipeline.cache")
        NativeBridge.nativeSetOption("PES17_SHADER_CACHE", shaderCacheFile.absolutePath)
        // Textures converted for the GPU (BC to ASTC) in earlier sessions,
        // read back instead of converted again (runtime/gpu/vulkan/texture_store.h).
        // The internal folder: reads through the external one's sdcardfs ran at
        // under 500 MB/s, which made a texture as slow to read as to convert.
        NativeBridge.nativeSetOption("PES17_TEXTURE_STORE", "${filesDir.absolutePath}/texture_store.bin")

        NativeBridge.nativeLookSettings(
            prefs.lookSensitivityX, prefs.lookSensitivityY,
            prefs.lookSmoothing, prefs.invertLook, prefs.lookSaturation,
        )
        NativeBridge.nativePadEnabled(prefs.gamepadEnabled)
    }

    private data class SeedCounts(val magic: Int, val version: Int, val shaders: Int, val pipelines: Int)

    private fun readSeedCounts(stream: java.io.InputStream): SeedCounts? = try {
        stream.use {
            val head = ByteArray(16)
            var read = 0
            while (read < 16) {
                val n = it.read(head, read, 16 - read)
                if (n <= 0) break
                read += n
            }
            if (read < 16) null
            else {
                val words = java.nio.ByteBuffer.wrap(head).order(java.nio.ByteOrder.LITTLE_ENDIAN).asIntBuffer()
                SeedCounts(words[0], words[1], words[2], words[3])
            }
        }
    } catch (_: Exception) {
        null
    }

    // The APK ships a recorded seed; a fuller one wins over a thinner
    // installed cache, whichever side it came from. Headers compared, never
    // trusted blindly; a foreign or corrupt file is left alone.
    private fun adoptRicherSeed(target: File) {
        val seed = try {
            readSeedCounts(assets.open("shader_cache.bin"))
        } catch (_: Exception) {
            null
        } ?: return
        val have = if (target.exists() && target.length() >= 16) {
            try {
                readSeedCounts(target.inputStream())
            } catch (_: Exception) {
                null
            }
        } else null
        val take = have == null ||
            (seed.magic == have.magic && seed.version == have.version && seed.pipelines > have.pipelines)
        if (!take) return
        try {
            assets.open("shader_cache.bin").use { input ->
                target.outputStream().use { output -> input.copyTo(output) }
            }
            android.util.Log.i("PES17", "Shader seed: bundled ${seed.pipelines} pipelines replaces ${have?.pipelines ?: 0}")
        } catch (_: Exception) {
        }
    }

    private fun applyDriver() {
        val directory = prefs.driverDirectory
        val library = prefs.driverLibrary
        if (directory.isNotEmpty() && library.isNotEmpty()) {
            NativeBridge.nativeSelectDriver(directory, library)
        } else {
            NativeBridge.nativeSelectDriver("", "")
        }
    }

    // ---- the pause menu ----------------------------------------------------------

    private fun showMenu() {
        // Built as a list: the invite entry only exists in the multiplayer
        // app, so fixed indices would point at the wrong rows there.
        val labels = mutableListOf(
            getString(R.string.resume) to { _: android.content.DialogInterface -> },
            getString(R.string.edit_controls) to { _: android.content.DialogInterface -> beginEditing() },
        )
        // Macros are a test tool: only with developer tools on (Settings > Advanced).
        // Recording has its own button beside Menu; playing is picked here.
        if (prefs.developerTools || Macro.recording || Macro.playing) {
            labels.add(getString(if (Macro.playing) R.string.macro_stop_play else R.string.macro_play) to { _: android.content.DialogInterface ->
                if (Macro.playing) Macro.stopPlayback() else pickMacro()
            })
        }
        labels.add(getString(if (prefs.touchControls) R.string.hide_touch_controls else R.string.show_touch_controls) to { _: android.content.DialogInterface -> setTouchControls(!prefs.touchControls) })
        labels.add(getString(if (prefs.gamepadEnabled) R.string.disable_gamepad else R.string.enable_gamepad) to { _: android.content.DialogInterface -> setGamepad(!prefs.gamepadEnabled) })
        labels.add(getString(R.string.qg_menu) to { _: android.content.DialogInterface ->
            QuickGraphics.show(this, prefs, onApply = { quit() }, onClose = { goFullscreen() })
        })
        labels.add(getString(R.string.hud_menu) to { _: android.content.DialogInterface -> pickHudLevel() })
        labels.add(getString(R.string.quit) to { _: android.content.DialogInterface -> confirmQuit() })
        AlertDialog.Builder(this)
            .setTitle(R.string.paused)
            .setItems(labels.map { it.first }.toTypedArray()) { dialog, which ->
                dialog.dismiss()
                labels[which].second(dialog)
            }
            .setOnDismissListener { goFullscreen() }
            .show()
    }

    /** A physical pad took over: the touch pad is out of the way until the screen is touched. */
    private var touchHiddenByPad = false

    private fun hideTouchForPad() {
        if (touchHiddenByPad || !prefs.touchControls || overlay.editing) return
        touchHiddenByPad = true
        overlay.releaseEverything()
        overlay.visibility = View.GONE
    }

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        if (touchHiddenByPad && event.actionMasked == MotionEvent.ACTION_DOWN &&
            event.isFromSource(android.view.InputDevice.SOURCE_TOUCHSCREEN)
        ) {
            touchHiddenByPad = false
            if (prefs.touchControls) overlay.visibility = View.VISIBLE
        }
        return super.dispatchTouchEvent(event)
    }

    private fun setTouchControls(on: Boolean) {
        touchHiddenByPad = false
        prefs.touchControls = on
        overlay.releaseEverything()
        overlay.visibility = if (on) View.VISIBLE else View.GONE
    }

    private fun setGamepad(on: Boolean) {
        prefs.gamepadEnabled = on
        pads.setEnabled(on)
    }

    /** Off / Minimal / Normal / Complete, as in Settings > Performance. */
    private fun pickHudLevel() {
        val names = arrayOf(
            getString(R.string.hud_off), getString(R.string.hud_minimal),
            getString(R.string.hud_normal), getString(R.string.hud_complete),
        )
        AlertDialog.Builder(this)
            .setTitle(R.string.hud_menu)
            .setSingleChoiceItems(names, prefs.hudLevel) { dialog, which ->
                prefs.hudLevel = which
                hudContainer.visibility = if (prefs.showStats) View.VISIBLE else View.GONE
                dialog.dismiss()
            }
            .setOnDismissListener { goFullscreen() }
            .show()
    }

    private fun confirmQuit() {
        AlertDialog.Builder(this)
            .setTitle(R.string.quit)
            .setMessage(R.string.quit_explained)
            .setNegativeButton(R.string.cancel, null)
            .setPositiveButton(R.string.quit) { _, _ -> quit() }
            .setOnDismissListener { goFullscreen() }
            .show()
    }

    private fun quit() {
        overlay.layout.save(this)
        if (NativeBridge.isLoaded()) NativeBridge.nativeRequestStop()
        finishAndRemoveTask()
        // The only way to end a guest that is inside recompiled code. The
        // launcher is another process and carries on.
        handler.postDelayed({ Process.killProcess(Process.myPid()) }, 120)
    }

    private fun fail(message: String) {
        AlertDialog.Builder(this)
            .setTitle(R.string.cannot_start)
            .setMessage(message)
            .setCancelable(false)
            .setPositiveButton(R.string.close) { _, _ ->
                finish()
                handler.postDelayed({ Process.killProcess(Process.myPid()) }, 80)
            }
            .show()
    }

    // ---- editing the pad -----------------------------------------------------------

    private fun beginEditing() {
        if (!prefs.touchControls) setTouchControls(true)
        overlay.editing = true
        overlay.select(null)
        menuButton.visibility = View.GONE
        captureButton.visibility = View.GONE
        macroButton.visibility = View.GONE
        if (editPanel == null) {
            editPanel = ControlEditPanel(this, root, overlay) { endEditing(save = true) }
        }
        editPanel?.attach()
        NativeBridge.nativeSetPaused(true)
    }

    private fun endEditing(save: Boolean) {
        overlay.editing = false
        overlay.select(null)
        editPanel?.detach()
        menuButton.visibility = View.VISIBLE
        applyDeveloperTools()
        if (save) overlay.layout.save(this)
        NativeBridge.nativeSetPaused(false)
        goFullscreen()
    }

    // ---- input ------------------------------------------------------------------------

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        // The dialogs and the editor come first; a pad should not fire a gun
        // through a menu.
        if (started && !overlay.editing && pads.onKey(event)) {
            if (event.action == KeyEvent.ACTION_DOWN) hideTouchForPad()
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (started && !overlay.editing && pads.onMotion(event)) return true
        return super.onGenericMotionEvent(event)
    }

    // ---- lifecycle ---------------------------------------------------------------------

    override fun onResume() {
        super.onResume()
        goFullscreen()
        warnIfPowerSaving()
        if (!started) return
        NativeBridge.nativeSetPaused(false)
        setGameState(playing = true)
        // A trim while the window was away may have cut the texture cache.
        // Without this it stays cut, and every texture is uploaded again on
        // every draw for the rest of the run.
        NativeBridge.nativeRestoreMemory()
        pads.firstSlot = if (prefs.padAsPlayer2 && prefs.developerTools) 1 else 0
        pads.setEnabled(prefs.gamepadEnabled)
        pads.refreshDevices()
        // The pad may have been arranged in Settings meanwhile (ControlsEditorActivity,
        // another process): this run saved its own copy on the way out, so read it back.
        if (!overlay.editing) overlay.layout = ControlLayout.load(this)
        overlay.playStation = prefs.playStationButtons
        hudContainer.visibility = if (prefs.showStats) View.VISIBLE else View.GONE
        applyDeveloperTools()
        handler.post(statsTick)
    }

    override fun onPause() {
        super.onPause()
        if (!started) return
        // Nothing is presented while the activity is away, the mixer is
        // muted, and the touch pad lets go of whatever was held -- a trigger
        // stuck down through a phone call would empty a magazine into a wall.
        NativeBridge.nativeSetPaused(true)
        setGameState(playing = false)
        overlay.releaseEverything()
        handler.removeCallbacks(statsTick)
        overlay.layout.save(this)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) goFullscreen()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        goFullscreen()
    }

    override fun onTrimMemory(level: Int) {
        super.onTrimMemory(level)
        // The texture cache is the only thing here big enough to be worth
        // giving back, and it can rebuild what it drops
        // (runtime/android/perf.cpp).
        // UI_HIDDEN (20) says the window went away, not that memory is
        // short, and it arrives on every switch away. It is passed on so the
        // runtime can say so in the log, but the levels that mean pressure
        // while running are RUNNING_LOW and RUNNING_CRITICAL.
        if (NativeBridge.isLoaded() && level >= ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW) {
            NativeBridge.nativeTrimMemory(level)
        }
    }

    // ---- controller macros ----------------------------------------------------

    private val macroControl = object : android.content.BroadcastReceiver() {
        override fun onReceive(context: android.content.Context, intent: Intent) {
            // Remote control is opt-in: it exists only while a file says so
            // (adb shell touch .../files/macros/.remote_control).
            if (!java.io.File(Macro.directory(this@GameActivity), ".remote_control").exists()) return
            val name = intent.getStringExtra("name") ?: "last"
            val log = { text: String -> android.util.Log.i("PES17.Macro", text) }
            when (intent.getStringExtra("cmd")) {
                "record" -> { Macro.startRecording(); log("recording") }
                "stop" -> log("saved ${Macro.stopRecording(this@GameActivity, name)} events as $name")
                "play" -> Macro.play(this@GameActivity, name, intent.getIntExtra("loops", 1)) { log("play $name: $it") }
                "stopplay" -> Macro.stopPlayback()
                "list" -> log("macros: ${Macro.names(this@GameActivity)}")
                "capture_start" -> runOnUiThread { if (!capture.active) toggleCapture() }
                "capture_stop" -> runOnUiThread {
                    if (capture.active) {
                        captureButton.text = getString(R.string.capture_saving)
                        capture.stop(surfaceView, name, intent.getStringExtra("note") ?: "") { folder, message ->
                            resetCaptureButton()
                            log("capture ${folder?.name ?: message}")
                        }
                    }
                }
            }
        }
    }

    private fun registerMacroControl() {
        androidx.core.content.ContextCompat.registerReceiver(
            this, macroControl,
            android.content.IntentFilter("com.pes2017.recomp.MACRO"),
            androidx.core.content.ContextCompat.RECEIVER_EXPORTED,
        )
    }

    private val capture by lazy { CaptureSession(this, prefs) }

    private fun toggleCapture() {
        if (!capture.active) {
            capture.onTick = { seconds ->
                captureButton.text = "\u25A0 STOP %d:%02d".format(seconds / 60, seconds % 60)
            }
            capture.start(surfaceView)
            captureButton.alpha = 1f
            captureButton.setTextColor(Color.rgb(255, 90, 90))
            captureButton.text = "\u25A0 STOP 0:00"
            toast(getString(R.string.capture_started))
            return
        }
        // Stopping: say what to call it and what went wrong, then save.
        val name = EditText(this).apply {
            hint = getString(R.string.capture_name_hint)
            setSingleLine()
        }
        val note = EditText(this).apply {
            hint = getString(R.string.capture_note_hint)
            minLines = 2
        }
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad(20), pad(8), pad(20), 0)
            addView(name)
            addView(note)
        }
        AlertDialog.Builder(this)
            .setTitle(R.string.capture_stop_title)
            .setView(box)
            .setCancelable(false)
            .setPositiveButton(R.string.capture_save) { _, _ ->
                finishCapture(name.text.toString(), note.text.toString())
            }
            .setNegativeButton(R.string.capture_discard) { _, _ ->
                capture.cancel()
                resetCaptureButton()
            }
            .setOnDismissListener { goFullscreen() }
            .show()
    }

    private fun finishCapture(name: String, note: String) {
        captureButton.text = getString(R.string.capture_saving)
        capture.stop(surfaceView, name, note) { folder, message ->
            resetCaptureButton()
            if (folder == null) {
                toast(getString(R.string.capture_failed, message))
            } else {
                AlertDialog.Builder(this)
                    .setTitle(R.string.capture_saved_title)
                    .setMessage(getString(R.string.capture_saved_body, folder.name))
                    .setPositiveButton(R.string.capture_share) { _, _ -> shareCapture(folder) }
                    .setNegativeButton(R.string.close, null)
                    .setOnDismissListener { goFullscreen() }
                    .show()
            }
        }
    }

    /** Zips the capture and hands it to the system share sheet (a chat, mail or a drive). */
    private fun shareCapture(folder: java.io.File) {
        Thread {
            val ok = try { Captures.share(this, folder) } catch (_: Throwable) { false }
            if (!ok) runOnUiThread { toast(getString(R.string.capture_failed, "zip")) }
        }.start()
    }

    /**
     * The debug-only views follow Settings > Advanced > Developer tools. A capture
     * already running keeps its button, so it can still be stopped.
     */
    private fun applyDeveloperTools() {
        val show = prefs.developerTools || capture.active
        captureButton.visibility = if (show && !overlay.editing) View.VISIBLE else View.GONE
        macroButton.visibility =
            if ((prefs.developerTools || Macro.recording) && !overlay.editing) View.VISIBLE else View.GONE
        refreshMacroButton()
        if (!prefs.developerTools && hudIsExpanded) {
            hudIsExpanded = false
            hudExpandedCard.visibility = View.GONE
            statsText.visibility = View.VISIBLE
        }
    }

    private fun resetCaptureButton() {
        captureButton.text = "CAPTURE"
        captureButton.alpha = 0.55f
        captureButton.setTextColor(Color.WHITE)
    }

    private fun refreshMacroButton() {
        if (Macro.recording) {
            macroButton.text = "\u25A0 STOP REC"
            macroButton.alpha = 1f
            macroButton.setTextColor(Color.rgb(255, 90, 90))
        } else {
            macroButton.text = "REC MACRO"
            macroButton.alpha = 0.55f
            macroButton.setTextColor(Color.WHITE)
        }
    }

    private fun toggleRecording() {
        if (!Macro.recording) {
            Macro.startRecording()
            refreshMacroButton()
            toast(getString(R.string.macro_recording))
            return
        }
        // Stopping: ask what to call it, so a particular macro can be named later.
        val input = EditText(this).apply {
            hint = getString(R.string.macro_name_hint)
            setText(suggestedMacroName())
            setSelectAllOnFocus(true)
            setSingleLine()
        }
        val box = LinearLayout(this).apply {
            setPadding(pad(20), pad(8), pad(20), 0)
            addView(input, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }
        AlertDialog.Builder(this)
            .setTitle(R.string.macro_name_title)
            .setMessage(getString(R.string.macro_existing, Macro.names(this).joinToString(", ").ifEmpty { "-" }))
            .setView(box)
            .setCancelable(false)
            .setPositiveButton(R.string.macro_save) { _, _ ->
                val name = input.text.toString().ifBlank { "last" }
                val count = Macro.stopRecording(this, name)
                toast(if (count < 0) getString(R.string.macro_nothing) else getString(R.string.macro_saved, count, name))
            }
            .setNegativeButton(R.string.capture_discard) { _, _ ->
                Macro.stopRecording(this, "discarded")
                java.io.File(Macro.directory(this), "discarded.macro").delete()
            }
            .setOnDismissListener { applyDeveloperTools(); goFullscreen() }
            .show()
    }

    /** macro1, macro2 ... the first not yet used. */
    private fun suggestedMacroName(): String {
        val taken = Macro.names(this).toSet()
        return generateSequence(1) { it + 1 }.map { "macro$it" }.first { it !in taken }
    }

    private fun pickMacro() {
        val names = Macro.names(this)
        if (names.isEmpty()) { toast(getString(R.string.macro_none)); return }
        AlertDialog.Builder(this)
            .setTitle(R.string.macro_play)
            .setItems(names.toTypedArray()) { _, which ->
                Macro.play(this, names[which]) { handler.post { toast(getString(R.string.macro_done, it)) } }
            }
            .setOnDismissListener { goFullscreen() }
            .show()
    }

    private fun toast(text: String) = android.widget.Toast.makeText(this, text, android.widget.Toast.LENGTH_SHORT).show()

    private var thermalListener: Any? = null

    /**
     * The phone's own heat status, from Android 10, handed to the runtime: the adaptive scene
     * scale (Settings > Adaptive quality) lowers the picture when it reaches MODERATE, before the
     * GPU is throttled, rather than after. Without a status the runtime reads none.
     */
    private fun watchThermalStatus() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return
        val power = getSystemService(Context.POWER_SERVICE) as? android.os.PowerManager ?: return
        val listener = android.os.PowerManager.OnThermalStatusChangedListener { status ->
            try { NativeBridge.nativeSetThermalStatus(status) } catch (_: Throwable) {}
        }
        thermalListener = listener
        try {
            NativeBridge.nativeSetThermalStatus(power.currentThermalStatus)
            power.addThermalStatusListener(listener)
        } catch (_: Throwable) {
            thermalListener = null
        }
    }

    // Android 13+: tells the system (and a vendor game booster listening to GameManager)
    // that gameplay that must not be interrupted is running, or that it stopped.
    private fun setGameState(playing: Boolean) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
        val games = getSystemService(Context.GAME_SERVICE) as? android.app.GameManager ?: return
        val mode = if (playing) android.app.GameState.MODE_GAMEPLAY_UNINTERRUPTIBLE else android.app.GameState.MODE_NONE
        try { games.setGameState(android.app.GameState(false, mode)) } catch (_: Throwable) {}
    }

    private fun stopWatchingThermalStatus() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return
        val listener = thermalListener as? android.os.PowerManager.OnThermalStatusChangedListener ?: return
        (getSystemService(Context.POWER_SERVICE) as? android.os.PowerManager)?.removeThermalStatusListener(listener)
        thermalListener = null
    }

    override fun onDestroy() {
        stopWatchingThermalStatus()
        try { unregisterReceiver(macroControl) } catch (_: Throwable) {}
        Macro.stopPlayback()
        super.onDestroy()
        monitoringActive = false
        monitorThread?.interrupt()
        monitorThread = null
        handler.removeCallbacksAndMessages(null)
        NativeBridge.listener = null
        if (started && !isChangingConfigurations) {
            // The activity is going for good: the run goes with it.
            GameService.stop(this)
            NativeBridge.nativeRequestStop()
            Process.killProcess(Process.myPid())
        }
    }

    // ---- what the runtime tells the app ---------------------------------------------------

    // Battery Saver halves the fast cores' clock (2.84 GHz to 1.5 GHz on a Snapdragon 865): a match falls from
    // about 58 fps to about 45, and the uneven frames show as a stuttering ball. Said once a resume.
    private fun warnIfPowerSaving() {
        val power = getSystemService(android.content.Context.POWER_SERVICE) as? android.os.PowerManager ?: return
        if (power.isPowerSaveMode)
            android.widget.Toast.makeText(this, "Battery Saver is on: the game runs slower and less smoothly. Turn it off for 60 fps.",
                android.widget.Toast.LENGTH_LONG).show()
    }

    // XamShowKeyboardUI: a text dialog, so the phone's keyboard comes up for a name.
    override fun onKeyboard(title: String, description: String, initial: String, maxLength: Int) {
        runOnUiThread {
            val input = android.widget.EditText(this).apply {
                setText(initial)
                setSelection(text.length)
                filters = arrayOf(android.text.InputFilter.LengthFilter(if (maxLength > 0) maxLength else 256))
                inputType = android.text.InputType.TYPE_CLASS_TEXT
                imeOptions = android.view.inputmethod.EditorInfo.IME_ACTION_DONE
                setSingleLine(true)
            }
            val dialog = androidx.appcompat.app.AlertDialog.Builder(this)
                .setTitle(if (title.isBlank()) "Enter text" else title)
                .setMessage(description)
                .setView(input)
                .setCancelable(false)
                .setPositiveButton(android.R.string.ok) { _, _ -> NativeBridge.nativeKeyboardResult(true, input.text.toString()) }
                .setNegativeButton(android.R.string.cancel) { _, _ -> NativeBridge.nativeKeyboardResult(false, "") }
                .create()
            input.setOnEditorActionListener { _, action, _ ->
                if (action == android.view.inputmethod.EditorInfo.IME_ACTION_DONE) {
                    dialog.getButton(android.content.DialogInterface.BUTTON_POSITIVE).performClick(); true
                } else false
            }
            dialog.window?.setSoftInputMode(android.view.WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE)
            dialog.show()
            input.requestFocus()
        }
    }

    override fun onRumble(user: Int, lowFrequency: Int, highFrequency: Int) {
        if (!prefs.vibration || user != 0) return
        val strength = max(lowFrequency, highFrequency)
        // Only on a change, and the runtime only calls on a change, so this
        // is not a vibration started sixty times a second.
        if (strength == lastRumble) return
        lastRumble = strength
        val device = vibrator ?: return
        handler.post {
            try {
                if (strength <= 0) {
                    device.cancel()
                } else {
                    val amplitude = (strength * 255 / 65535).coerceIn(1, 255)
                    device.vibrate(VibrationEffect.createOneShot(160, amplitude))
                }
            } catch (_: Throwable) {
                // A phone that refuses to vibrate is not a reason to stop
                // playing.
            }
        }
    }

    override fun onStatus(text: String) {
        handler.post {
            if (isFinishing) return@post
            if (text.startsWith("GPU lacks") || text.startsWith("Failed to") || text.startsWith("No Vulkan")) {
                AlertDialog.Builder(this)
                    .setTitle("Renderer Notice")
                    .setMessage("$text\n\nYour device GPU does not provide Vulkan 1.3 or VK_KHR_dynamic_rendering. The game will run in headless mode without video output. Check the log for details.")
                    .setPositiveButton("View Log") { _, _ ->
                        finish()
                    }
                    .setNegativeButton("Continue Headless", null)
                    .show()
            } else if (text.isNotEmpty() && prefs.showStats) {
                statsText.text = text
            }
        }
    }

    companion object {
        /** Started by the launcher once the files are in place. */
        fun start(activity: Activity) {
            activity.startActivity(Intent(activity, GameActivity::class.java))
        }
    }
}
