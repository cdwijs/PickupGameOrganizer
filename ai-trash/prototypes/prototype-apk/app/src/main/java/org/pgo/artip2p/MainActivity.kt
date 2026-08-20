package org.pgo.artip2p

import android.os.Bundle
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.lifecycle.Lifecycle
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

class MainActivity : AppCompatActivity() {

    private lateinit var onionField: EditText
    private lateinit var libp2pField: EditText
    private lateinit var statusView: TextView
    private lateinit var statusScroll: ScrollView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        onionField = findViewById(R.id.onionField)
        libp2pField = findViewById(R.id.libp2pField)
        statusView = findViewById(R.id.statusView)
        statusScroll = findViewById(R.id.statusScroll)

        Native.nativeInit(filesDir.absolutePath)

        findViewById<Button>(R.id.connectButton).setOnClickListener {
            appendStatus("• Connect pressed")
            Native.nativeConnect(
                onionField.text.toString(),
                libp2pField.text.toString(),
            )
        }
        findViewById<Button>(R.id.counterButton).setOnClickListener {
            Native.nativeStartCounter()
        }

        // Poll the native event queue while the activity is started.
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                while (true) {
                    drainEvents(Native.nativePoll())
                    delay(200)
                }
            }
        }
    }

    private fun drainEvents(batch: String) {
        if (batch.isEmpty()) return
        for (line in batch.split("\n")) {
            if (line.isEmpty()) continue
            val idx = line.indexOf('\u0001')
            val tag = if (idx >= 0) line.substring(0, idx) else "LOG"
            val body = if (idx >= 0) line.substring(idx + 1) else line
            when (tag) {
                "ONION" -> onionField.setText(body)
                "MADDR" -> libp2pField.setText(body)
                "RECV" -> appendStatus("⟵ received: $body")
                else -> appendStatus("• $body")
            }
        }
    }

    private fun appendStatus(msg: String) {
        statusView.append(msg + "\n")
        statusScroll.post { statusScroll.fullScroll(View.FOCUS_DOWN) }
    }
}
