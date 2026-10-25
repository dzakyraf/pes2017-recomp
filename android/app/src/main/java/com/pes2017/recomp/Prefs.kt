package com.pes2017.recomp

import android.content.Context
import android.content.SharedPreferences

/**
 * Everything the player can change, in one place, with the defaults a weak
 * phone wants. Most of these end up as one of the runtime's PES17_* switches
 * (docs/switches.md) the moment a run starts.
 */
class Prefs(context: Context) {

    private val store: SharedPreferences =
        context.applicationContext.getSharedPreferences("pes17", Context.MODE_PRIVATE)

    // ---- graphics ---------------------------------------------------------

    /**
     * The fraction of the screen the game actually renders at. The surface is
     * told to be that many pixels (SurfaceHolder.setFixedSize) and the
     * compositor scales it up, which costs nothing: the phone's display
     * pipeline does that anyway. At 0.7 a 1080p phone renders 756p, close to
     * the console's own, and it is the single biggest thing a weak device can
     * be given.
     *
     * Not for this port: PES renders at a fixed 1280x720 whatever the surface is, so a smaller
     * surface only shrank the finished frame (an HD+ phone showed 896x504, and thin pitch lines
     * broke up) before the compositor stretched it back. Full size by default; the window's FSR
     * upscaler (runtime/gpu/vulkan/upscaler.cpp) does the stretch instead.
     */
    var resolutionScale: Float
        get() = store.getFloat(KEY_RESOLUTION, 0f).let { if (it <= 0f) 1.0f else it }
        set(value) = store.edit().putFloat(KEY_RESOLUTION, value.coerceIn(0.35f, 1.0f)).apply()

    /**
     * Internal Vulkan render scale multiplier (PES17_SCALE):
     * 1 = 1x native base dimensions, 2 = 2x multiplier, 3 = 3x multiplier.
     */
    var renderScale: Int
        get() = store.getInt(KEY_RENDER_SCALE, 1).coerceIn(1, 3)
        set(value) = store.edit().putInt(KEY_RENDER_SCALE, value.coerceIn(1, 3)).apply()

    /**
     * How large the 3D scene is drawn, in percent of 1280x720 (PES17_SCENE_SCALE,
     * 50 to 100). 75 matches the runtime's own default for a phone.
     */
    var sceneScalePercent: Int
        get() = store.getInt(KEY_SCENE_SCALE, 75).coerceIn(50, 100)
        set(value) = store.edit().putInt(KEY_SCENE_SCALE, value.coerceIn(50, 100)).apply()

    /**
     * How much larger the match ball is drawn, in percent (PES17_BALL_SCALE, 100 to 300): the ball
     * is a few pixels on a phone's screen, so it defaults to 150.
     */
    var ballScalePercent: Int
        get() = store.getInt(KEY_BALL_SCALE, 150).coerceIn(100, 300)
        set(value) = store.edit().putInt(KEY_BALL_SCALE, value.coerceIn(100, 300)).apply()

    /**
     * Lower the picture when the phone gets hot (PES17_HEAT_SCALE): the match's render quality
     * above is the most it will be, and the runtime takes it down as the phone's thermal status
     * rises (a quarter of the way to 50% at MODERATE, half at SEVERE, all of it from CRITICAL),
     * before the GPU is throttled, and back up as it falls. Read from the same key the first
     * adaptive switch used, so a phone that had it on keeps it.
     */
    var heatScale: Boolean
        get() = store.getBoolean(KEY_HEAT_SCALE, store.getBoolean(KEY_DYNAMIC_SCALE, false))
        set(value) = store.edit().putBoolean(KEY_HEAT_SCALE, value).apply()

    /**
     * Lower the picture when the GPU runs over the frame (PES17_LOAD_SCALE): the render passes'
     * GPU time is held under a share of the frame by stepping the scale down, and raised again
     * when there is room. It reacts to GPU pass time only, which on the Snapdragon 865 was not what made
     * a slow match slow, so it is a separate choice from the heat one. Experimental.
     */
    var loadScale: Boolean
        get() = store.getBoolean(KEY_LOAD_SCALE, store.getBoolean(KEY_DYNAMIC_SCALE, false))
        set(value) = store.edit().putBoolean(KEY_LOAD_SCALE, value).apply()

    /**
     * The system's performance hint (PES17_HINT): the frame threads report how long each frame
     * took so Android can raise or lower the CPU and GPU clock to suit. Does nothing on a phone
     * that refuses it (a Snapdragon 865 phone on Android 13 does). On by default.
     */
    var perfHint: Boolean
        get() = store.getBoolean(KEY_PERF_HINT, true)
        set(value) = store.edit().putBoolean(KEY_PERF_HINT, value).apply()

    /**
     * The stadium crowd (off: PES17_NO_CROWD=1). Off drops its draws, which saves the
     * renderer and the GPU their work and leaves the stands empty. On by default.
     */
    var crowd: Boolean
        get() = store.getBoolean(KEY_CROWD, true)
        set(value) = store.edit().putBoolean(KEY_CROWD, value).apply()

    /** FSR upscale and sharpen instead of a plain stretch (PES17_UPSCALER=fsr). */
    var fsr: Boolean
        get() = store.getBoolean(KEY_FSR, false)
        set(value) = store.edit().putBoolean(KEY_FSR, value).apply()

    /**
     * Frame generation (PES17_FRAMEGEN): the game draws 30 frames a second and a frame made
     * between each two real ones fills the other blanks of the 60 Hz screen, for half the work
     * and heat. Sets the 30 fps limit and the 60 Hz screen itself while it is on.
     */
    var frameGeneration: Boolean
        get() = store.getBoolean(KEY_FRAMEGEN, false)
        set(value) = store.edit().putBoolean(KEY_FRAMEGEN, value).apply()

    /** RCAS sharpness in stops (PES17_FSR_SHARPNESS): 0 is sharpest, 2 is softest. */
    var fsrSharpness: Float
        get() = store.getFloat(KEY_FSR_SHARP, 0.2f).coerceIn(0f, 2f)
        set(value) = store.edit().putFloat(KEY_FSR_SHARP, value.coerceIn(0f, 2f)).apply()

    /**
     * Fill a screen wider than 16:9 (PES17_ASPECT): the game's cameras are widened to the
     * screen's shape and the frame is stretched to it, instead of black bars at the sides.
     */
    var widescreen: Boolean
        get() = store.getBoolean(KEY_WIDESCREEN, true)
        set(value) = store.edit().putBoolean(KEY_WIDESCREEN, value).apply()

    /**
     * Frames a second the match is limited to (PES17_FPS_LIMIT), for heat and battery: 30 to 60
     * in steps of 5, where 60 is the console's own pacing and no limit. Before this was a
     * setting of its own, the old 30 FPS switch (cap30) stood for 30.
     */
    var fpsLimit: Int
        get() = if (store.contains(KEY_FPS_LIMIT)) store.getInt(KEY_FPS_LIMIT, 60).coerceIn(30, 60)
                else if (store.getBoolean(KEY_CAP30, false)) 30 else 60
        set(value) = store.edit().putInt(KEY_FPS_LIMIT, value.coerceIn(30, 60)).apply()

    /** Skip the large vertex shadow buffer (PES17_NO_SHADOW), for phones with little memory. */
    var lowMemory: Boolean
        get() = store.getBoolean(KEY_LOW_MEMORY, false)
        set(value) = store.edit().putBoolean(KEY_LOW_MEMORY, value).apply()

    /**
     * The performance preset last applied (DeviceProfile.Tier's name), CUSTOM once
     * the player moves one of its settings, or "" before presets were ever chosen.
     */
    var preset: String
        get() = store.getString(KEY_PRESET, "") ?: ""
        set(value) = store.edit().putString(KEY_PRESET, value).apply()

    /** Whether the player set any of the settings a preset sets, before presets existed. */
    fun hasTunedPerformance(): Boolean =
        listOf(KEY_SCENE_SCALE, KEY_MSAA, KEY_FSR, KEY_CAP30, KEY_FPS_LIMIT, KEY_LOW_MEMORY).any { store.contains(it) }

    /** The GPU's GL renderer name, as read on the build `key` (DeviceProfile). */
    fun cachedGpu(key: String): String? =
        if (store.getString(KEY_GPU_BUILD, null) == key) store.getString(KEY_GPU_NAME, null) else null

    fun cacheGpu(key: String, name: String) =
        store.edit().putString(KEY_GPU_BUILD, key).putString(KEY_GPU_NAME, name).apply()

    /** A game folder the player chose that still waits for the storage permission. */
    var pendingFolder: String
        get() = store.getString(KEY_PENDING_FOLDER, "") ?: ""
        set(value) = store.edit().putString(KEY_PENDING_FOLDER, value).apply()

    /**
     * Multisampling: the title's own 2x/4x world pass. On by default for PES: without it the thin pitch
     * lines break into dashes at the 0.75 scene scale, and on the Snapdragon 865's Adreno 650 it measured no cost
     * (50 fps with it, about 44 without, Battery Saver on).
     */
    var multisampling: Boolean
        get() = store.getBoolean(KEY_MSAA, true)
        set(value) = store.edit().putBoolean(KEY_MSAA, value).apply()

    /**
     * How much of the phone's memory the texture cache may hold. Zero means
     * "let the runtime decide from the device"
     * (runtime/gpu/vulkan/texture_cache.cpp).
     */
    var textureBudgetMB: Int
        // 0 is "automatic": the runtime sizes it from the device's memory
        // (android/perf.cpp, a sixth of it: 1 GB on an 8 GB phone). A fixed 128
        // was far too little once DXT textures are decoded to RGBA on a GPU
        // without BC support -- PES 2017's pitch alone is 2048x2048 -- and the
        // match evicted and re-uploaded its textures every few frames.
        get() = store.getInt(KEY_TEXTURE_MB, 0)
        set(value) = store.edit().putInt(KEY_TEXTURE_MB, value).apply()

    /**
     * How much the performance HUD shows: HUD_OFF, HUD_MINIMAL (frame rate),
     * HUD_NORMAL (adds frame time and temperature) or HUD_COMPLETE (adds CPU,
     * memory and textures on a second line). Before there were levels it was
     * on or off, so an old "on" reads as Normal.
     */
    var hudLevel: Int
        get() = store.getInt(KEY_HUD_LEVEL, if (store.getBoolean(KEY_SHOW_STATS, true)) HUD_NORMAL else HUD_OFF)
            .coerceIn(HUD_OFF, HUD_COMPLETE)
        set(value) = store.edit().putInt(KEY_HUD_LEVEL, value.coerceIn(HUD_OFF, HUD_COMPLETE)).apply()

    /** The HUD on or off; turning it on from off brings back Normal. */
    var showStats: Boolean
        get() = hudLevel != HUD_OFF
        set(value) {
            if (value == showStats) return
            hudLevel = if (value) HUD_NORMAL else HUD_OFF
        }

    /**
     * Debug-only tools in the match: the CAPTURE button, macro record and play
     * in the pause menu, the HUD's detail card and console, and the controller
     * as player 2. Off for players; the adb macro control works either way.
     */
    var developerTools: Boolean
        get() = store.getBoolean(KEY_DEV_TOOLS, false)
        set(value) = store.edit().putBoolean(KEY_DEV_TOOLS, value).apply()

    /**
     * Lock the display and presentation pipeline to 60 FPS.
     * The console's frame is a 60 Hz frame; locking to 60 avoids erratic
     * frame pacing, thermal throttling, and excessive battery drain.
     */
    var prefer60Hz: Boolean
        get() = store.getBoolean(KEY_60HZ, true)
        set(value) = store.edit().putBoolean(KEY_60HZ, value).apply()

    // ---- sound ------------------------------------------------------------

    var audioEnabled: Boolean
        get() = store.getBoolean(KEY_AUDIO, true)
        set(value) = store.edit().putBoolean(KEY_AUDIO, value).apply()

    // ---- controls ---------------------------------------------------------

    /** The on-screen pad. Off is for playing with a physical controller. */
    var touchControls: Boolean
        get() = store.getBoolean(KEY_TOUCH, true)
        set(value) = store.edit().putBoolean(KEY_TOUCH, value).apply()

    /**
     * Whether a physical controller is listened to at all. Off is not a
     * nicety: a phone that reports an accessory, or its own sensors, as a
     * gamepad pushes a stick nobody is touching, and the character walks into
     * a wall for the whole match with nothing on screen to explain it.
     */
    var gamepadEnabled: Boolean
        get() = store.getBoolean(KEY_GAMEPAD, true)
        set(value) = store.edit().putBoolean(KEY_GAMEPAD, value).apply()

    /**
     * Test aid: physical pads start at player 2, so the touch controls keep
     * player 1 and one phone can drive both sides of a scenario.
     */
    var padAsPlayer2: Boolean
        get() = store.getBoolean(KEY_PAD_P2, false)
        set(value) = store.edit().putBoolean(KEY_PAD_P2, value).apply()

    var vibration: Boolean
        get() = store.getBoolean(KEY_VIBRATION, true)
        set(value) = store.edit().putBoolean(KEY_VIBRATION, value).apply()

    /** On-screen button names: PlayStation (Cross, Circle, L1) by default, or Xbox (A, B, LB). */
    var playStationButtons: Boolean
        get() = store.getString(KEY_BUTTON_STYLE, STYLE_PLAYSTATION) != STYLE_XBOX
        set(value) = store.edit().putString(KEY_BUTTON_STYLE, if (value) STYLE_PLAYSTATION else STYLE_XBOX).apply()

    var lookSensitivityX: Float
        get() = store.getFloat(KEY_LOOK_X, 1.0f)
        set(value) = store.edit().putFloat(KEY_LOOK_X, value.coerceIn(0.2f, 4.0f)).apply()

    var lookSensitivityY: Float
        get() = store.getFloat(KEY_LOOK_Y, 1.0f)
        set(value) = store.edit().putFloat(KEY_LOOK_Y, value.coerceIn(0.2f, 4.0f)).apply()

    /** 0 is raw, 1 is very smooth. A little hides the touchscreen's jitter. */
    var lookSmoothing: Float
        get() = store.getFloat(KEY_LOOK_SMOOTH, 0.35f)
        set(value) = store.edit().putFloat(KEY_LOOK_SMOOTH, value.coerceIn(0f, 0.9f)).apply()

    var invertLook: Boolean
        get() = store.getBoolean(KEY_LOOK_INVERT, false)
        set(value) = store.edit().putBoolean(KEY_LOOK_INVERT, value).apply()

    /** How far a fast flick pushes the stick: 1.0 reaches the edge readily. */
    var lookSaturation: Float
        get() = store.getFloat(KEY_LOOK_SATURATION, 1.0f)
        set(value) = store.edit().putFloat(KEY_LOOK_SATURATION, value.coerceIn(0.3f, 3.0f)).apply()

    // ---- the graphics driver ------------------------------------------------

    /** The folder of the driver the player picked, or "" for the system's. */
    var driverDirectory: String
        get() = store.getString(KEY_DRIVER_DIR, "") ?: ""
        set(value) = store.edit().putString(KEY_DRIVER_DIR, value).apply()

    var driverLibrary: String
        get() = store.getString(KEY_DRIVER_LIB, "") ?: ""
        set(value) = store.edit().putString(KEY_DRIVER_LIB, value).apply()

    // ---- where the game lives ------------------------------------------------

    /** Set once the files are in place; empty means "not installed yet". */
    var gameDirectory: String
        get() = store.getString(KEY_GAME_DIR, "") ?: ""
        set(value) = store.edit().putString(KEY_GAME_DIR, value).apply()

    /** The folder the file browser last showed, so it opens where it left off. */
    var lastBrowsed: String
        get() = store.getString(KEY_LAST_BROWSED, "") ?: ""
        set(value) = store.edit().putString(KEY_LAST_BROWSED, value).apply()

    /** The disc image or folder the player last picked to install from. */
    var lastSource: String
        get() = store.getString(KEY_LAST_SOURCE, "") ?: ""
        set(value) = store.edit().putString(KEY_LAST_SOURCE, value).apply()

    /** Set once the first-run driver download has been tried, hit or miss. */
    var driverAutoTried: Boolean
        get() = store.getBoolean(KEY_DRIVER_AUTO, false)
        set(value) = store.edit().putBoolean(KEY_DRIVER_AUTO, value).apply()

    /** The update this phone was told about and skipped, by tag. */
    var seenUpdateTag: String
        get() = store.getString(KEY_UPDATE_TAG, "") ?: ""
        set(value) = store.edit().putString(KEY_UPDATE_TAG, value).apply()

    /** Shown once, the first time the player opens the app. */
    var seenWelcome: Boolean
        get() = store.getBoolean(KEY_WELCOME, false)
        set(value) = store.edit().putBoolean(KEY_WELCOME, value).apply()

    var autoLaunch: Boolean
        get() = store.getBoolean(KEY_AUTO_LAUNCH, true)
        set(value) = store.edit().putBoolean(KEY_AUTO_LAUNCH, value).apply()

    /**
     * Compile every recorded pipeline synchronously at boot instead of the
     * small instant-boot budget (PES17_SHADER_PREWARM_ALL). Boot takes minutes
     * on a cold cache but there is no warmup debt left afterwards.
     */
    var prewarmAllShaders: Boolean
        get() = store.getBoolean(KEY_PREWARM_ALL, false)
        set(value) = store.edit().putBoolean(KEY_PREWARM_ALL, value).apply()

    companion object {
        private const val KEY_RESOLUTION = "resolutionScale"
        private const val KEY_SCENE_SCALE = "sceneScale"
        private const val KEY_BALL_SCALE = "ballScale"
        private const val KEY_FSR = "fsr"
        private const val KEY_FRAMEGEN = "frameGeneration"
        private const val KEY_DYNAMIC_SCALE = "dynamicScale"   // the first, single adaptive switch
        private const val KEY_HEAT_SCALE = "heatScale"
        private const val KEY_LOAD_SCALE = "loadScale"
        private const val KEY_PERF_HINT = "perfHint"
        private const val KEY_CROWD = "crowd"
        private const val KEY_FSR_SHARP = "fsrSharpness"
        private const val KEY_CAP30 = "cap30"
        private const val KEY_FPS_LIMIT = "fpsLimit"
        private const val KEY_WIDESCREEN = "widescreen"
        private const val KEY_LOW_MEMORY = "lowMemory"
        private const val KEY_PENDING_FOLDER = "pendingFolder"
        const val HUD_OFF = 0
        const val HUD_MINIMAL = 1
        const val HUD_NORMAL = 2
        const val HUD_COMPLETE = 3

        private const val KEY_RENDER_SCALE = "renderScale"
        private const val KEY_MSAA = "multisampling"
        private const val KEY_TEXTURE_MB = "textureBudgetMB"
        private const val KEY_SHOW_STATS = "showStats"
        private const val KEY_HUD_LEVEL = "hudLevel"
        private const val KEY_DEV_TOOLS = "developerTools"
        private const val KEY_60HZ = "prefer60Hz"
        private const val KEY_AUDIO = "audio"
        private const val KEY_TOUCH = "touchControls"
        private const val KEY_GAMEPAD = "gamepad"
        private const val KEY_PAD_P2 = "padAsPlayer2"
        private const val KEY_VIBRATION = "vibration"
        private const val KEY_BUTTON_STYLE = "buttonStyle"
        private const val STYLE_PLAYSTATION = "playstation"
        private const val STYLE_XBOX = "xbox"
        private const val KEY_LOOK_X = "lookX"
        private const val KEY_LOOK_Y = "lookY"
        private const val KEY_LOOK_SMOOTH = "lookSmoothing"
        private const val KEY_LOOK_INVERT = "lookInvert"
        private const val KEY_LOOK_SATURATION = "lookSaturation"
        private const val KEY_DRIVER_DIR = "driverDirectory"
        private const val KEY_DRIVER_LIB = "driverLibrary"
        private const val KEY_GAME_DIR = "gameDirectory"
        private const val KEY_LAST_BROWSED = "lastBrowsed"
        private const val KEY_LAST_SOURCE = "lastSource"
        private const val KEY_WELCOME = "seenWelcome"
        private const val KEY_AUTO_LAUNCH = "autoLaunch"
        private const val KEY_DRIVER_AUTO = "driverAutoTried"
        private const val KEY_UPDATE_TAG = "seenUpdateTag"
        private const val KEY_PREWARM_ALL = "prewarmAllShaders"
        private const val KEY_PRESET = "performancePreset"
        private const val KEY_GPU_BUILD = "gpuNameBuild"
        private const val KEY_GPU_NAME = "gpuName"
    }
}
