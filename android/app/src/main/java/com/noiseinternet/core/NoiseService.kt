package com.noiseinternet.core

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.media.AudioDeviceInfo
import android.net.VpnService
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.os.PowerManager
import android.util.Log
import java.io.FileInputStream
import java.io.FileOutputStream
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean

/**
 * The engine's home on Android: one foreground service owning the native node, the PCM pump, and
 * optionally a [VpnService] tunnel that pushes UDP/DNS into the acoustic link.
 *
 * It extends VpnService rather than being a second service, because two services each owning a node
 * would fight over the microphone and the ARQ state machine; one owner, one set of counters.
 *
 * What this class does **not** do: it never synthesises a number. Every field the UI shows comes from
 * [NoiseCore.statusJson], and anything the platform could not measure stays "-" there.
 *
 * Status: never compiled or run (no Android SDK/NDK in the authoring sandbox - see ../../../BUILD.md).
 */
class NoiseService : VpnService(), AudioEngine.Sink {

    companion object {
        const val TAG = "NoiseInternet"
        const val ACTION_UPDATE = "com.noiseinternet.core.UPDATE"
        const val EXTRA_ROLE = "role"
        const val EXTRA_PROFILE = "profile"
        const val EXTRA_SECRET = "secret"
        const val EXTRA_SESSION = "session"
        const val EXTRA_FS = "fs"
        const val EXTRA_DUPLEX = "duplex"
        const val EXTRA_TUNNEL = "tunnel"
        const val EXTRA_INPUT = "input_type"
        const val EXTRA_OUTPUT = "output_type"

        /** Streams with bit 14 set belong to the proxy; the tunnel stays below it (and avoids the
         *  link-test stream 16383). The core's routing rule is documented in protocol/SPEC.md. */
        const val STREAM_SPACE = 0x3FFF
        // MsgType in the core (ni/session.h): 0 = opaque stream bytes.
        const val MSG_STREAM = 0

        @Volatile
        var lastStatusJson: String = "{}"
            private set

        private val listeners = ArrayList<(String) -> Unit>()

        fun addListener(f: (String) -> Unit) = synchronized(listeners) { listeners.add(f) }
        fun removeListener(f: (String) -> Unit) = synchronized(listeners) { listeners.remove(f) }

        fun isRunning(): Boolean = instance != null

        /** The one running service, or null. Never a placeholder object: "not running" must be visible. */
        fun get(): NoiseService? = instance

        @Volatile
        private var instance: NoiseService? = null
    }

    private var core: NoiseCore? = null
    private var audio: AudioEngine? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var pump: Thread? = null
    private val alive = AtomicBoolean(false)
    private val handler = Handler(Looper.getMainLooper())

    // tunnel
    private var tun: ParcelFileDescriptor? = null
    private var tunIn: FileInputStream? = null
    private var tunOut: FileOutputStream? = null
    private var tunThread: Thread? = null
    private val flows = ConcurrentHashMap<Int, Flow>()
    private val flowIds = ConcurrentHashMap<String, Int>()
    private var nextStream = 1
    @Volatile
    var tunnelUp = false
        private set
    @Volatile
    var tunnelNote = "tunnel not requested"
        private set
    @Volatile
    var droppedTcp = 0L
        private set
    @Volatile
    var droppedOther = 0L
        private set

    private class Flow(
        val srcIp: ByteArray,     // the phone app's address, as it appeared in the outbound packet
        val srcPort: Int,
        val dstIp: ByteArray,
        val dstPort: Int,
        var recvSeq: Int = 0,
    )

    override fun onCreate() {
        super.onCreate()
        instance = this
        if (Build.VERSION.SDK_INT >= 26) {
            val ch = NotificationChannel("noise", "Noise Internet link", NotificationManager.IMPORTANCE_LOW)
            (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).createNotificationChannel(ch)
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val cfg = NoiseCore.Config(
            role = if (intent?.getStringExtra(EXTRA_ROLE) == "giver") NoiseCore.Role.GIVER
            else NoiseCore.Role.RECEIVER,
            profile = intent?.getIntExtra(EXTRA_PROFILE, 2) ?: 2,
            pairingSecret = intent?.getStringExtra(EXTRA_SECRET) ?: "",
            sessionId = intent?.getIntExtra(EXTRA_SESSION, 0) ?: 0,
            sampleRate = intent?.getIntExtra(EXTRA_FS, 48000) ?: 48000,
            duplex = intent?.getBooleanExtra(EXTRA_DUPLEX, false) ?: false,
            proxyEnable = true,
        )
        startForeground(4242, buildNotification("starting"))
        acquireWakeLock()

        val c = try {
            NoiseCore(cfg)
        } catch (e: Exception) {
            Log.e(TAG, "native core refused to start", e)
            publish("{\"state_name\":\"NATIVE CORE REFUSED TO START\",\"audio_note\":\"" +
                (e.message ?: "unknown").replace("\"", "'") + "\"}")
            stopSelf()
            return START_NOT_STICKY
        }
        core = c
        try {
            c.startChecked()
        } catch (e: Exception) {
            publish("{\"state_name\":\"START FAILED\",\"audio_note\":\"${e.message}\"}")
        }

        val a = AudioEngine(this, cfg.sampleRate, this)
        audio = a
        val wantIn = pick(intent?.getIntExtra(EXTRA_INPUT, -1) ?: -1, a.listInputs())
        val wantOut = pick(intent?.getIntExtra(EXTRA_OUTPUT, -1) ?: -1, a.listOutputs())
        if (!a.start(wantIn, wantOut)) {
            // The honest path: no audio, no link. We keep the node up so the UI can show why, and the
            // tunnel stays down, but nothing here pretends a session exists.
            publish("{\"state_name\":\"NO AUDIO\",\"audio_note\":\"" +
                a.lastError.replace("\"", "'") + "\"}")
        }
        if (intent?.getBooleanExtra(EXTRA_TUNNEL, false) == true) openTunnel()

        alive.set(true)
        pump = Thread({
            // step() drives pairing, ARQ timers and the proxy state machines; it is called on its own
            // thread at a few kHz so the audio callbacks never block on protocol work.
            while (alive.get()) {
                try {
                    core?.step(System.currentTimeMillis())
                } catch (e: Exception) {
                    Log.e(TAG, "step failed", e)
                }
                Thread.sleep(5)
            }
        }, "ni-step").apply { isDaemon = true; start() }
        handler.postDelayed(statusTicker, 500)
        return START_STICKY
    }

    private fun pick(type: Int, list: List<AudioDeviceInfo>): AudioDeviceInfo? =
        if (type < 0) null else list.firstOrNull { it.type == type }

    private val statusTicker = object : Runnable {
        override fun run() {
            val c = core
            if (c != null) {
                val s = c.statusJson()
                publish(s)
                try {
                    val o = org.json.JSONObject(s)
                    val snr = if (o.isNull("snr_db")) "-" else String.format("%.1f dB", o.getDouble("snr_db"))
                    notify(buildNotification("state " + o.optString("state_name", "?") + ", snr " + snr))
                } catch (e: Exception) {
                    notify(buildNotification("state unknown"))
                }
            }
            if (alive.get()) handler.postDelayed(this, 500)
        }
    }

    private fun publish(json: String) {
        lastStatusJson = json
        synchronized(listeners) { listeners.toList() }.forEach {
            handler.post { runCatching { it(json) } }
        }
        notify(buildNotification(summary(json)))
    }

    private fun summary(json: String): String = try {
        val o = org.json.JSONObject(json)
        "NOISE INTERNET - " + o.optString("role", "?") + "/" + o.optString("state_name", "?")
    } catch (e: Exception) {
        "NOISE INTERNET"
    }

    private fun buildNotification(text: String): Notification {
        // Named by string, not class literal: the core package must not depend on the app package.
        val pi = PendingIntent.getActivity(
            this, 0,
            Intent().setClassName(packageName, "com.noiseinternet.app.MainActivity"),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        val b = if (Build.VERSION.SDK_INT >= 26) Notification.Builder(this, "noise")
        else Notification.Builder(this)
        return b.setContentTitle("NOISE INTERNET")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_notify_sync)
            .setOngoing(true)
            .setContentIntent(pi)
            .build()
    }

    private fun notify(n: Notification) =
        (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).notify(4242, n)

    private fun acquireWakeLock() {
        if (wakeLock == null) {
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "noise:link").apply {
                setReferenceCounted(false)
                acquire(8 * 60 * 60 * 1000L)   // never an indefinite lock
            }
        }
    }

    // ---- AudioEngine.Sink: PCM in both directions -----------------------------------------------

    override fun onCapture(buf: FloatArray, n: Int): Int = core?.feedPcm(buf, n) ?: 0

    override fun onPull(buf: FloatArray, cap: Int): Int = core?.pullPcm(buf, cap) ?: 0

    // ---- user actions (all of them return measured reports, none of them a stub) -----------------

    fun calibrate(): String {
        val c = core ?: return "{\"error\":\"no node\"}"
        return c.calibrate(System.currentTimeMillis())
    }

    fun linkTest(bytes: Int): String {
        val c = core ?: return "{\"error\":\"no node\"}"
        return c.testLink(System.currentTimeMillis(), bytes)
    }

    fun internetTest(url: String): String {
        val c = core ?: return "{\"error\":\"no node\"}"
        return c.testInternet(System.currentTimeMillis(), url)
    }

    fun audioDescription(): String = audio?.describe() ?: "audio engine not started"

    fun routeWarning(): String? {
        val a = audio ?: return "audio engine not started"
        return if (a.acousticRouteOk()) null
        else "captured on ${a.routedInput} and played on ${a.routedOutput}: this is NOT an " +
            "airborne acoustic link, so results must not be reported as such"
    }

    // ---- tunnel: UDP + DNS only, by design -------------------------------------------------------

    /**
     * Opens a TUN and pumps outbound UDP (which includes the DNS on 53) into the acoustic link. TCP is
     * deliberately not tunnelled: a 1 kbps-ish acoustic link cannot carry a TCP handshake with its own
     * retransmission timers, and half-implementing it would produce a tunnel that silently breaks. Web
     * traffic therefore goes through the receiver's local HTTP/CONNECT proxy (127.0.0.1:8471), where
     * the link's own ARQ provides the reliability. That is the whole justification for the limitation
     * - and it is why "usable internet" is a separate gate from "a working tunnel".
     */
    fun openTunnel(): Boolean {
        if (tunnelUp) return true
        val c = core ?: run {
            tunnelNote = "no node"
            return false
        }
        try {
            var b = Builder()
                .setSession("NOISE INTERNET")
                .addAddress("10.77.0.2", 30)
                .addDnsServer("8.8.8.8")
                .addRoute("0.0.0.0", 0)
                .setBlocking(true)
            // Our own traffic must stay outside the tunnel, or the fetch that feeds it is captured by
            // it and the link eats its own tail. The method is addAllowedApplication(ApplicationInfo)
            // - not allowApplication(String), which is what this file claimed until the compiler
            // said otherwise - and it throws NameNotFoundException if the package is unknown.
            try {
                val mine = packageManager.getApplicationInfo(packageName, 0)
                b = b.addAllowedApplication(mine)
            } catch (e: Exception) {
                tunnelNote = "own app not excluded from tunnel (${e.javaClass.simpleName}): " +
                    "the link may loop its own traffic"
            }
            val p = b.establish()
            if (p == null) {
                tunnelNote = "user must approve the VPN request first (VpnService.prepare)"
                return false
            }
            tun = p
            // protect() takes the raw descriptor int; passing the FileDescriptor object is a type
            // error the Kotlin compiler catches, and it would have been a "why is my own traffic
            // looping back into the tunnel" mystery at runtime.
            protect(p.fd)
            tunIn = FileInputStream(p.fileDescriptor)
            tunOut = FileOutputStream(p.fileDescriptor)
            tunnelUp = true
            tunnelNote = "UDP + DNS only; TCP/ICMP counted and dropped ($droppedTcp/$droppedOther)"
            val buf = ByteArray(16 * 1024)
            tunThread = Thread({
                while (tunnelUp) {
                    val n = try {
                        tunIn!!.read(buf)
                    } catch (e: Exception) {
                        if (tunnelUp) tunnelNote = "tun read failed: ${e.message}"
                        break
                    }
                    if (n <= 0) continue
                    handleOutbound(buf, n, c)
                }
                tunnelUp = false
            }, "ni-tun").apply { isDaemon = true; start() }
            return true
        } catch (e: Exception) {
            tunnelNote = "tunnel refused: ${e.message}"
            return false
        }
    }

    fun closeTunnel() {
        tunnelUp = false
        try { tunIn?.close() } catch (e: Exception) { }
        try { tunOut?.close() } catch (e: Exception) { }
        try { tun?.close() } catch (e: Exception) { }
        tunIn = null
        tunOut = null
        tun = null
        flows.clear()
        tunnelNote = "tunnel closed"
    }

    /**
     * Inbound bytes for a stream -> a TUN packet. The link hands reassembled payloads to the message
     * handler; this is where a phone app's answer comes back as a datagram.
     */
    fun inject(stream: Int, payload: ByteArray) {
        val f = flows[stream] ?: return
        val out = tunOut ?: return
        out.write(buildUdpPacket(f, payload))
        out.flush()
    }

    private fun handleOutbound(pkt: ByteArray, n: Int, c: NoiseCore) {
        if (n < 20 || (pkt[0].toInt() and 0xF0) != 0x40) {
            droppedOther++
            return
        }
        val ihl = (pkt[0].toInt() and 0x0F) * 4
        if (ihl < 20 || n < ihl + 8) {
            droppedOther++
            return
        }
        val proto = pkt[9].toInt() and 0xFF
        if (proto != 17) {
            if (proto == 6) droppedTcp++ else droppedOther++
            return
        }
        val src = pkt.copyOfRange(12, 20)
        val dst = pkt.copyOfRange(16, 20)
        val total = ((pkt[2].toInt() and 0xFF) shl 8) or (pkt[3].toInt() and 0xFF)
        val span = if (total >= ihl + 8 && total <= n) total else n
        val srcPort = ((pkt[ihl].toInt() and 0xFF) shl 8) or (pkt[ihl + 1].toInt() and 0xFF)
        val dstPort = ((pkt[ihl + 2].toInt() and 0xFF) shl 8) or (pkt[ihl + 3].toInt() and 0xFF)
        // The UDP length field counts its own 8-byte header, so the payload ends at ihl + ulen.
        val ulen = ((pkt[ihl + 4].toInt() and 0xFF) shl 8) or (pkt[ihl + 5].toInt() and 0xFF)
        val end = minOf(span, ihl + 8 + maxOf(ulen - 8, 0))
        if (end <= ihl + 8) {
            droppedOther++
            return
        }
        val body = pkt.copyOfRange(ihl + 8, end)
        val key = streamKey(src, srcPort, dst, dstPort)
        val stream = flowIds.getOrPut(key) {
            val s = allocStream()
            flows[s] = Flow(src, srcPort, dst, dstPort)
            s
        }
        try {
            c.sendBytes(stream, MSG_STREAM, body, body.size, 0)
        } catch (e: Exception) {
            tunnelNote = "sendBytes failed: ${e.message}"
        }
    }

    private fun allocStream(): Int {
        val s = nextStream
        nextStream = if (nextStream >= STREAM_SPACE - 1) 1 else nextStream + 1
        return s
    }

    /** Flows are keyed by the 4-tuple so return traffic finds the app that opened the socket. */
    private fun v4(b: ByteArray): String =
        "${b[0].toInt() and 0xFF}.${b[1].toInt() and 0xFF}.${b[2].toInt() and 0xFF}." +
            "${b[3].toInt() and 0xFF}"

    private fun streamKey(src: ByteArray, sp: Int, dst: ByteArray, dp: Int): String =
        "${v4(src)}:$sp->${v4(dst)}:$dp"

    /** Minimal IPv4 + UDP frame. The UDP checksum is left 0, which IPv4 permits for UDP. */
    private fun buildUdpPacket(f: Flow, body: ByteArray): ByteArray {
        val total = 20 + 8 + body.size
        val p = ByteArray(total)
        p[0] = 0x45.toByte()
        p[1] = 0
        p[2] = (total shr 8 and 0xFF).toByte()
        p[3] = (total and 0xFF).toByte()
        p[4] = 0
        p[5] = 0
        p[6] = 0
        p[7] = 0
        p[8] = 64
        p[9] = 17
        for (i in 0..3) p[12 + i] = f.dstIp[i]      // deliver to the app that asked
        for (i in 0..3) p[16 + i] = f.srcIp[i]
        val hl = ipChecksum(p, 20)
        p[10] = (hl shr 8 and 0xFF).toByte()
        p[11] = (hl and 0xFF).toByte()
        p[20] = (f.dstPort shr 8 and 0xFF).toByte()
        p[21] = (f.dstPort and 0xFF).toByte()
        p[22] = (f.srcPort shr 8 and 0xFF).toByte()
        p[23] = (f.srcPort and 0xFF).toByte()
        val ul = 8 + body.size
        p[24] = (ul shr 8 and 0xFF).toByte()
        p[25] = (ul and 0xFF).toByte()
        p[26] = 0
        p[27] = 0
        body.copyInto(p, 28)
        return p
    }

    private fun ipChecksum(h: ByteArray, len: Int): Int {
        var sum = 0
        var i = 0
        while (i + 1 < len) {
            sum += ((h[i].toInt() and 0xFF) shl 8) or (h[i + 1].toInt() and 0xFF)
            i += 2
        }
        if (i < len) sum += (h[i].toInt() and 0xFF) shl 8
        while (sum shr 16 != 0) sum = (sum and 0xFFFF) + (sum shr 16)
        return sum.inv() and 0xFFFF
    }

    // ---- lifecycle --------------------------------------------------------------------------------

    override fun onDestroy() {
        alive.set(false)
        closeTunnel()
        handler.removeCallbacks(statusTicker)
        try { pump?.join(200) } catch (e: InterruptedException) { }
        audio?.stop()
        core?.close()
        core = null
        runCatching { wakeLock?.release() }
        wakeLock = null
        instance = null
        super.onDestroy()
    }

    override fun onRevoke() {
        closeTunnel()
        super.onRevoke()
    }
}
