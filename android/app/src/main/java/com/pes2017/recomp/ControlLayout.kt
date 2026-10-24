package com.pes2017.recomp

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import kotlin.math.max
import kotlin.math.min

/** What a control does when it is touched. */
enum class ControlKind {
    /** A round button that holds one of the pad's bits while it is down. */
    BUTTON,

    /** A round button that holds a trigger down (0..255). */
    TRIGGER,

    /** The left stick: a base with a knob that follows the finger. */
    STICK,

    /** The look area: a region where dragging turns the view. */
    LOOK,
}

/**
 * One thing on screen. Positions are fractions of the window so a layout
 * made on one phone still makes sense on another, and sizes are in
 * density-independent pixels so a button is the same size in millimetres on
 * every screen.
 */
data class ControlElement(
    val id: String,
    val kind: ControlKind,
    /** The pad bit (BUTTON), or which trigger (TRIGGER: 0 left, 1 right). */
    val value: Int,
    val label: String,
    var xFrac: Float,
    var yFrac: Float,
    var sizeDp: Float,
    var alpha: Float = 0.45f,
    var visible: Boolean = true,
    /** Tap to hold, tap again to let go. */
    var toggle: Boolean = false,
    /** What it does on the ball (or, for a stick, a short name), drawn on the control. */
    val sub: String = "",
    /** What it does without the ball -- defending -- drawn smaller beneath. */
    val sub2: String = "",
)

/**
 * The whole on-screen pad: what it is, where it came from, and how it is
 * kept. Saved as json in the app's own folder, so a layout survives an
 * update, and one bad file never stops the game starting -- it falls back to
 * the defaults instead.
 */
class ControlLayout private constructor(
    val elements: MutableList<ControlElement>,
    /** The window the defaults were laid out for, so a reset puts things back there. */
    private val widthDp: Float,
    private val heightDp: Float,
) {

    fun find(id: String): ControlElement? = elements.firstOrNull { it.id == id }

    fun resetAll() {
        val fresh = defaults(widthDp, heightDp)
        elements.clear()
        elements.addAll(fresh.elements)
    }

    fun reset(id: String) {
        val fresh = defaults(widthDp, heightDp).find(id) ?: return
        val index = elements.indexOfFirst { it.id == id }
        if (index >= 0) elements[index] = fresh
    }

    fun toJson(): String {
        val array = JSONArray()
        for (element in elements) {
            val item = JSONObject()
            item.put("id", element.id)
            item.put("x", element.xFrac.toDouble())
            item.put("y", element.yFrac.toDouble())
            item.put("size", element.sizeDp.toDouble())
            item.put("alpha", element.alpha.toDouble())
            item.put("visible", element.visible)
            item.put("toggle", element.toggle)
            array.put(item)
        }
        val root = JSONObject()
        root.put("version", FORMAT_VERSION)
        root.put("elements", array)
        return root.toString(2)
    }

    fun save(context: Context) {
        try {
            val file = File(context.filesDir, FILE_NAME)
            // Written beside and moved into place: a layout being saved as
            // the process is killed must not leave a half-written file that
            // the next start cannot read.
            val temporary = File(context.filesDir, "$FILE_NAME.part")
            temporary.writeText(toJson())
            if (!temporary.renameTo(file)) {
                file.writeText(toJson())
                temporary.delete()
            }
        } catch (_: Throwable) {
            // A layout that cannot be saved is a small loss; crashing over
            // it would be a large one.
        }
    }

    companion object {
        private const val FILE_NAME = "controls.json"
        private const val FORMAT_VERSION = 6

        fun load(context: Context): ControlLayout {
            // The whole screen (the match is full screen, into the cutout), in
            // landscape whichever way the phone was held when asked.
            val metrics = context.resources.displayMetrics
            var widthPx = metrics.widthPixels
            var heightPx = metrics.heightPixels
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
                try {
                    val bounds = context.getSystemService(android.view.WindowManager::class.java)
                        .maximumWindowMetrics.bounds
                    widthPx = bounds.width()
                    heightPx = bounds.height()
                } catch (_: Throwable) {
                }
            }
            val widthDp = max(widthPx, heightPx) / metrics.density
            val heightDp = min(widthPx, heightPx) / metrics.density
            val layout = defaults(widthDp, heightDp)
            val file = File(context.filesDir, FILE_NAME)
            if (!file.exists()) return layout
            try {
                val root = JSONObject(file.readText())
                val version = root.optInt("version", 1)
                if (version < FORMAT_VERSION) {
                    // An older layout (the shooter's look area, the d-pad, the old places): start over.
                    try { file.delete() } catch (_: Throwable) {}
                    return layout
                }
                val array = root.optJSONArray("elements") ?: return layout
                for (i in 0 until array.length()) {
                    val item = array.optJSONObject(i) ?: continue
                    // Only what the app still knows about: an element dropped
                    // in a later version is ignored rather than resurrected,
                    // and a new one keeps its default.
                    val element = layout.find(item.optString("id")) ?: continue
                    element.xFrac = item.optDouble("x", element.xFrac.toDouble())
                        .toFloat().coerceIn(0.02f, 0.98f)
                    element.yFrac = item.optDouble("y", element.yFrac.toDouble())
                        .toFloat().coerceIn(0.02f, 0.98f)
                    element.sizeDp = item.optDouble("size", element.sizeDp.toDouble())
                        .toFloat().coerceIn(32f, 900f)
                    element.alpha = item.optDouble("alpha", element.alpha.toDouble())
                        .toFloat().coerceIn(0.05f, 1f)
                    element.visible = item.optBoolean("visible", element.visible)
                    element.toggle = item.optBoolean("toggle", element.toggle)
                }
            } catch (_: Throwable) {
                return defaults(widthDp, heightDp)
            }
            return layout
        }

        /**
         * Where everything starts: a football pad laid out for thumbs, the way
         * PES is played on a phone. The left thumb has only the move stick. The
         * right thumb has everything else in an arc around the four actions:
         *
         *  - Cross / A (pass) at the bottom, where the thumb rests, with
         *    Square / X (lob, cross) beside it: close enough to roll between,
         *    with a gap a thumb does not bridge by accident.
         *  - R1 / RB (sprint, "run") bigger and left of the actions, so the
         *    thumb can hold it and slide onto pass or shoot.
         *  - L1 / LB (change player) above the actions, the next thing the thumb
         *    reaches for when defending.
         *  - The skill stick above the sprint button; L2 / R2 small, under the
         *    HUD and the MENU button at the top; start and select top centre.
         *
         * Positions are in dp from the nearest edge, so the thumb distances are
         * the same on every phone and a wider screen only opens up the middle.
         * Nothing touches (with the touch slack) from a 592 x 336 dp window up.
         *
         * The game's default binding, for reference: Cross pass, Circle shoot,
         * Square lob / cross, Triangle through pass, R1 sprint, L1 change player.
         */
        fun defaults(widthDp: Float = 800f, heightDp: Float = 360f): ControlLayout {
            val w = max(widthDp, 320f)
            val h = max(heightDp, 240f)

            /** A control at x, y dp: positive from the left / top, negative from the right / bottom. */
            fun place(
                id: String, kind: ControlKind, value: Int, label: String,
                x: Float, y: Float, size: Float, alpha: Float = 0.45f, visible: Boolean = true,
            ) = ControlElement(
                id, kind, value, label,
                (if (x >= 0f) x else w + x) / w, (if (y >= 0f) y else h + y) / h,
                size, alpha, visible,
            )

            val centre = w / 2f
            return ControlLayout(
                mutableListOf(
                    // Left thumb: movement.
                    place("stick", ControlKind.STICK, 0, "LS", 95f, -95f, 150f, 0.40f),

                    // Right thumb: the four actions as the pad's diamond.
                    place("a", ControlKind.BUTTON, NativeBridge.A, "A", -110f, -52f, 68f),
                    place("b", ControlKind.BUTTON, NativeBridge.B, "B", -52f, -110f, 68f),
                    place("x", ControlKind.BUTTON, NativeBridge.X, "X", -168f, -110f, 68f),
                    place("y", ControlKind.BUTTON, NativeBridge.Y, "Y", -110f, -168f, 68f),

                    // The shoulders where a pad has them: L2 / L1 along the top left,
                    // R1 / R2 along the top right, triggers outermost.
                    place("lt", ControlKind.TRIGGER, 0, "LT", 44f, 76f, 56f),
                    place("lb", ControlKind.BUTTON, NativeBridge.LB, "LB", 110f, 76f, 56f),
                    place("rb", ControlKind.BUTTON, NativeBridge.RB, "RB", -110f, 76f, 56f),
                    place("rt", ControlKind.TRIGGER, 1, "RT", -44f, 76f, 56f),

                    // The skill stick, left of the actions.
                    place("right_stick", ControlKind.STICK, 1, "RS", -250f, -170f, 100f, 0.40f),

                    // Stick clicks: kept for the editor, hidden until wanted.
                    place("l3", ControlKind.BUTTON, NativeBridge.L3, "L3", 200f, -40f, 52f, visible = false),
                    place("r3", ControlKind.BUTTON, NativeBridge.R3, "R3", -320f, -45f, 52f, visible = false),

                    // Menus have to be found at a glance in a football game.
                    place("start", ControlKind.BUTTON, NativeBridge.START, "START", centre + 60f, 37f, 54f, 0.75f),
                    place("back", ControlKind.BUTTON, NativeBridge.BACK, "SELECT", centre - 40f, 37f, 54f, 0.75f),
                ),
                widthDp, heightDp,
            )
        }

        /** The name printed on a control: its Xbox name, or the PlayStation one. */
        fun labelFor(element: ControlElement, playStation: Boolean): String {
            if (!playStation) return element.label
            return when (element.id) {
                "a" -> "CROSS"
                "b" -> "CIRCLE"
                "x" -> "SQUARE"
                "y" -> "TRIANGLE"
                "lb" -> "L1"
                "rb" -> "R1"
                "lt" -> "L2"
                "rt" -> "R2"
                "stick" -> "L"
                "right_stick" -> "R"
                "start" -> "START"
                "back" -> "SELECT"
                else -> element.label
            }
        }
    }
}
