package com.noiseinternet.core

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import android.os.Build
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

/**
 * Full-duplex PCM plumbing: [AudioRecord] (microphone) feeds the native engine, and [AudioTrack]
 * (speaker) plays whatever the engine wants to emit. The engine itself never touches Android audio -
 * it works on float arrays - which is what keeps this class about buffer sizes and nothing else.
 *
 * Two decisions in here matter for the physical tests, and both are reported to the UI rather than
 * hidden:
 *
 *  1. **Input source.** `UNPROCESSED` (API 24+) is tried first because phone HALs apply noise
 *     suppression, AGC and echo cancellation to `MIC`/`VOICE_RECOGNITION`, all of which the modem
 *     depends on *not* happening - the burst is a calibrated signal, and a HAL that "cleans it up"
 *     changes the amplitudes the FEC was tuned for. Whatever source we end up with is recorded in
 *     [inputSourceName] and shown, because a test run on `MIC` is not comparable to one on
 *     `UNPROCESSED`.
 *  2. **Route.** `getRoutedDevice()` tells us whether the sound actually left the loudspeaker or went
 *     to headphones/BT. An acoustic link with a wired headset attached is not a link at all, so the
 *     service warns when the route is not the built-in speaker/microphone.
 *
 * Status: never compiled or run (no Android SDK in the authoring sandbox).
 */
class AudioEngine(
    private val context: Context,
    private val sampleRate: Int,
    private val sink: Sink,
) {

    interface Sink {
        /** @return frames consumed by the engine; the pump leaves the rest in the ring. */
        fun onCapture(buf: FloatArray, n: Int): Int

        /** Ask the engine for up to [cap] frames of speaker output; @return frames produced. */
        fun onPull(buf: FloatArray, cap: Int): Int
    }

    val running = AtomicBoolean(false)
    val inputSourceName = AtomicInteger(-1)
    @Volatile
    var routedInput: String = "unknown"
        private set
    @Volatile
    var routedOutput: String = "unknown"
        private set
    @Volatile
    var underruns: Int = -1
        private set
    @Volatile
    var overruns: Int = 0
        private set
    @Volatile
    var lastError: String = ""
        private set

    private var rec: AudioRecord? = null
    private var track: AudioTrack? = null
    // Frames this class has handed to the AudioTrack. `AudioTrack.getFramesWritten()` is a hidden API
    // (the Kotlin build failed on it: "Unresolved reference"), so the count is kept here from write()'s
    // own return value, which is public and exact.
    @Volatile
    private var framesQueued: Long = 0
    private var inThread: Thread? = null
    private var outThread: Thread? = null
    private val mgr: AudioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager

    private fun deviceKind(d: AudioDeviceInfo?): String = when (d?.type) {
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "built-in speaker"
        AudioDeviceInfo.TYPE_BUILTIN_MIC -> "built-in microphone"
        AudioDeviceInfo.TYPE_WIRED_HEADSET, AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> "WIRED HEADSET"
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO, AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "BLUETOOTH"
        null -> "not reported"
        else -> "type " + d.type
    }

    /**
     * @param input preferred capture device, or null for the default. Preferred devices need API 23.
     */
    /** Requires RECORD_AUDIO, granted by the activity before this is called. */
    fun start(input: AudioDeviceInfo?, output: AudioDeviceInfo?): Boolean {
        if (running.get()) return true
        val minIn = AudioRecord.getMinBufferSize(
            sampleRate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_FLOAT
        )
        val minOut = AudioTrack.getMinBufferSize(
            sampleRate, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_FLOAT
        )
        if (minIn < 0 || minOut < 0) {
            lastError = "device refuses 32-bit float mono PCM at $sampleRate Hz " +
                "(minIn=$minIn minOut=$minOut) - the modem needs float to avoid a second quantiser"
            return false
        }
        val frames = maxOf(minIn, minOut, sampleRate / 50)  // >= 20 ms per callback

        var rec0: AudioRecord? = null
        var chosen = -1
        // API 31 renamed the constant; both spellings are tried, oldest-first fallback chain.
        val candidates = ArrayList<Pair<Int, String>>()
        if (Build.VERSION.SDK_INT >= 24) {
            candidates.add(
                MediaRecorder.AudioSource.UNPROCESSED to "UNPROCESSED"
            )
        }
        candidates.add(MediaRecorder.AudioSource.VOICE_RECOGNITION to "VOICE_RECOGNITION")
        candidates.add(MediaRecorder.AudioSource.MIC to "MIC")
        for ((src, name) in candidates) {
            try {
                val r = AudioRecord(
                    src, sampleRate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_FLOAT,
                    frames * 4
                )
                if (r.state == AudioRecord.STATE_INITIALIZED) {
                    rec0 = r
                    chosen = src
                    inputSourceName.set(src)
                    break
                }
                r.release()
            } catch (e: Exception) {
                lastError = "AudioRecord($name) failed: ${e.message}"
            }
        }
        val r = rec0 ?: run {
            if (lastError.isEmpty()) lastError = "no usable AudioRecord source"
            return false
        }
        var preferredIgnored = false
        input?.let {
            if (Build.VERSION.SDK_INT >= 23) preferredIgnored = !r.setPreferredDevice(it)
        }

        val t = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    // USAGE_MEDIA keeps the call/voice-processing path out of the signal; the modem is
                    // not voice, and routing it through the voice pipeline invites the same HAL
                    // processing we avoided on the input.
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build()
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                    .setSampleRate(sampleRate)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .build()
            )
            .setBufferSizeInBytes(frames * 4)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        output?.let { if (Build.VERSION.SDK_INT >= 23 && !t.setPreferredDevice(it)) preferredIgnored = true }

        rec = r
        track = t
        try {
            r.startRecording()
            t.play()
        } catch (e: Exception) {
            lastError = "start failed: ${e.message}"
            stop()
            return false
        }
        framesQueued = 0
        running.set(true)

        val inBuf = FloatArray(frames)
        inThread = Thread({
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)
            while (running.get()) {
                val n = r.read(inBuf, 0, frames, AudioFormat.ENCODING_PCM_FLOAT)
                if (n > 0) {
                    sink.onCapture(inBuf, n)
                } else if (n < 0) {
                    lastError = "AudioRecord.read = $n"
                    running.set(false)
                }
            }
        }, "ni-capture").apply { isDaemon = true; start() }

        val outBuf = FloatArray(frames)
        outThread = Thread({
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)
            while (running.get()) {
                val n = sink.onPull(outBuf, frames)
                if (n > 0) {
                    val w = t.write(outBuf, 0, n, AudioTrack.WRITE_BLOCKING)
                    if (w < n) overruns++
                    if (w > 0) framesQueued += w.toLong()
                } else {
                    Thread.sleep(4)
                }
                routeProbe()
            }
        }, "ni-render").apply { isDaemon = true; start() }

        if (preferredIgnored) lastError = "setPreferredDevice was refused by the platform"
        routeProbe()
        return true
    }

    private fun routeProbe() {
        if (Build.VERSION.SDK_INT >= 24) {
            try {
                rec?.routedDevice?.let { routedInput = deviceKind(it) }
                track?.routedDevice?.let { routedOutput = deviceKind(it) }
            } catch (e: Exception) {
                routedInput = "probe failed"
            }
            try {
                underruns = track?.underrunCount ?: -1
            } catch (e: Exception) {
                underruns = -1
            }
        }
    }

    fun stop() {
        running.set(false)
        try { inThread?.join(200) } catch (e: InterruptedException) { }
        try { outThread?.join(200) } catch (e: InterruptedException) { }
        inThread = null
        outThread = null
        try { rec?.stop() } catch (e: Exception) { }
        rec?.release()
        rec = null
        try { track?.stop() } catch (e: Exception) { }
        try { track?.release() } catch (e: Exception) { }
        track = null
    }

    /**
     * Frames the AudioTrack has accepted but not yet played. The half-duplex scheduler times its next
     * burst from when the previous one finished *playing*, so this number is what stops an endpoint
     * from talking over its peer's reply - and until it existed, the native link assumed a burst was
     * over the instant the samples left the library, up to a whole buffer early.
     *
     * -1 means "this platform will not tell us", which is reported as 0 frames of backlog: the link
     * then behaves as it did before, rather than being handed an invented number.
     */
    fun queuedFrames(): Long {
        val t = track ?: return -1
        if (Build.VERSION.SDK_INT < 23) return -1
        return try {
            val head = t.playbackHeadPosition.toLong() and 0xFFFFFFFFL   // it wraps at 2^32
            val inFlight = framesQueued - head
            val cap = t.bufferSizeInFrames.toLong()
            when {
                inFlight < 0 -> 0L          // the head moved past our count: the track was flushed
                inFlight > cap -> cap       // never report more than the buffer can hold
                else -> inFlight
            }
        } catch (e: Exception) {
            -1
        }
    }

    /** What the status panel prints under "audio". Everything here is measured by the platform. */
    fun describe(): String = buildString {
        append(sampleRate).append(" Hz mono float, period ")
        append(maxOf(1, sampleRate / 50)).append(" frames, input source ")
        append(sourceName(inputSourceName.get()))
        append(", capture route ").append(routedInput)
        append(", playback route ").append(routedOutput)
        if (underruns >= 0) append(", track underruns ").append(underruns)
        if (overruns > 0) append(", write short-counts ").append(overruns)
        if (lastError.isNotEmpty()) append(", last error: ").append(lastError)
    }

    private fun sourceName(src: Int): String = when (src) {
        MediaRecorder.AudioSource.MIC -> "MIC"
        MediaRecorder.AudioSource.VOICE_RECOGNITION -> "VOICE_RECOGNITION"
        else -> if (Build.VERSION.SDK_INT >= 24 && src == MediaRecorder.AudioSource.UNPROCESSED) {
            "UNPROCESSED"
        } else {
            "source #$src"
        }
    }

    /** Devices the user can actually pick, from the platform - not a hard-coded list. */
    fun listInputs(): List<AudioDeviceInfo> =
        if (Build.VERSION.SDK_INT >= 23) mgr.getDevices(AudioManager.GET_DEVICES_INPUTS).toList()
        else emptyList()

    fun listOutputs(): List<AudioDeviceInfo> =
        if (Build.VERSION.SDK_INT >= 23) mgr.getDevices(AudioManager.GET_DEVICES_OUTPUTS).toList()
        else emptyList()

    /**
     * True when the built-in microphone and speaker are both present and selected. Anything else
     * (headset, BT, screen-off route changes) makes an "over the air" claim wrong, so the UI shows a
     * warning instead of results.
     */
    fun acousticRouteOk(): Boolean =
        routedInput.startsWith("built-in") && routedOutput.startsWith("built-in")
}
