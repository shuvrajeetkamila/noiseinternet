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
    // Which capture format is actually delivering. The first field test failed here: the phone accepted
    // a 32-bit-float UNPROCESSED AudioRecord, reported the right route, and then returned -2
    // (ERROR_BAD_VALUE) from every single read() - so the engine never received one sample, the phone
    // was deaf, and the only trace was a raw "-2" in a log line while the panel showed a level of -120.
    // The capture path now retries, falls back to 16-bit PCM (which every device supports) and keeps
    // playback running whatever happens on the input side.
    @Volatile
    private var capEncoding = AudioFormat.ENCODING_PCM_FLOAT
    @Volatile
    private var capReadFailures = 0
    @Volatile
    private var capRebuilds = 0
    @Volatile
    private var capFrames = 0

    // Set by the capture thread: `capOpen` once a recorder is really recording, `capDead` once every
    // source/encoding combination on the device has been refused. start() waits briefly on these so a
    // phone whose microphone cannot be opened says so immediately instead of running a deaf engine.
    @Volatile private var capOpen = false
    @Volatile private var capDead = false
    // The microphone the user picked, remembered for every (re)open - the recorder is created by the
    // capture thread now, so start() can no longer attach the preference to an object it owns.
    @Volatile private var prefInput: AudioDeviceInfo? = null
    // The rates the platform actually gave us, which are not always the rate we asked for: Android
    // silently resamples when the hardware runs at 44.1 kHz. A resampled burst is audible and
    // undecodable, so the panel has to say which rate is real (see NoiseService.nativeSampleRate).
    @Volatile private var capRate = 0
    @Volatile private var outRate = 0
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
        // Capture is asked for in 16-bit first for the *requirement check*, because 16-bit is what
        // every Android device is guaranteed to support and the capture loop converts it to float
        // itself. Float is still preferred at open time - it skips one quantiser - but a device that
        // refuses float capture is no longer a device that cannot run this app, which is what the old
        // check said.
        var minIn = AudioRecord.getMinBufferSize(
            sampleRate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT
        )
        if (minIn <= 0) minIn = AudioRecord.getMinBufferSize(
            sampleRate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_FLOAT
        )
        val minOut = AudioTrack.getMinBufferSize(
            sampleRate, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_FLOAT
        )
        if (minIn <= 0 || minOut <= 0) {
            lastError = "this device refuses mono PCM capture/playback at $sampleRate Hz " +
                "(minIn=$minIn minOut=$minOut)"
            return false
        }
        val frames = maxOf(minIn, minOut, sampleRate / 50)  // >= 20 ms per callback

        var preferredIgnored = false

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

        // One recorder, owned by the capture thread. This used to open a float recorder here, pick a
        // source here, and then captureLoop() opened a *second* recorder with its own encoding and
        // source choice - the first one was never released, both held the microphone, and the failure
        // that reached the panel ("AudioRecord.read = -2") came from whichever one the loop happened to
        // be reading. openInput()/sourceCandidates() are now the only place a recorder is created.
        track = t
        try {
            t.play()
        } catch (e: Exception) {
            lastError = "start failed: ${e.message}"
            stop()
            return false
        }
        framesQueued = 0
        running.set(true)
        outRate = if (Build.VERSION.SDK_INT >= 23) t.sampleRate else sampleRate

        prefInput = input
        capFrames = frames
        capEncoding = AudioFormat.ENCODING_PCM_FLOAT
        capOpen = false
        capDead = false
        inThread = Thread({
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)
            captureLoop()
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

        // Give the capture thread a moment to open the microphone, so that "the engine is running and
        // the microphone is dead" cannot be mistaken for "everything is fine". 1.5 s is short enough
        // that the user sees it, long enough that a slow device is not failed spuriously.
        val deadline = System.currentTimeMillis() + 1500
        while (System.currentTimeMillis() < deadline && !capOpen && !capDead) {
            try { Thread.sleep(20) } catch (e: InterruptedException) { }
        }
        if (!capOpen) {
            if (capDead) {
                if (lastError.isEmpty()) lastError = "no usable AudioRecord source"
                lastError = "microphone could not be opened: $lastError"
                stop()
                return false
            }
            // Still trying (a slow device, or a source that takes its time): keep running, but say so.
            lastError = "microphone has not opened yet (" +
                "recording will start when it does): " + lastError
        }
        routeProbe()
        return true
    }

    /**
     * Microphone -> engine, with the failure handling that the first field test proved was missing.
     *
     * What went wrong without it: `read()` returned -2 for every block, one negative return set
     * `running = false`, and that stopped the *playback* loop too - so the app became both deaf and mute
     * while still looking started, and the only evidence was the string "AudioRecord.read = -2".
     *
     * Now: a failed read is retried; after [kReadsBeforeRebuild] consecutive failures the capture is
     * reopened as 16-bit PCM (universally supported, converted to float here); if that fails too, the
     * next input source in the chain is tried. Playback is independent and keeps running throughout.
     * Every rebuild is counted and reported, so "the microphone is not working" can never again be a
     * mystery: the audio line says which format is running and how many reads failed.
     */
    private fun captureLoop() {
        val kReadsBeforeRebuild = 12
        var fails = 0
        var sources = sourceCandidates()
        var si = 0
        val inBuf = FloatArray(capFrames)
        val shortBuf = ShortArray(capFrames)
        while (running.get()) {
            val r = rec
            if (r == null) {
                // (re)open: try the current source with the current encoding, then 16-bit, then move on
                val encodings = if (capEncoding == AudioFormat.ENCODING_PCM_FLOAT)
                    intArrayOf(AudioFormat.ENCODING_PCM_FLOAT, AudioFormat.ENCODING_PCM_16BIT)
                else intArrayOf(AudioFormat.ENCODING_PCM_16BIT)
                var opened = false
                for (enc in encodings) {
                    val src = sources[si % sources.size]
                    val nr = openInput(src.first, enc, capFrames)
                    if (nr != null) {
                        rec = nr
                        capEncoding = enc
                        inputSourceName.set(src.first)
                        lastError = if (enc == AudioFormat.ENCODING_PCM_16BIT)
                            "capture opened as 16-bit PCM (this device refused 32-bit float reads)"
                        else lastError
                        if (Build.VERSION.SDK_INT >= 23) prefInput?.let { pd ->
                            if (!nr.setPreferredDevice(pd))
                                lastError = "the chosen microphone was refused by the platform; " +
                                    "using the device default instead"
                        }
                        try { nr.startRecording() } catch (e: Exception) {
                            lastError = "startRecording failed: ${e.message}"
                            try { nr.release() } catch (e2: Exception) { }
                            rec = null
                            continue
                        }
                        capRebuilds++
                        capOpen = true
                        capRate = if (Build.VERSION.SDK_INT >= 23) nr.sampleRate else sampleRate
                        fails = 0
                        opened = true
                        break
                    }
                    lastError = "AudioRecord(${src.second}, ${encodingName(enc)}) could not be created"
                }
                if (!opened) {
                    si++
                    if (si >= sources.size) {
                        // Every source, in both encodings, has been refused. This is not a transient
                        // failure: say so once, keep retrying in the background, and keep playback
                        // running - the far end may still be able to reach us.
                        capDead = true
                        Thread.sleep(500)
                        si = 0
                    }
                    continue
                }
                continue
            }
            // The two encodings have separate read() overloads - float needs the FloatArray one and
            // 16-bit needs the ShortArray one, and mixing them throws - so the 16-bit path is read into
            // a ShortArray and converted here. The engine only ever sees float.
            val n = if (capEncoding == AudioFormat.ENCODING_PCM_FLOAT) {
                r.read(inBuf, 0, inBuf.size, AudioFormat.ENCODING_PCM_FLOAT)
            } else {
                val m = r.read(shortBuf, 0, shortBuf.size)
                if (m > 0) for (i in 0 until m) inBuf[i] = shortBuf[i] / 32768f
                m
            }
            if (n > 0) {
                fails = 0
                sink.onCapture(inBuf, n)
            } else {
                fails++
                capReadFailures++
                if (fails >= kReadsBeforeRebuild) {
                    // Give up on this recorder: close it, and let the top of the loop reopen (16-bit
                    // first if we were on float) or move to the next source.
                    lastError = "capture read failed $fails times in a row (last code $n): reopening"
                    try { r.stop() } catch (e: Exception) { }
                    try { r.release() } catch (e: Exception) { }
                    rec = null
                    fails = 0
                } else {
                    Thread.sleep(10)
                }
            }
        }
        try { rec?.stop() } catch (e: Exception) { }
        try { rec?.release() } catch (e: Exception) { }
        rec = null
    }

    /** The capture sources this platform will accept, most processed-avoiding first. */
    private fun sourceCandidates(): List<Pair<Int, String>> {
        val out = ArrayList<Pair<Int, String>>()
        if (Build.VERSION.SDK_INT >= 24)
            out.add(MediaRecorder.AudioSource.UNPROCESSED to "UNPROCESSED")
        out.add(MediaRecorder.AudioSource.VOICE_RECOGNITION to "VOICE_RECOGNITION")
        out.add(MediaRecorder.AudioSource.MIC to "MIC")
        return out
    }

    private fun encodingName(enc: Int): String = when (enc) {
        AudioFormat.ENCODING_PCM_FLOAT -> "32-bit float"
        AudioFormat.ENCODING_PCM_16BIT -> "16-bit PCM"
        else -> "encoding #$enc"
    }

    private fun openInput(src: Int, encoding: Int, frames: Int): AudioRecord? = try {
        val minBytes = AudioRecord.getMinBufferSize(sampleRate, AudioFormat.CHANNEL_IN_MONO, encoding)
        val bytes = if (minBytes > 0) maxOf(minBytes, frames * bytesPerSample(encoding)) else frames * bytesPerSample(encoding)
        val r = AudioRecord(src, sampleRate, AudioFormat.CHANNEL_IN_MONO, encoding, bytes)
        if (r.state == AudioRecord.STATE_INITIALIZED) r else { r.release(); null }
    } catch (e: Exception) {
        lastError = "AudioRecord($src, ${encodingName(encoding)}) threw: ${e.message}"
        null
    }

    private fun bytesPerSample(encoding: Int): Int =
        if (encoding == AudioFormat.ENCODING_PCM_FLOAT) 4 else 2

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
        append(maxOf(1, sampleRate / 50)).append(" frames, capture ")
        append(encodingName(capEncoding)).append(" from ")
        append(if (inputSourceName.get() >= 0) sourceName(inputSourceName.get()) else "no source yet")
        if (!capOpen) append(", CAPTURE NOT OPEN")
        if (capDead) append(" (every input source this device offers was refused)")
        if (capRate > 0 && outRate > 0 && (capRate != sampleRate || outRate != sampleRate))
            append(", DEVICE RESAMPLED (asked $sampleRate, capture $capRate, playback $outRate Hz)")
        if (capReadFailures > 0) append(", failed reads ").append(capReadFailures)
        if (capRebuilds > 1) append(", capture reopened ").append(capRebuilds - 1).append(" times")
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
