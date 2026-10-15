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
import org.json.JSONArray
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
    private lateinit var tokenEdit: EditText
    private lateinit var duplexBox: CheckBox
    private lateinit var tunnelBox: CheckBox
    private lateinit var roleGroup: RadioGroup
    private lateinit var results: TextView
    private lateinit var profiles: MutableList<Pair<Int, String>>

    /**
     * Which build this APK is, written into the app's resources by the build (see
     * android/app/build.gradle.kts). It is printed in the report, so "did my phone actually get the new
     * build?" is answered by the app itself rather than by comparing download timestamps.
     */
    private val appStamp: String by lazy {
        try {
            getString(R.string.ni_build_stamp)
        } catch (e: Exception) {
            "unknown (this APK has no stamp resource)"
        }
    }

    // Exactly what renderStatus() built for the state pane this time round - the report, without the
    // copy acknowledgement that is appended to the pane itself (see copyReport/renderStatus).
    private var reportBase: String = ""
    // The most recent "copied N characters" line and when it was made. It is re-drawn on every 500 ms
    // refresh for a few seconds, because the pane it is written into is rewritten by that same loop and
    // the acknowledgement the old handler wrote was gone before anyone could read it (defect 71).
    private var copiedNote: String = ""
    private var copiedAt: Long = 0L

    private companion object {
        const val ACK_PREFIX = "COPIED:"
        const val COPY_ACK_MS = 15000L
    }

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
        tokenEdit = findViewById(R.id.proxy_token)
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
        // The one test that needs no second device: does THIS phone's speaker reach THIS phone's
        // microphone, and is what it hears readable? The same analyzer the PC runs, so a recording that
        // decodes here and not there is a fact about the room, not about the app.
        findViewById<Button>(R.id.record_test).setOnClickListener {
            val svc = NoiseService.get()
            if (svc == null) {
                results.text = "nothing is running: press Start first, because this records through the " +
                    "same engine the link uses."
                return@setOnClickListener
            }
            val pid = if (profileSpinner.selectedItemPosition in profiles.indices)
                profiles[profileSpinner.selectedItemPosition].first else -1
            results.text = "recording 8 seconds of what the microphone delivers, then analysing it..."
            svc.recordAndAnalyse(8, 48000.0, if (pid > 0) pid else 10) { text ->
                results.text = "WHAT THIS DEVICE HEARS\n\n" + text
            }
        }
        findViewById<Button>(R.id.link_test).setOnClickListener {
            runAsync("link test 228 B") { it.linkTest(228) }
        }
        findViewById<Button>(R.id.net_test).setOnClickListener {
            runAsync("internet test") { it.internetTest("http://example.com/") }
        }
        findViewById<Button>(R.id.refresh).setOnClickListener { renderStatus() }
        // Copy the report. The panes here are the only place the measurements exist, and a user with a
        // phone cannot select-and-copy a TextView reliably; the button copies exactly what is shown.
        findViewById<Button>(R.id.copy_report).setOnClickListener { copyReport() }

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
        // ni_abi_profiles_json() returns a JSON ARRAY of profiles. This used to parse it as an object
        // (JSONObject(json)), which throws on an array, so the spinner fell back to a single "#-1
        // profile table unavailable" entry and the app then sent an invalid profile id to the engine -
        // which quietly substituted its own default (DBPSK-MID) while the PC ran FD-LOW-DBPSK. Two ends
        // on different profiles cannot decode one burst from each other, so this one parse failure was
        // enough on its own to stop pairing ("it never paired ... most probably the android one").
        // Accept either shape, so a future core change cannot reintroduce it.
        try {
            val t = json.trim()
            val arr = when {
                t.startsWith("[") -> JSONArray(t)
                else -> {
                    val root = JSONObject(t)
                    when {
                        root.has("profiles") -> root.getJSONArray("profiles")
                        root.has("items") -> root.getJSONArray("items")
                        else -> JSONArray()
                    }
                }
            }
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                val id = o.optInt("id", -1)
                if (id < 0) continue
                out.add(Pair(id, o.optString("name", "profile $id")))
            }
        } catch (e: Exception) {
            out.clear()
        }
        if (out.isEmpty()) {
            // Nothing to select means nothing can be configured. Say that, and never let a start go
            // out with an id the engine will have to guess at.
            out.add(Pair(-1, "profile table unavailable: ${json.take(60)}"))
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
        // A profile the core does not know is not a profile: the engine would substitute its own
        // default and the two ends would talk past each other without a word on screen. Refuse, and
        // name the reason (the profile list could not be read - see parseProfiles).
        if (pid < 0) {
            results.text = "Cannot start: this build could not read the core's modulation profile table, " +
                "and starting with a guessed profile is how two devices end up unable to hear each other.\n" +
                "Profile list: ${profiles.joinToString { "#${it.first} ${it.second}" }}"
            return
        }
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
            putExtra(NoiseService.EXTRA_SESSION, sessionEdit.text.toString().toIntOrNull() ?: 7)
            putExtra(NoiseService.EXTRA_DUPLEX, duplexBox.isChecked)
            putExtra(NoiseService.EXTRA_TUNNEL, tunnelBox.isChecked)
            // Defect 27 on the phone side: the receiver's proxy refuses any client without its bearer
            // token, and until this field existed the phone's token could only be read out of a status
            // line. Empty means the engine generates one.
            putExtra(NoiseService.EXTRA_PROXY_TOKEN, tokenEdit.text.toString().trim())
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
            // The pane and the copied text are the same string here. They used to diverge: reportBase stayed
            // empty on this path, so the copy button on a phone that had never been started copied the
            // banner and nothing else (found while fixing defect 71 - a copy button that copies part of
            // what is on screen is the same defect one step smaller).
            val line = "no node yet - press Start\nbuild: app " + appStamp +
                "   (the engine's build appears here once a node is running)"
            reportBase = line
            val note = freshCopyNote()
            state.text = if (note.isEmpty()) line else line + "\n\n" + note
            banner.text = "NOT TESTED: nothing has been measured by this app"
            return
        }
        val st = NoiseCore.Status.parse(json, appStamp)
        val svc = NoiseService.get()
        reportBase = buildString {
            // Build first: the single most useful line when a fix "did not work" is knowing which build is
            // running. Engine and app separately, because two different toolchains produce them.
            append("build: engine ").append(st.build.ifBlank { "(none reported)" })
            append("   app ").append(st.appBuild.ifBlank { "(unstamped)" })
            append("\nstate ").append(st.state).append("  role ").append(st.role)
            append("  profile ").append(st.profile)
            st.sessionId?.let { append("  session ").append(it) }
            st.sampleRate?.let { append("  fs ").append(it.toInt()) }
            append("   <- profile, session and fs must be identical on both ends")
            // The engine's own sentence when it has one (it knows things the panel cannot see: own
            // transmissions heard back, a passphrase proof that failed). The Kotlin diagnosis is the
            // fallback for the documents the service writes itself - NO PASSPHRASE and friends - which
            // never went through the engine and therefore have no advice line.
            val explain = st.advice.ifBlank { st.diagnosis() }
            if (explain.isNotBlank()) {
                append("\n\nWHAT TO DO: ").append(explain).append("\n")
            }
            append("\nPAIRED: ").append(if (st.pairing == "paired") "YES" else "NOT PAIRED")
            append("\npeer ").append(if (st.peerOnline) "online" else "offline")
            append("  pairing ").append(st.pairing)
            append("\nmatch code: ").append(st.sas.ifBlank { "(not paired yet)" })
            append("   <- must read the same on both devices")
            append("\ninput ").append(st.f(st.inDbfs)).append(" dBFS")
            append("  bursts seen ").append(st.n(st.syncHits))
            append("  decoded ").append(st.n(st.framesReceived))
            // Its own voice, counted as such. Without this line "decoded 48, bytes received 0" reads
            // like a peer that answers and delivers nothing; with it, it reads like a device listening to
            // itself, which is what it is (defect 59).
            append("  self-echo ").append(st.n(st.selfEchoBursts))
            val reps = st.selfEchoRepeats ?: 0L
            if (reps > 0) append(" (+").append(reps).append(" repeat decodes of the same burst)")
            st.txGain?.let { append("\nout level ").append(String.format("%.2f", it)).append(" of full scale") }
            append("  mtu ").append(st.n(st.mtu))
            // "not measured yet" instead of the engine's default values (-120 dBFS, 0.00 %, 100.00 %
            // loss). Printing those as readings made a link on which no burst had been demodulated look
            // like a link with a dead microphone and total packet loss (defect F); the flags travel with
            // the status JSON for exactly this.
            if (st.phyMeasured) {
                append("\nSNR ").append(st.f(st.snrDb, "%.2f")).append(" dB")
                append("  RSSI ").append(st.f(st.rssiDbfs)).append(" dBFS")
                append("  noise ").append(st.f(st.noiseDbfs)).append(" dBFS")
                append("  EVM ").append(st.f(st.evecPct, "%.2f")).append(" %")
            } else {
                append("\nSNR / RSSI / noise / EVM: not measured yet - no burst has been demodulated on " +
                    "this device, so these would be the engine's default values and not readings")
            }
            if (st.arqMeasured) {
                append("\nRTT ").append(st.f(st.rttMs)).append(" ms")
                append("  loss ").append(st.f(st.lossPct, "%.2f")).append(" %")
            } else {
                append("\nRTT / loss: not measured yet - nothing has been transmitted yet, so there is " +
                    "nothing whose round trip or loss could be reported")
            }
            append("  useful up ").append(st.n(st.usefulBpsUp)).append(" b/s")
            append("  down ").append(st.n(st.usefulBpsDown)).append(" b/s")
            append("\nframes sent ").append(st.n(st.framesSent))
            append("  recv ").append(st.n(st.framesReceived))
            append("  retries ").append(st.n(st.retries))
            append("  crc-fail ").append(st.n(st.crcFail))
            append("\nbytes sent ").append(st.n(st.bytesSent))
            append("  received ").append(st.n(st.bytesReceived))
            // Two different things used to share the word "tunnel" and contradict each other on screen:
            // the engine's own proxy/relay state (from the status JSON) and this phone's Android
            // VpnService state (from the service). They are named for what they are now, and the engine's
            // tunnel_up is true only when the link is paired and a client is really using the proxy
            // (defect E).
            append("\nacoustic relay (engine): ").append(if (st.tunnelUp) "up" else "down")
            append(if (st.tunnelDetail.isNotEmpty()) "  " + st.tunnelDetail else "")
            append("\ninternet: ").append(if (st.internetOk) "verified by the engine's own check" else "not verified")
            if (svc != null) {
                append("\naudio: ").append(svc.audioDescription())
                append("\nphone VPN (VpnService): ").append(svc.tunnelNote)
            }
        }
        // The pane carries the report plus the copy acknowledgement, if one is recent. The acknowledgement
        // is re-added here on every refresh instead of being written once into a pane this 500 ms loop
        // rewrites (defect 71).
        val note = freshCopyNote()
        state.text = if (note.isEmpty()) reportBase else reportBase + "\n\n" + note
        val warn = st.audioNote.ifBlank { null } ?: svc?.routeWarning()
        banner.text = when {
            warn != null -> "NOT TESTED / WARNING: $warn"
            st.internetOk -> "internet path verified by the engine's own check"
            else -> "link up; no internet verification has succeeded"
        }
    }

    /** The copy acknowledgement while it is fresh, or "" - see copyReport and renderStatus. */
    private fun freshCopyNote(): String =
        if (copiedNote.isNotEmpty() && System.currentTimeMillis() - copiedAt < COPY_ACK_MS) copiedNote else ""

    /**
     * Put the report that is on screen on the clipboard.
     *
     * This used to copy [results] - the lower pane, which holds the answer to whatever test was last
     * run and is empty at rest - while the report a reader is actually looking at is built into [state]
     * and [banner]. Pressing the button therefore copied nothing and answered "nothing to copy yet",
     * and the acknowledgement it wrote into [results] was overwritten by the 500 ms refresh loop before
     * it could be read (defect 71, reported from the field on the APK).
     *
     * What it does now: re-renders first, so the panes hold this instant's snapshot rather than one up
     * to half a second old; copies every non-empty pane in reading order; and reports the result in the
     * state line - where renderStatus() re-adds it on every refresh for [COPY_ACK_MS], so the loop
     * cannot erase it - as well as in a toast (Android 13+ draws its own clipboard chip, which says
     * something was copied but not what or how much).
     */
    private fun copyReport() {
        renderStatus()
        val parts = ArrayList<String>(3)
        for (t in listOf(banner.text, reportBase, results.text)) {
            val s = t?.toString().orEmpty().trim()
            if (s.isNotEmpty() && !s.startsWith(ACK_PREFIX)) parts.add(s)
        }
        val body = parts.joinToString("\n\n")
        if (body.isBlank()) {
            results.text = "there is nothing on screen to copy yet: press Start and then this button " +
                "again. The button copies what this screen is showing, so an empty screen copies nothing."
            return
        }
        try {
            val cm = getSystemService(Context.CLIPBOARD_SERVICE) as android.content.ClipboardManager
            cm.setPrimaryClip(android.content.ClipData.newPlainText("Noise Internet report", body))
        } catch (e: Exception) {
            results.text = "this phone's clipboard refused the copy (${e.javaClass.simpleName}: " +
                "${e.message}). The report is in the pane above and can be selected by hand."
            return
        }
        val stamp = java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.US)
            .format(java.util.Date())
        copiedNote = ACK_PREFIX + " copied " + body.length + " characters to this phone's clipboard at " +
            stamp + " and left them there. Paste into a message or a note to send the report on."
        copiedAt = System.currentTimeMillis()
        renderStatus()
        android.widget.Toast.makeText(this, "Report copied ($stamp)", android.widget.Toast.LENGTH_SHORT).show()
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == 11) {
            results.text = if (resultCode == RESULT_OK) "VPN approved. Press Start to begin."
            else "VPN declined - the tunnel will stay down; the proxy path is unaffected."
        }
    }

}
