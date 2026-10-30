package com.pes2017.recomp

import android.app.Activity
import android.content.Intent
import android.view.View
import android.widget.CompoundButton
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.widget.SwitchCompat
import kotlin.math.roundToInt

/**
 * The graphics and performance settings that matter most, reachable from the
 * pause menu. The renderer reads them once as a run starts, so changing them
 * means a new run: "Apply & restart" hands the values to the launcher (the one
 * process that writes the settings) and starts the match again, with no trip
 * through the launcher's screens.
 */
object QuickGraphics {

    const val EXTRA_RELAUNCH = "pes17.relaunch"
    private const val X_SCENE = "gfx_scene"
    private const val X_SCALE = "gfx_scale"
    private const val X_MSAA = "gfx_msaa"
    private const val X_FSR = "gfx_fsr"
    private const val X_FPS = "gfx_fps"
    private const val X_HEAT = "gfx_heat"
    private const val X_LOADSCALE = "gfx_loadscale"
    private const val X_CROWD = "gfx_crowd"
    private const val X_LOWMEM = "gfx_lowmem"
    private const val X_PRESET = "gfx_preset"

    private class Values(
        var scene: Int, var scale: Int, var msaa: Boolean, var fsr: Boolean, var fps: Int,
        var heat: Boolean, var loadScale: Boolean, var crowd: Boolean, var lowMemory: Boolean,
        var preset: String,
    )

    /** Shows the dialog; `onApply` runs after the relaunch has been requested. */
    fun show(activity: Activity, prefs: Prefs, onApply: () -> Unit, onClose: () -> Unit) {
        val v = Values(
            prefs.sceneScalePercent, prefs.renderScale, prefs.multisampling, prefs.fsr, prefs.fpsLimit,
            prefs.heatScale, prefs.loadScale, prefs.crowd, prefs.lowMemory, prefs.preset,
        )
        val dp = activity.resources.displayMetrics.density
        fun px(value: Int) = (value * dp).roundToInt()

        val column = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(px(20), px(8), px(20), px(8))
        }
        fun label(text: String) = TextView(activity).apply { this.text = text; textSize = 13f; setPadding(0, px(10), 0, 0) }

        // Preset first: one tap sets everything below it, which can then be nudged.
        val presetGroup = RadioGroup(activity).apply { orientation = RadioGroup.HORIZONTAL }
        val tiers = DeviceProfile.Tier.values()
        val tierButtons = tiers.map { tier ->
            RadioButton(activity).apply { text = tier.name.lowercase().replaceFirstChar { it.uppercase() }; id = View.generateViewId() }
        }
        tierButtons.forEach { presetGroup.addView(it) }
        column.addView(label(activity.getString(R.string.qg_preset)))
        column.addView(presetGroup)

        val sceneText = label("")
        val scene = SeekBar(activity).apply { max = 10 }
        val scaleText = label("")
        val scale = SeekBar(activity).apply { max = 2 }
        val fpsText = label("")
        val fps = SeekBar(activity).apply { max = 6 }
        fun switch(text: String) = SwitchCompat(activity).apply { this.text = text; setPadding(0, px(6), 0, px(6)) }
        val msaa = switch(activity.getString(R.string.qg_msaa))
        val fsr = switch(activity.getString(R.string.qg_fsr))
        val heat = switch(activity.getString(R.string.qg_heat))
        val crowd = switch(activity.getString(R.string.qg_crowd))
        val lowMem = switch(activity.getString(R.string.qg_lowmem))

        fun refresh() {
            sceneText.text = activity.getString(R.string.qg_scene, v.scene)
            fpsText.text = activity.getString(R.string.qg_fps, v.fps)
            scaleText.text = activity.getString(R.string.qg_scale, v.scale)
            scale.progress = v.scale - 1
            scene.isEnabled = v.scale == 1
            scene.progress = (v.scene - 50) / 5
            fps.progress = (v.fps - 30) / 5
            msaa.isChecked = v.msaa; fsr.isChecked = v.fsr; heat.isChecked = v.heat
            crowd.isChecked = v.crowd; lowMem.isChecked = v.lowMemory
            tiers.forEachIndexed { i, tier -> tierButtons[i].isChecked = v.preset == tier.name }
        }

        var quiet = false
        tierButtons.forEachIndexed { i, button ->
            button.setOnClickListener {
                val p = DeviceProfile.preset(tiers[i])
                v.scene = p.sceneScalePercent; v.scale = 1; v.msaa = p.multisampling; v.fsr = p.fsr; v.fps = p.fpsLimit
                v.heat = p.heatScale; v.loadScale = p.loadScale; v.lowMemory = p.lowMemory
                v.preset = tiers[i].name
                quiet = true; refresh(); quiet = false
            }
        }
        fun touched() { if (!quiet) { v.preset = DeviceProfile.PRESET_CUSTOM; tierButtons.forEach { it.isChecked = false } } }
        scene.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                if (!fromUser) return
                v.scene = 50 + value * 5; sceneText.text = activity.getString(R.string.qg_scene, v.scene); touched()
            }
            override fun onStartTrackingTouch(bar: SeekBar) = Unit
            override fun onStopTrackingTouch(bar: SeekBar) = Unit
        })
        scale.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                if (!fromUser) return
                v.scale = value + 1
                scaleText.text = activity.getString(R.string.qg_scale, v.scale)
                scene.isEnabled = v.scale == 1
                touched()
            }
            override fun onStartTrackingTouch(bar: SeekBar) = Unit
            override fun onStopTrackingTouch(bar: SeekBar) = Unit
        })
        fps.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(bar: SeekBar, value: Int, fromUser: Boolean) {
                if (!fromUser) return
                v.fps = 30 + value * 5; fpsText.text = activity.getString(R.string.qg_fps, v.fps); touched()
            }
            override fun onStartTrackingTouch(bar: SeekBar) = Unit
            override fun onStopTrackingTouch(bar: SeekBar) = Unit
        })
        fun bind(button: CompoundButton, set: (Boolean) -> Unit) =
            button.setOnCheckedChangeListener { _, on -> if (!quiet) { set(on); touched() } }
        bind(msaa) { v.msaa = it }; bind(fsr) { v.fsr = it }; bind(heat) { v.heat = it }
        bind(crowd) { v.crowd = it }; bind(lowMem) { v.lowMemory = it }

        column.addView(scaleText); column.addView(scale)
        column.addView(sceneText); column.addView(scene)
        column.addView(fpsText); column.addView(fps)
        listOf(msaa, fsr, heat, crowd, lowMem).forEach { column.addView(it) }
        column.addView(TextView(activity).apply {
            setText(R.string.qg_note); textSize = 11f; alpha = 0.7f; setPadding(0, px(10), 0, 0)
        })
        quiet = true; refresh(); quiet = false

        AlertDialog.Builder(activity)
            .setTitle(R.string.qg_title)
            .setView(ScrollView(activity).apply { addView(column) })
            .setNegativeButton(R.string.cancel, null)
            .setPositiveButton(R.string.qg_apply) { _, _ ->
                val intent = Intent(activity, MainActivity::class.java)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
                    .putExtra(EXTRA_RELAUNCH, true)
                    .putExtra(X_SCENE, v.scene).putExtra(X_SCALE, v.scale).putExtra(X_MSAA, v.msaa).putExtra(X_FSR, v.fsr)
                    .putExtra(X_FPS, v.fps).putExtra(X_HEAT, v.heat).putExtra(X_LOADSCALE, v.loadScale)
                    .putExtra(X_CROWD, v.crowd).putExtra(X_LOWMEM, v.lowMemory).putExtra(X_PRESET, v.preset)
                activity.startActivity(intent)
                onApply()
            }
            .setOnDismissListener { onClose() }
            .show()
    }

    /** In the launcher: store what the match sent. True when this intent asked for a relaunch. */
    fun takeRelaunch(intent: Intent?, prefs: Prefs): Boolean {
        if (intent == null || !intent.getBooleanExtra(EXTRA_RELAUNCH, false)) return false
        intent.removeExtra(EXTRA_RELAUNCH)
        if (intent.hasExtra(X_SCENE)) {
            prefs.sceneScalePercent = intent.getIntExtra(X_SCENE, prefs.sceneScalePercent)
            prefs.renderScale = intent.getIntExtra(X_SCALE, prefs.renderScale)
            prefs.multisampling = intent.getBooleanExtra(X_MSAA, prefs.multisampling)
            prefs.fsr = intent.getBooleanExtra(X_FSR, prefs.fsr)
            prefs.fpsLimit = intent.getIntExtra(X_FPS, prefs.fpsLimit)
            prefs.heatScale = intent.getBooleanExtra(X_HEAT, prefs.heatScale)
            prefs.loadScale = intent.getBooleanExtra(X_LOADSCALE, prefs.loadScale)
            prefs.crowd = intent.getBooleanExtra(X_CROWD, prefs.crowd)
            prefs.lowMemory = intent.getBooleanExtra(X_LOWMEM, prefs.lowMemory)
            prefs.preset = intent.getStringExtra(X_PRESET) ?: prefs.preset
        }
        return true
    }
}
