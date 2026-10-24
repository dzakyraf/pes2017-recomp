package com.pes2017.recomp

import android.content.Intent
import android.os.Bundle
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import kotlin.math.roundToInt

/**
 * The debugging captures on this phone (CAPTURE button in a match): see them, send one, save one to a
 * folder of the player's choice, delete one or all. Opened from Settings > Storage.
 */
class CapturesActivity : AppCompatActivity() {

    private lateinit var summary: TextView
    private lateinit var list: ListView
    private lateinit var clearAll: Button
    private var items: List<Captures.Item> = emptyList()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_captures)
        findViewById<View>(R.id.back_button).setOnClickListener { onBackPressedDispatcher.onBackPressed() }
        title = getString(R.string.captures_title)
        // Zips left by an earlier share. Not removed when the screen is left: the share sheet opens over
        // it and the app picked there reads the zip after that.
        Thread { Captures.deleteZips(this) }.start()
        summary = findViewById(R.id.captures_summary)
        list = findViewById(R.id.captures_list)
        clearAll = findViewById(R.id.captures_clear)
        clearAll.setOnClickListener { confirmClearAll() }
        list.setOnItemClickListener { _, _, position, _ -> actions(items[position]) }
        list.setOnItemLongClickListener { _, _, position, _ -> confirmDelete(items[position]); true }
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    private fun refresh() {
        Thread {
            val found = Captures.list(this)
            runOnUiThread {
                items = found
                summary.text = if (found.isEmpty()) getString(R.string.captures_none)
                else getString(R.string.captures_summary, found.size, Captures.size(found.sumOf { it.bytes }))
                clearAll.isEnabled = found.isNotEmpty()
                list.adapter = Adapter()
            }
        }.start()
    }

    private fun actions(item: Captures.Item) {
        AlertDialog.Builder(this)
            .setTitle(item.name)
            .setItems(arrayOf(
                getString(R.string.capture_share),
                getString(R.string.captures_save_folder),
                getString(R.string.captures_delete),
            )) { _, which ->
                when (which) {
                    0 -> Thread {
                        val ok = Captures.share(this, item.folder)
                        if (!ok) runOnUiThread { Toast.makeText(this, R.string.captures_save_failed, Toast.LENGTH_LONG).show() }
                    }.start()
                    1 -> Thread {
                        val where = Captures.saveToDownloads(this, item.folder)
                        Captures.deleteZips(this)
                        runOnUiThread {
                            Toast.makeText(
                                this,
                                if (where != null) getString(R.string.captures_saved_to, where) else getString(R.string.captures_save_failed),
                                Toast.LENGTH_LONG,
                            ).show()
                        }
                    }.start()
                    else -> confirmDelete(item)
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun confirmDelete(item: Captures.Item) {
        AlertDialog.Builder(this)
            .setTitle(item.name)
            .setMessage(R.string.captures_delete_one)
            .setNegativeButton(R.string.cancel, null)
            .setPositiveButton(R.string.captures_delete) { _, _ ->
                Thread { Captures.delete(item.folder); runOnUiThread { refresh() } }.start()
            }
            .show()
    }

    private fun confirmClearAll() {
        AlertDialog.Builder(this)
            .setTitle(R.string.captures_clear_all)
            .setMessage(R.string.captures_clear_all_body)
            .setNegativeButton(R.string.cancel, null)
            .setPositiveButton(R.string.captures_clear_all) { _, _ ->
                Thread { Captures.deleteAll(this); runOnUiThread { refresh() } }.start()
            }
            .show()
    }

    private inner class Adapter : BaseAdapter() {
        override fun getCount() = items.size
        override fun getItem(position: Int): Any = items[position]
        override fun getItemId(position: Int) = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val row = (convertView as? LinearLayout) ?: LinearLayout(this@CapturesActivity).apply {
                orientation = LinearLayout.VERTICAL
                val margin = (12 * resources.displayMetrics.density).roundToInt()
                setPadding(margin, margin, margin, margin)
                addView(TextView(this@CapturesActivity).apply { id = android.R.id.text1; textSize = 15f })
                addView(TextView(this@CapturesActivity).apply { id = android.R.id.text2; textSize = 11f })
            }
            val item = items[position]
            row.findViewById<TextView>(android.R.id.text1).text = item.name
            row.findViewById<TextView>(android.R.id.text2).text =
                getString(R.string.captures_row, Captures.size(item.bytes), item.files)
            return row
        }
    }
}
