package com.noiseinternet.app

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.media.AudioDeviceInfo
import android.net.VpnService
import android.os.Bundle
import android.text.InputType
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.RadioGroup
import android.widget.Spinner
import android.widget.TextView
import android.app.Activity
import com.noiseinternet.core.NoiseCore
import com.noiseinternet.core.NoiseService
import com.noiseinternet.core.PhyTools
import org.json.JSONObject

/**
 * The whole UI, and the only place in the Android app that renders text. Every field comes from
 * [NoiseService.lastStatusJson] (i.e. `Node::snapshot()` in C++); a field with no measurement prints
 * "-", and the banner at the top prints the platform's own verdict about whether sound is going
 * through the air at all.
 *
 * Deliberately absent: any number computed here. If a statistic is not in the JSON, it does not
 * appear on screen - which is the difference between a dashboard and a mock-up.
 *
 * Status: never compiled or run (no Android SDK in the authoring sandbox - see ../../../BUILD.md).
 */
class MainActivity : Activity() {   // plain Activity: the app pulls in no AndroidX, so the APK needs
    // nothing but the platform and the NDK-built core (see BUILD.md for why that choice matters)

    private lateinit var banner: TextView
    private lateinit var state: TextView
    private lateinit var profileSpinner: Spinner
    private lateinit var inputSpinner: Spinner
    private lateinit var outputSpinner: Spinner
    private lateinit var sessionEdit: EditText
    private lateinit var secretEdit: EditText
    private lateinit var duplexBox: CheckBox
    private lateinit var tunnelBox: CheckBox
    private lateinit var roleGroup: RadioGroup
    private lateinit var results: TextView
    private lateinit var profiles: MutableList<Pair<Int, String>>

    private val refresh = object : Runnable {
        override fun run() {
            renderStatus()
            window.decorView.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        banner = findViewById(R.id.banner)
        state = findViewById(R.id.state_line)
        profileSpinner = findViewById(R.id.profile)
        inputSpinner = findViewById(R.id.input_device)
        outputSpinner = findViewById(R.id.output_device)
        sessionEdit = findViewById(R.id.session_id)
        // Numbers only, but deliberately not a hard filter: a typo must show up as a parse error in
        // the UI rather than as a silently mangled session id.
        sessionEdit.inputType = InputType.TYPE_CLASS_NUMBER
        secretEdit = findViewById(R.id.secret)
        duplexBox = findViewById(R.id.duplex)
        tunnelBox = findViewById(R.id.tunnel)
        roleGroup = findViewById(R.id.role)
        results = findViewById(R.id.results)

        profiles = parseProfiles(PhyTools.profilesJson())
        profileSpinner.adapter = simple(profiles.map { "#${it.first} ${it.second}" })
        // FD-LOW-DBPSK: the profile every measured end-to-end run has paired and delivered on, and the
        // same one the Windows app starts on. Two apps whose defaults differ cannot pair by accident,
        // which is what happened in the field.
        profiles.indexOfFirst { it.second == "FD-LOW-DBPSK" }
            .let { if (it >= 0) profileSpinner.setSelection(it) }
        if (sessionEdit.text.isNullOrBlank()) sessionEdit.setText("7")   // same default as the PC

        inputSpinner.adapter = simple(deviceNames(true))
        outputSpinner.adapter = simple(deviceNames(false))

        findViewById<Button>(R.id.start).setOnClickListener { startLink() }
        findViewById<Button>(R.id.stop).setOnClickListener {
            stopService(Intent(this, NoiseService::class.java))
            results.text = "stopped. The last measured counters stay below."
            renderStatus()
        }
        findViewById<Button>(R.id.calibrate).setOnClickListener { runAsync("calibrating") { it.calibrate() } }
        findViewById<Button>(R.id.link_test).setOnClickListener {
            runAsync("link test 228 B") { it.linkTest(228) }
        }
        findViewById<Button>(R.id.net_test).setOnClickListener {
            runAsync("internet test") { it.internetTest("http://example.com/") }
        }
        findViewById<Button>(R.id.refresh).setOnClickListener { renderStatus() }

        askAudioPermission()
        window.decorView.post(refresh)
    }

    private fun simple(items: List<String>) =
        ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, items)

    /**
     * The device pickers, straight from AudioManager.getDevices() - the list is whatever this phone
     * reports, never a hard-coded one, so a machine with no microphone shows an empty picker instead of
     * a plausible-looking "default".
     */
    private fun deviceNames(inputs: Boolean): List<String> {
        val base = mutableListOf("system default")
        val mgr = applicationContext.getSystemService(Context.AUDIO_SERVICE) as android.media.AudioManager
        val kinds: List<AudioDeviceInfo> =
            if (android.os.Build.VERSION.SDK_INT >= 23) {
                mgr.getDevices(
                    if (inputs) android.media.AudioManager.GET_DEVICES_INPUTS
                    else android.media.AudioManager.GET_DEVICES_OUTPUTS
                ).toList()
            } else {
                emptyList()
            }
        for (d in kinds) {
            val label = (if (d.productName != null) d.productName.toString() else "device") +
                (if (d.isSink) " [output]" else " [input]") + (if (!d.isSource && !d.isSink) "" else "")
            base.add("$label (type ${d.type})")
        }
        if (kinds.isEmpty()) base.add("no devices reported by AudioManager")
        return base
    }

    private fun parseProfiles(json: String): MutableList<Pair<Int, String>> {
        val out = mutableListOf<Pair<Int, String>>()
        try {
            val root = JSONObject(json)
            val arr = if (root.has("profiles")) root.getJSONArray("profiles") else root.getJSONArray("items")
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                out.add(Pair(o.getInt("id"), o.optString("name", "?")))
            }
        } catch (e: Exception) {
            // No profiles means no session can be configured, so say exactly that instead of listing
            // a plausible-looking default.
            out.clear()
            out.add(Pair(-1, "profile table unavailable: ${e.message}"))
        }
        return out
    }

    private fun askAudioPermission() {
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), 7)
        }
    }

    private fun startLink() {
        val sel = profileSpinner.selectedItemPosition
        val pid = if (sel in profiles.indices) profiles[sel].first else -1
        // The single most common reason this app never paired: an empty passphrase. The engine cannot
        // pair without one (it will not invent a key), so this is checked here, in words, before any
        // service starts - instead of leaving the user watching a status line that says nothing.
        val secret = secretEdit.text.toString()
        if (secret.length < 8) {
           results.text = "The passphrase is ${secret.length} characters long. Both ends need the SAME " +
                "passphrase, 8 characters or more: without one the engine never even sends a pairing " +
                "challenge, so nothing can pair. Type a passphrase here, type the identical one on the " +
                "other device, then press Start again. Nothing has been started or transmitted."
            secretEdit.requestFocus()
            return
        }
        if (sessionEdit.text.toString().toIntOrNull() == null) {
            results.text = "The session id must be a number (7 is the default). Both ends need the same one."
            sessionEdit.requestFocus()
            return
        }
        val i = Intent(this, NoiseService::class.java).apply {
            putExtra(
                NoiseService.EXTRA_ROLE,
                if (roleGroup.checkedRadioButtonId == R.id.role_giver) "giver" else "receiver"
            )
            putExtra(NoiseService.EXTRA_PROFILE, pid)
            putExtra(NoiseService.EXTRA_SECRET, secret)
            putExtra(NoiseService.EXTRA_SESSION, sessionEdit.text.toString().toIntOrNull() ?: 0)
            putExtra(NoiseService.EXTRA_DUPLEX, duplexBox.isChecked)
            putExtra(NoiseService.EXTRA_TUNNEL, tunnelBox.isChecked)
            putExtra(NoiseService.EXTRA_INPUT, inputSpinner.selectedItemPosition - 1)
            putExtra(NoiseService.EXTRA_OUTPUT, outputSpinner.selectedItemPosition - 1)
        }
        if (tunnelBox.isChecked) {
            val prep = VpnService.prepare(this)
            if (prep != null) {
                startActivityForResult(prep, 11)
                results.text = "approve the VPN request, then press Start again. Nothing was measured."
                return
            }
        }
        startForegroundService(i)
    }

    /** Long actions run off the UI thread; the service owns the node, so we only forward requests. */
    private fun runAsync(label: String, body: (NoiseService) -> String) {
        results.text = "$label ..."
        val svc = NoiseService.get()
        if (svc == null) {
            results.text = "$label refused: the link service is not running. Press Start first - " +
                "there is no measurement to show without it."
            return
        }
        Thread {
            val text = try {
                body(svc)
            } catch (e: Exception) {
                "refused: ${e.message}"
            }
            runOnUiThread { results.text = prettify(text) }
        }.start()
    }

    private fun prettify(s: String): String = try {
        JSONObject(s).toString(2)
    } catch (e: Exception) {
        s
    }

    private fun renderStatus() {
        val json = NoiseService.lastStatusJson
        val o = try {
            JSONObject(json)
        } catch (e: Exception) {
            null
        }
        if (o == null) {
            state.text = "no node yet - press Start"
            banner.text = "NOT TESTED: nothing has been measured by this app"
            return
        }
        val st = NoiseCore.Status.parse(json)
        val svc = NoiseService.get()
        state.text = buildString {
            append("state ").append(st.state).append("  role ").append(st.role)
            append("  profile ").append(st.profile)
            st.sampleRate?.let { append("  fs ").append(it.toInt()) }
            append("\npeer ").append(if (st.peerOnline) "online" else "offline")
            append("  pairing ").append(st.pairing)
            append("\nmatch code: ").append(st.sas.ifBlank { "(not paired yet)" })
            append("   <- must read the same on both devices")
            append("\ninput ").append(st.f(st.inDbfs)).append(" dBFS")
            append("  bursts seen ").append(st.n(st.syncHits))
            append("  decoded ").append(st.n(st.framesReceived))
            append("\nwhy: ").append(st.diagnosis())
            append("  mtu ").append(st.n(st.mtu))
            append("\nSNR ").append(st.f(st.snrDb, "%.2f")).append(" dB")
            append("  RSSI ").append(st.f(st.rssiDbfs)).append(" dBFS")
            append("  noise ").append(st.f(st.noiseDbfs)).append(" dBFS")
            append("  EVM ").append(st.f(st.evecPct, "%.2f")).append(" %")
            append("\nRTT ").append(st.f(st.rttMs)).append(" ms")
            append("  loss ").append(st.f(st.lossPct, "%.2f")).append(" %")
            append("  useful up ").append(st.n(st.usefulBpsUp)).append(" b/s")
            append("  down ").append(st.n(st.usefulBpsDown)).append(" b/s")
            append("\nframes sent ").append(st.n(st.framesSent))
            append("  recv ").append(st.n(st.framesReceived))
            append("  retries ").append(st.n(st.retries))
            append("  crc-fail ").append(st.n(st.crcFail))
            append("\nbytes sent ").append(st.n(st.bytesSent))
            append("  received ").append(st.n(st.bytesReceived))
            append("\ntunnel ").append(if (st.tunnelUp) "up" else "down")
            append("  internet ").append(if (st.internetOk) "verified" else "not verified")
            if (svc != null) {
                append("\naudio: ").append(svc.audioDescription())
                append("\ntunnel note: ").append(svc.tunnelNote)
            }
        }
        val warn = st.audioNote.ifBlank { null } ?: svc?.routeWarning()
        banner.text = when {
            warn != null -> "NOT TESTED / WARNING: $warn"
            st.internetOk -> "internet path verified by the engine's own check"
            else -> "link up; no internet verification has succeeded"
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == 11) {
            results.text = if (resultCode == RESULT_OK) "VPN approved. Press Start to begin."
            else "VPN declined - the tunnel will stay down; the proxy path is unaffected."
        }
    }

}
