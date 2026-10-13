package com.noiseinternet.core

import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException

/**
 * The whole native surface, in one file, on purpose. Every method is a direct forwarder to the flat C
 * ABI in `shared/acoustic_core/include/ni/abi.h`; there is no protocol, DSP or crypto logic in Kotlin,
 * so the Android app cannot behave differently from the Windows app or the CLI except through a bug in
 * the JNI layer.
 *
 * Status: never compiled or run (no NDK/JDK in the authoring sandbox - see ../../../../BUILD.md).
 */
class NoiseCore(config: Config) {

    /** Everything the core needs to build a node, as the JSON the ABI parses. */
    data class Config(
        val name: String = "android",
        val role: Role = Role.RECEIVER,
        val profile: Int = 2,               // DBPSK-MID, the profile the self-test ranked best
        val rxProfile: Int = -1,            // FDD only
        val sampleRate: Int = 48000,
        val txAmplitude: Double = 0.85,
        val sessionId: Int = 0,
        val pairingSecret: String = "",
        val duplex: Boolean = false,
        val proxyPort: Int = 8471,
        val proxyEnable: Boolean = true,
        val proxyToken: String = "",
        val allowPlaintextTest: Boolean = false,
    ) {
        fun toJson(): String = JSONObject().apply {
            put("name", name)
            put("role", role.wire)
            put("profile", profile)
            put("fs", sampleRate)
            put("tx_amplitude", txAmplitude)
            put("session_id", sessionId)
            put("pairing_secret", pairingSecret)
            put("duplex", if (duplex) 1 else 0)
            if (rxProfile >= 0) put("rx_profile", rxProfile)
            put("proxy", JSONObject().apply {
                put("port", proxyPort)
                put("bind", "127.0.0.1")   // never a routable address: see BUILD.md security notes
                put("enable", proxyEnable)
                if (proxyToken.isNotEmpty()) put("token", proxyToken)
            })
            put("allow_plaintext_test", allowPlaintextTest)
        }.toString()
    }

    enum class Role(val wire: String) { GIVER("giver"), RECEIVER("receiver") }

    private var nativePtr: Long = 0L

    private external fun nativeCreate(configJson: String): Long
    private external fun nativeDestroy()

    external fun start(): Int
    external fun stop(): Int
    external fun step(nowMs: Long): Int
    external fun feedPcm(pcm: FloatArray, nFrames: Int): Int
    external fun pullPcm(out: FloatArray, cap: Int): Int
    external fun pullPcmNeeded(): Int
    external fun statusJson(): String
    external fun reportJson(): String
    external fun sendBytes(stream: Int, type: Int, data: ByteArray, len: Int, flags: Int): Int
    external fun calibrate(nowMs: Long): String
    external fun testLink(nowMs: Long, payloadBytes: Int): String
    external fun testInternet(nowMs: Long, url: String): String
    external fun renderTones(
        profile: Int, fs: Double, amplitude: Double, sessionId: Int,
        payload: ByteArray, out: FloatArray
    ): Int


    companion object {
        @Volatile
        private var loaded = false

        /** Library name must match the AAR/CMake target in app/src/main/jni/CMakeLists.txt. */
        @Synchronized
        fun loadOnce() {
            if (loaded) return
            System.loadLibrary("noiseinternet_jni")
            loaded = true
        }

        /** True after [loadOnce]; the UI uses this to say "native core missing" instead of crashing. */
        fun isLoaded(): Boolean = loaded
    }

    init {
        loadOnce()
        nativePtr = nativeCreate(config.toJson())
        if (nativePtr == 0L) throw IOException("ni_abi_create rejected the configuration")
    }

    fun close() {
        if (nativePtr != 0L) nativeDestroy()
    }

    /** Parsed view of ni_abi_status_json(). Unset numbers stay null rather than defaulting to 0. */
    data class Status(
        val state: String,
        val role: String,
        val profile: String,
        val sampleRate: Double?,
        val peerOnline: Boolean,
        val snrDb: Double?,
        val rssiDbfs: Double?,
        val noiseDbfs: Double?,
        val evecPct: Double?,
        val rttMs: Double?,
        val lossPct: Double?,
        val usefulBpsUp: Double?,
        val usefulBpsDown: Double?,
        val framesSent: Long?,
        val framesReceived: Long?,
        val retries: Long?,
        val crcFail: Long?,
        val bytesSent: Long?,
        val bytesReceived: Long?,
        val mtu: Int?,
        val tunnelUp: Boolean,
        val internetOk: Boolean,
        val audioNote: String,
        val raw: String,
    ) {
        companion object {
            fun parse(json: String): Status {
                val o = try {
                    JSONObject(json)
                } catch (e: Exception) {
                    // A malformed status document is a bug worth showing, never worth hiding behind
                    // plausible zeros.
                    return Status(
                        state = "unparsable", role = "-", profile = "-", sampleRate = null,
                        peerOnline = false, snrDb = null, rssiDbfs = null, noiseDbfs = null,
                        evecPct = null, rttMs = null, lossPct = null, usefulBpsUp = null,
                        usefulBpsDown = null, framesSent = null, framesReceived = null,
                        retries = null, crcFail = null, bytesSent = null, bytesReceived = null,
                        mtu = null, tunnelUp = false, internetOk = false,
                        audioNote = "status JSON could not be parsed: ${e.message}", raw = json,
                    )
                }

                fun d(k: String): Double? = if (o.isNull(k)) null else o.optDouble(k)
                fun l(k: String): Long? = if (o.isNull(k)) null else o.optLong(k)
                return Status(
                    state = o.optString("state_name", o.optString("state", "-")),
                    role = o.optString("role", "-"),
                    profile = o.optString("profile_name", "-"),
                    sampleRate = d("sample_rate"),
                    peerOnline = o.optBoolean("peer_online", false),
                    snrDb = d("snr_db"),
                    rssiDbfs = d("rssi_dbfs"),
                    noiseDbfs = d("noise_dbfs"),
                    evecPct = d("evec_pct"),
                    rttMs = d("rtt_ms"),
                    lossPct = d("loss_pct"),
                    usefulBpsUp = d("useful_bps_up"),
                    usefulBpsDown = d("useful_bps_down"),
                    framesSent = l("frames_sent"),
                    framesReceived = l("frames_received"),
                    retries = l("retries"),
                    crcFail = l("crc_fail"),
                    bytesSent = l("bytes_sent"),
                    bytesReceived = l("bytes_received"),
                    mtu = if (o.isNull("mtu")) null else o.optInt("mtu"),
                    tunnelUp = o.optBoolean("tunnel_up", false),
                    internetOk = o.optBoolean("internet_ok", false),
                    audioNote = o.optString("audio_note", ""),
                    raw = json,
                )
            }
        }

        fun f(v: Double?, fmt: String = "%.1f"): String = v?.let { String.format(fmt, it) } ?: "-"
        fun n(v: Number?): String = v?.toString() ?: "-"
    }

    /** Calibration report -> rows for the UI, straight out of ni_abi_calibrate's JSON. */
    data class CalibRow(
        val profile: String,
        val snrDb: Double,
        val okPct: Double,
        val rawBer: Double,
        val fecBer: Double,
        val bps: Double,
        val usable: Boolean,
        val note: String,
    )

    fun calibrateRows(json: String): List<CalibRow> {
        val out = ArrayList<CalibRow>()
        try {
            val o = JSONObject(json)
            val bands: JSONArray? = o.optJSONArray("bands")
            if (bands != null) {
                for (i in 0 until bands.length()) {
                    val b = bands.getJSONObject(i)
                    out.add(
                        CalibRow(
                            profile = b.optString("name", b.optString("profile", "-")),
                            snrDb = b.optDouble("snr_db", 0.0),
                            okPct = b.optDouble("frame_success", 0.0) * 100.0,
                            rawBer = b.optDouble("raw_ber", 0.0),
                            fecBer = b.optDouble("fec_ber", 0.0),
                            bps = b.optDouble("payload_bps", 0.0),
                            usable = b.optBoolean("usable", false),
                            note = b.optString("note", "")
                        )
                    )
                }
            }
        } catch (e: Exception) {
            out.add(CalibRow("error", 0.0, 0.0, 0.0, 0.0, 0.0, false, "unparsable: ${e.message}"))
        }
        return out
    }

    @Throws(IOException::class)
    private fun ensure(rc: Int, what: String): Int {
        if (rc < 0) throw IOException("$what failed with $rc")
        return rc
    }

    fun startChecked() = ensure(start(), "start")
    fun testLinkChecked(bytes: Int) = testLink(System.currentTimeMillis(), bytes)
}

/**
 * The stateless PHY entry points from the same C ABI - render a burst into samples, decode a recording
 * back to bytes, read the profile table. They are on an `object` rather than on [NoiseCore] because
 * they need no node: a Kotlin `object` gives them plain `Java_com_noiseinternet_core_PhyTools_*` JNI
 * names, while companion-object natives would be mangled.
 */
object PhyTools {

    init {
        NoiseCore.loadOnce()
    }

    external fun profilesJson(): String
    external fun version(): String
    external fun audioEnumerate(): String
    external fun renderTones(
        profile: Int, fs: Double, amplitude: Double, sessionId: Int,
        payload: ByteArray, out: FloatArray
    ): Int

    /** @param meta 2 doubles, filled with {measured SNR dB, peak amplitude}. */
    external fun decodePcm(
        profile: Int, fs: Double, sessionId: Int, pcm: FloatArray, meta: DoubleArray
    ): ByteArray

    /**
     * The core's answer when it has no way to know: NI_ERR_UNSUPPORTED as JSON. Callers must show this
     * string rather than treating an empty list as "no devices found", because they are different
     * facts and only one of them is a pass.
     */
    fun audioOrError(): String = try {
        audioEnumerate()
    } catch (e: Exception) {
        "{\"error\":\"\${e.message}\"}"
    }
}
