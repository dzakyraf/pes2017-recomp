package com.pes2017.recomp

import android.content.Intent
import android.os.Bundle
import android.widget.Button
import android.widget.SeekBar
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.SwitchCompat
import kotlin.math.roundToInt

/**
 * Everything the player can change outside a match. Each setting is written
 * the moment it is touched: there is no "apply", because there is nothing
 * to apply to -- the values are read when a match starts (GameActivity).
 */
class SettingsActivity : AppCompatActivity() {

    companion object {
        const val EXTRA_TAB = "pes17.tab"
        const val TAB_GRAPHICS = 0
        const val TAB_CONTROLS = 1
        const val TAB_ADVANCED = 2
        const val TAB_FILES = 3
    }

    private lateinit var prefs: Prefs
    private val tabButtons = mutableListOf<Button>()
    private val tabPages = mutableListOf<android.view.View>()
    private lateinit var storageAccess: TextView
    private lateinit var grantStorage: Button
    private lateinit var savesSummary: TextView

    // Save backups (SaveBackup): written where the player picks, read from a file they pick.
    private val createBackup = registerForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.CreateDocument("application/zip"),
    ) { uri -> if (uri != null) backupTo(uri) }
    private val openBackup = registerForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.OpenDocument(),
    ) { uri -> if (uri != null) confirmRestore { contentResolver.openInputStream(uri) } }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_settings)
        // BACK on the strip along the top; through the dispatcher, so a screen's own back handling (the install's
        // "stop copying?") applies to it too.
        findViewById<android.view.View>(R.id.back_button).setOnClickListener { onBackPressedDispatcher.onBackPressed() }
        prefs = Prefs(this)
        title = getString(R.string.entry_settings)

        // ---- tab bar (plain views, no new deps): one page visible at a time ----
        listOf(R.id.tab_graphics, R.id.tab_controls, R.id.tab_advanced, R.id.tab_files)
            .forEach { tabButtons.add(findViewById(it)) }
        listOf(R.id.page_graphics, R.id.page_controls, R.id.page_advanced, R.id.page_files)
            .forEach { tabPages.add(findViewById(it)) }
        // Performance is part of the graphics tab: shown and hidden with it (selectTab).
        tabButtons.forEachIndexed { index, button -> button.setOnClickListener { selectTab(index) } }
        selectTab(intent.getIntExtra(EXTRA_TAB, TAB_GRAPHICS).coerceIn(0, tabPages.size - 1))

        // ---- graphics: preset, display, image quality ----
        presets()
        bar(R.id.resolution, R.id.resolution_label, 65, ((prefs.resolutionScale * 100f) - 35f).roundToInt(),
            { getString(R.string.resolution, it + 35) }) { prefs.resolutionScale = (it + 35) / 100f }
        switch(R.id.widescreen, prefs.widescreen) { prefs.widescreen = it }
        bar(R.id.ball_scale, R.id.ball_scale_label, 20, (prefs.ballScalePercent - 100) / 10,
            { getString(R.string.ball_scale, 100 + it * 10) }) { prefs.ballScalePercent = 100 + it * 10 }
        bar(R.id.scene_scale, R.id.scene_scale_label, 10, (prefs.sceneScalePercent - 50) / 5,
            { getString(R.string.scene_scale, 50 + it * 5) }) { prefs.sceneScalePercent = 50 + it * 5; presetTouched() }
        switch(R.id.msaa, prefs.multisampling) { prefs.multisampling = it; presetTouched() }
        switch(R.id.fsr, prefs.fsr) { prefs.fsr = it; presetTouched() }
        switch(R.id.frame_gen, prefs.frameGeneration) { prefs.frameGeneration = it }
        switch(R.id.black_guard, prefs.blackGuard) { prefs.blackGuard = it }
        // Experiments: arena size (Auto, 128, 192, 256, 320 MB) and four switches the runtime already has.
        val arenaIds = intArrayOf(R.id.arena_auto, R.id.arena_128, R.id.arena_192, R.id.arena_256, R.id.arena_320)
        val arenaSizes = intArrayOf(0, 128, 192, 256, 320)
        val arena: android.widget.RadioGroup = findViewById(R.id.arena_size)
        arena.check(arenaIds[arenaSizes.indexOf(prefs.arenaMB).coerceAtLeast(0)])
        arena.setOnCheckedChangeListener { _, id -> prefs.arenaMB = arenaSizes[arenaIds.indexOf(id).coerceAtLeast(0)] }
        switch(R.id.exp_legacy, prefs.expLegacy) { prefs.expLegacy = it }
        switch(R.id.exp_no_table, prefs.expNoTable) { prefs.expNoTable = it }
        switch(R.id.exp_bc_hw, prefs.expBcHw) { prefs.expBcHw = it }
        switch(R.id.exp_dynamic_depth, prefs.expDynamicDepth) { prefs.expDynamicDepth = it }
        bar(R.id.fsr_sharp, R.id.fsr_sharp_label, 20, (prefs.fsrSharpness * 10f).roundToInt(),
            { getString(R.string.fsr_sharp, it / 10f) }) { prefs.fsrSharpness = it / 10f }

        // ---- performance: frame rate, heat and battery ----
        bar(R.id.fps_limit, R.id.fps_limit_label, 6, (prefs.fpsLimit - 30) / 5,
            { if (it == 6) getString(R.string.fps_limit_off) else getString(R.string.fps_limit, 30 + it * 5) }) {
            prefs.fpsLimit = 30 + it * 5; presetTouched()
        }
        switch(R.id.prefer60, prefs.prefer60Hz) { prefs.prefer60Hz = it }
        // Off / Minimal / Normal / Complete, in Prefs.HUD_* order.
        val hudChoices = intArrayOf(R.id.hud_off, R.id.hud_minimal, R.id.hud_normal, R.id.hud_complete)
        val hudLevel: android.widget.RadioGroup = findViewById(R.id.hud_level)
        hudLevel.check(hudChoices[prefs.hudLevel])
        hudLevel.setOnCheckedChangeListener { _, id -> prefs.hudLevel = hudChoices.indexOf(id).coerceAtLeast(0) }
        switch(R.id.heat_scale, prefs.heatScale) { prefs.heatScale = it; presetTouched() }
        switch(R.id.perf_hint, prefs.perfHint) { prefs.perfHint = it }
        switch(R.id.crowd, prefs.crowd) { prefs.crowd = it }

        // ---- controls and sound ----
        switch(R.id.touch, prefs.touchControls) { prefs.touchControls = it }
        switch(R.id.gamepad, prefs.gamepadEnabled) { prefs.gamepadEnabled = it }
        switch(R.id.vibration, prefs.vibration) { prefs.vibration = it }
        val buttonStyle: android.widget.RadioGroup = findViewById(R.id.button_style)
        buttonStyle.check(if (prefs.playStationButtons) R.id.button_style_playstation else R.id.button_style_xbox)
        buttonStyle.setOnCheckedChangeListener { _, id -> prefs.playStationButtons = id == R.id.button_style_playstation }
        findViewById<Button>(R.id.arrange_controls).setOnClickListener {
            startActivity(Intent(this, ControlsEditorActivity::class.java))
        }
        switch(R.id.audio, prefs.audioEnabled) { prefs.audioEnabled = it }

        // ---- advanced: rendering, memory, testing ----
        bar(R.id.render_scale, R.id.render_scale_label, 2, prefs.renderScale - 1,
            { getString(R.string.render_scale, it + 1) }) { prefs.renderScale = it + 1 }
        switch(R.id.load_scale, prefs.loadScale) { prefs.loadScale = it; presetTouched() }
        switch(R.id.low_memory, prefs.lowMemory) { prefs.lowMemory = it; presetTouched() }
        // 0 means "let the runtime decide"; then 128 MB steps to 2 GB.
        bar(R.id.texture_budget, R.id.texture_label, 15,
            if (prefs.textureBudgetMB <= 0) 0 else (prefs.textureBudgetMB / 128).coerceIn(1, 15),
            { textureText(it) }) { prefs.textureBudgetMB = if (it == 0) 0 else it * 128 }
        switch(R.id.prewarm_all, prefs.prewarmAllShaders) { prefs.prewarmAllShaders = it }
        switch(R.id.gamepad_p2, prefs.padAsPlayer2) { prefs.padAsPlayer2 = it }
        // The test options only exist for someone who turned the developer tools on.
        // The bug-hunting experiments (arena size, render path, the black-frame guard) are the same: hidden
        // until then, and all off by default.
        val testing = listOf<android.view.View>(
            findViewById(R.id.testing_heading), findViewById(R.id.testing_group),
            findViewById(R.id.experiments_heading), findViewById(R.id.experiments_group),
            findViewById(R.id.black_guard), findViewById(R.id.black_guard_hint),
        )
        fun showTesting(on: Boolean) = testing.forEach { it.visibility = if (on) android.view.View.VISIBLE else android.view.View.GONE }
        showTesting(prefs.developerTools)
        switch(R.id.developer_tools, prefs.developerTools) { prefs.developerTools = it; showTesting(it) }

        // ---- the game files ----
        val paths: TextView = findViewById(R.id.paths)
        storageAccess = findViewById(R.id.storage_access)
        grantStorage = findViewById(R.id.grant_storage)
        grantStorage.setOnClickListener {
            val intent = GameFolder.accessIntent(this)
            if (intent != null) {
                try {
                    startActivity(intent)
                } catch (_: Throwable) {
                    try {
                        startActivity(Intent(android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
                    } catch (_: Throwable) {
                    }
                }
            } else {
                requestPermissions(GameFolder.legacyPermissions, 1)
            }
        }
        findViewById<Button>(R.id.change_folder).setOnClickListener {
            // The folder picker lives in the main menu, where the permission
            // flow and the check of default.xex are.
            finish()
        }
        findViewById<Button>(R.id.forget).setOnClickListener {
            AlertDialog.Builder(this)
                .setTitle(R.string.forget_install)
                .setMessage(R.string.forget_explained)
                .setNegativeButton(R.string.cancel, null)
                .setPositiveButton(R.string.reset) { _, _ ->
                    prefs.gameDirectory = ""
                    paths.text = getString(R.string.paths_none)
                }
                .show()
        }
        paths.text = pathsText()

        // ---- save data ----
        savesSummary = findViewById(R.id.saves_summary)
        refreshSaves()
        findViewById<Button>(R.id.backup_saves).setOnClickListener {
            createBackup.launch(SaveBackup.suggestedName())
        }
        findViewById<Button>(R.id.restore_saves).setOnClickListener { chooseRestore() }

        // ---- storage ----
        val storage: TextView = findViewById(R.id.storage)
        refreshStorage(storage)
        findViewById<Button>(R.id.clear_shaders).setOnClickListener {
            shaderCacheFiles().forEach { try { it.delete() } catch (_: Throwable) {} }
            storage.text = getString(R.string.shaders_cleared)
            refreshStorage(storage)
        }
        findViewById<Button>(R.id.manage_captures).setOnClickListener {
            startActivity(Intent(this, CapturesActivity::class.java))
        }
        findViewById<Button>(R.id.clear_logs).setOnClickListener {
            listOf("game.log", "launcher.log", "last-crash-game.txt", "last-crash-launcher.txt").forEach {
                try {
                    java.io.File(filesDir, it).delete()
                } catch (_: Throwable) {
                }
            }
            storage.text = getString(R.string.logs_cleared)
            refreshStorage(storage)
        }
    }

    override fun onResume() {
        super.onResume()
        // Back from the permission screen.
        val granted = GameFolder.hasAccess(this)
        storageAccess.setText(if (granted) R.string.storage_granted else R.string.storage_not_granted)
        grantStorage.visibility = if (granted) android.view.View.GONE else android.view.View.VISIBLE
    }

    /**
     * Low / Medium / High (DeviceProfile): a button writes its tier's settings and
     * the page is built again, so every slider and switch shows what it set.
     */
    private fun presets() {
        DeviceProfile.ensurePreset(this, prefs)
        val detected = DeviceProfile.detect(this, prefs)
        val specs = detected.specs
        findViewById<TextView>(R.id.preset_detected).text = getString(
            R.string.preset_detected,
            tierName(detected.tier.name), detected.reason,
            specs.gpu.ifEmpty { getString(R.string.preset_gpu_unknown) },
            specs.fastCores, specs.fastestMHz, specs.totalMemoryMB / 1024.0,
        )
        presetLabel = findViewById(R.id.preset_label)
        presetLabel.text = getString(R.string.preset_label, tierName(prefs.preset))
        listOf(
            R.id.preset_low to DeviceProfile.Tier.LOW,
            R.id.preset_medium to DeviceProfile.Tier.MEDIUM,
            R.id.preset_high to DeviceProfile.Tier.HIGH,
        ).forEach { (id, tier) ->
            findViewById<Button>(id).setOnClickListener {
                DeviceProfile.apply(prefs, tier)
                intent.putExtra(EXTRA_TAB, TAB_GRAPHICS)
                recreate()
            }
        }
    }

    private lateinit var presetLabel: TextView


    /** One of a preset's settings moved by hand: it is the player's own now. */
    private fun presetTouched() {
        if (prefs.preset == DeviceProfile.PRESET_CUSTOM) return
        prefs.preset = DeviceProfile.PRESET_CUSTOM
        presetLabel.text = getString(R.string.preset_label, tierName(prefs.preset))
    }

    private fun tierName(name: String): String = getString(
        when (name) {
            DeviceProfile.Tier.LOW.name -> R.string.preset_low
            DeviceProfile.Tier.MEDIUM.name -> R.string.preset_medium
            DeviceProfile.Tier.HIGH.name -> R.string.preset_high
            else -> R.string.preset_custom
        },
    )

    private fun refreshSaves() {
        val saves = SaveBackup.savesDir(this, prefs)
        val packages = SaveBackup.packages(saves)
        savesSummary.text =
            if (packages.isEmpty()) getString(R.string.saves_none, saves.absolutePath)
            else getString(R.string.saves_summary, packages.size, saves.absolutePath, packages.joinToString(", "))
    }

    private fun backupTo(uri: android.net.Uri) {
        val saves = SaveBackup.savesDir(this, prefs)
        Thread {
            val message = try {
                val count = contentResolver.openOutputStream(uri)?.use { SaveBackup.write(saves, it) }
                    ?: throw IllegalStateException("cannot write there")
                getString(R.string.backup_done, count)
            } catch (t: Throwable) {
                getString(R.string.backup_failed, t.message ?: t.javaClass.simpleName)
            }
            runOnUiThread { android.widget.Toast.makeText(this, message, android.widget.Toast.LENGTH_LONG).show() }
        }.start()
    }

    /** A file the player picks, or one of the automatic backups, newest first. */
    private fun chooseRestore() {
        val automatic = SaveBackup.automaticBackups(this)
        val names = listOf(getString(R.string.restore_from_file)) +
            automatic.map { getString(R.string.restore_auto_entry, it.name.removePrefix("auto-").removeSuffix(".zip")) }
        AlertDialog.Builder(this)
            .setTitle(R.string.restore_saves)
            .setItems(names.toTypedArray()) { _, which ->
                if (which == 0) openBackup.launch(arrayOf("application/zip", "application/octet-stream"))
                else confirmRestore { automatic[which - 1].inputStream() }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun confirmRestore(open: () -> java.io.InputStream?) {
        if (SaveBackup.gameRunning(this)) {
            AlertDialog.Builder(this).setMessage(R.string.restore_game_running).setPositiveButton(android.R.string.ok, null).show()
            return
        }
        AlertDialog.Builder(this)
            .setTitle(R.string.restore_confirm_title)
            .setMessage(R.string.restore_confirm)
            .setNegativeButton(R.string.cancel, null)
            .setPositiveButton(R.string.restore) { _, _ ->
                val saves = SaveBackup.savesDir(this, prefs)
                Thread {
                    val message = try {
                        val count = open()?.use { SaveBackup.restore(saves, it) }
                            ?: throw IllegalStateException("cannot read the backup")
                        getString(R.string.restore_done, count)
                    } catch (t: Throwable) {
                        getString(R.string.restore_failed, t.message ?: t.javaClass.simpleName)
                    }
                    runOnUiThread {
                        android.widget.Toast.makeText(this, message, android.widget.Toast.LENGTH_LONG).show()
                        refreshSaves()
                    }
                }.start()
            }
            .show()
    }

    private fun pathsText(): String =
        if (prefs.gameDirectory.isEmpty()) getString(R.string.paths_none)
        else getString(R.string.paths_line, prefs.gameDirectory)

    /** Custom tab bar: highlight bar on the chosen tab, one page shown. */
    private fun selectTab(index: Int) {
        tabPages.forEachIndexed { i, page ->
            page.visibility = if (i == index) android.view.View.VISIBLE else android.view.View.GONE
        }
        findViewById<android.view.View>(R.id.page_performance).visibility =
            if (index == TAB_GRAPHICS) android.view.View.VISIBLE else android.view.View.GONE
        tabButtons.forEachIndexed { i, button ->
            if (i == index) {
                button.setBackgroundResource(R.drawable.bg_tab_selected)
                button.setTextColor(getColor(R.color.pes_gold))
            } else {
                button.setBackgroundColor(android.graphics.Color.TRANSPARENT)
                button.setTextColor(getColor(R.color.pes_on_dark))
            }
        }
    }

    /** A slider whose label follows it; `onChange` only for the player's own touches. */
    private fun bar(
        barId: Int, labelId: Int, max: Int, initial: Int,
        label: (Int) -> String, onChange: (Int) -> Unit,
    ) {
        val text: TextView = findViewById(labelId)
        val seek: SeekBar = findViewById(barId)
        seek.max = max
        seek.progress = initial.coerceIn(0, max)
        text.text = label(seek.progress)
        seek.setOnSeekBarChangeListener(object : Simple() {
            override fun onProgressChanged(view: SeekBar, value: Int, fromUser: Boolean) {
                text.text = label(value)
                if (fromUser) onChange(value)
            }
        })
    }

    private fun gameBase(): java.io.File = getExternalFilesDir(null) ?: filesDir

    private fun shaderCacheFiles(): List<java.io.File> {
        val base = gameBase()
        return listOf(
            java.io.File(base, "shader_cache.bin"),
            java.io.File(filesDir, "shader_cache.bin"),
            java.io.File(filesDir, "pipeline.cache"),
        )
    }

    private fun refreshStorage(view: TextView) {
        view.text = getString(R.string.storage_computing)
        Thread {
            fun sizeOf(dir: java.io.File): Long = try {
                if (dir.isFile) dir.length()
                else dir.walkTopDown().filter { it.isFile }.sumOf { it.length() }
            } catch (_: Throwable) {
                0L
            }
            val gameDir = prefs.gameDirectory.ifEmpty { null }?.let { java.io.File(it) }
            val base = gameBase()
            val saves = java.io.File(
                (gameDir?.parent ?: base.absolutePath) + "/saves",
            )
            val rows = listOf(
                getString(R.string.section_files) to (gameDir?.let { sizeOf(it) } ?: 0L),
                getString(R.string.saves_files) to sizeOf(saves),
                getString(R.string.clear_shaders) to shaderCacheFiles().sumOf {
                    try {
                        it.length()
                    } catch (_: Throwable) {
                        0L
                    }
                },
                getString(R.string.view_log) to listOf(
                    "game.log", "launcher.log", "last-crash-game.txt", "last-crash-launcher.txt",
                ).sumOf {
                    try {
                        java.io.File(filesDir, it).length()
                    } catch (_: Throwable) {
                        0L
                    }
                },
            )
            val text = rows.joinToString("\n") { (label, bytes) ->
                getString(R.string.storage_line, label, formatSize(bytes))
            }
            runOnUiThread { view.text = text }
        }.start()
    }

    private fun formatSize(bytes: Long): String =
        android.text.format.Formatter.formatFileSize(this, bytes)

    private fun textureText(step: Int): String =
        if (step == 0) getString(R.string.texture_budget_auto)
        else getString(R.string.texture_budget, step * 128)

    private fun switch(id: Int, initial: Boolean, onChange: (Boolean) -> Unit) {
        val view: SwitchCompat = findViewById(id)
        view.isChecked = initial
        view.setOnCheckedChangeListener { _, checked -> onChange(checked) }
    }

    /** The two methods nobody ever needs, out of the way. */
    private abstract class Simple : SeekBar.OnSeekBarChangeListener {
        override fun onStartTrackingTouch(bar: SeekBar) = Unit
        override fun onStopTrackingTouch(bar: SeekBar) = Unit
    }
}
