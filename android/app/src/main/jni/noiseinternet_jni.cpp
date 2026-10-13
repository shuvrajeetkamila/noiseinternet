// noiseinternet_jni.cpp — JNI bridge between the Kotlin app and the portable C core.
//
// The bridge is deliberately thin: it converts arrays/strings and forwards to the flat C ABI in
// shared/acoustic_core/include/ni/abi.h. There is no second implementation of anything here, which is
// the point of the ABI - if a behaviour differs between Android and Windows, that is a bug in this
// file and nowhere else.
//
// NEVER COMPILED (no NDK in the authoring sandbox). The native method names below must match
// android/app/src/main/java/com/noiseinternet/core/NoiseCore.kt, and the signatures are what
// `javac -h` would generate - re-run that header step when the Kotlin side changes.
#include <jni.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "ni/abi.h"
#include "ni/common.h"  // ni_strerror, NI_ERR_* codes

namespace {

jclass g_core_class = nullptr;
jfieldID g_ptr_field = nullptr;

NiNode* node_of(JNIEnv* env, jobject self) {
  return reinterpret_cast<NiNode*>((intptr_t)env->GetLongField(self, g_ptr_field));
}

// A negative NI_* code becomes a Java exception, because "rc = -10" is not something a UI should have
// to remember; the message carries ni_strerror() so the user sees the reason, not a number.
jint check(JNIEnv* env, jint rc, const char* what) {
  if (rc < 0) {
    char buf[192];
    snprintf(buf, sizeof(buf), "%s failed: %s (%d)", what, ni_strerror((int)rc), (int)rc);
    env->ThrowNew(env->FindClass("java/io/IOException"), buf);
  }
  return rc;
}

jstring json_or_error(JNIEnv* env, int rc, char* owned) {
  std::string s = owned && *owned ? owned : "{}";
  if (rc < 0) s = "{\"error\":\"" + s + "\",\"rc\":" + std::to_string((int)rc) + "}";
  return env->NewStringUTF(s.c_str());
}

}  // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass cls = env->FindClass("com/noiseinternet/core/NoiseCore");
  if (!cls) return JNI_ERR;
  g_core_class = (jclass)env->NewGlobalRef(cls);
  g_ptr_field = env->GetFieldID(cls, "nativePtr", "J");
  if (!g_ptr_field) return JNI_ERR;
  return JNI_VERSION_1_6;
}

JNIEXPORT jlong JNICALL Java_com_noiseinternet_core_NoiseCore_nativeCreate(JNIEnv* env, jobject self,
                                                                           jstring config_json) {
  const char* c = env->GetStringUTFChars(config_json, nullptr);
  char err[256] = {0};
  NiNode* n = ni_abi_create(c ? c : "{}", err, (int)sizeof(err));
  if (c) env->ReleaseStringUTFChars(config_json, c);
  if (!n) {
    env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"), err);
    return 0;
  }
  env->SetLongField(self, g_ptr_field, (jlong)(intptr_t)n);
  return (jlong)(intptr_t)n;
}

JNIEXPORT void JNICALL Java_com_noiseinternet_core_NoiseCore_nativeDestroy(JNIEnv* env, jobject self) {
  NiNode* n = node_of(env, self);
  env->SetLongField(self, g_ptr_field, 0);
  if (n) ni_abi_destroy(n);
}

JNIEXPORT jint JNICALL Java_com_noiseinternet_core_NoiseCore_start(JNIEnv* env, jobject self) {
  return check(env, ni_abi_start(node_of(env, self)), "start");
}

JNIEXPORT jint JNICALL Java_com_noiseinternet_core_NoiseCore_stop(JNIEnv* env, jobject self) {
  return ni_abi_stop(node_of(env, self));
}

JNIEXPORT jint JNICALL Java_com_noiseinternet_core_NoiseCore_step(JNIEnv* env, jobject self,
                                                                   jlong now_ms) {
  return ni_abi_step(node_of(env, self), (int64_t)now_ms);
}

// Microphone -> engine. Returns frames consumed.
JNIEXPORT jint JNICALL Java_com_noiseinternet_core_NoiseCore_feedPcm(JNIEnv* env, jobject self,
                                                                      jfloatArray pcm, jint n) {
  if (!pcm || n <= 0) return 0;
  jfloat* p = env->GetFloatArrayElements(pcm, nullptr);
  jint rc = ni_abi_feed_pcm(node_of(env, self), (const float*)p, (int)n);
  env->ReleaseFloatArrayElements(pcm, p, JNI_ABORT);  // read-only input: never copy back
  return rc;
}

// Engine -> speaker. The Kotlin side owns the AudioTrack buffer and reuses it, so the frames the
// engine wrote are copied back into the same array (mode 0).
JNIEXPORT jint JNICALL Java_com_noiseinternet_core_NoiseCore_pullPcm(JNIEnv* env, jobject self,
                                                                      jfloatArray out, jint cap) {
  if (!out || cap <= 0) return 0;
  jfloat* p = env->GetFloatArrayElements(out, nullptr);
  jint rc = ni_abi_pull_pcm(node_of(env, self), (float*)p, (int)cap);
  env->ReleaseFloatArrayElements(out, p, rc > 0 ? 0 : JNI_ABORT);
  return rc;
}

JNIEXPORT jint JNICALL Java_com_noiseinternet_core_NoiseCore_pullPcmNeeded(JNIEnv* env, jobject self) {
  return ni_abi_pull_pcm_needed(node_of(env, self));
}

JNIEXPORT jstring JNICALL Java_com_noiseinternet_core_NoiseCore_statusJson(JNIEnv* env, jobject self) {
  char* s = ni_abi_status_json(node_of(env, self));
  jstring out = env->NewStringUTF(s ? s : "{}");
  ni_abi_free(s);
  return out;
}

JNIEXPORT jstring JNICALL Java_com_noiseinternet_core_NoiseCore_reportJson(JNIEnv* env, jobject self) {
  char* s = ni_abi_report_json(node_of(env, self));
  jstring out = env->NewStringUTF(s ? s : "{}");
  ni_abi_free(s);
  return out;
}

JNIEXPORT jint JNICALL Java_com_noiseinternet_core_NoiseCore_sendBytes(JNIEnv* env, jobject self,
                                                                        jint stream, jint type,
                                                                        jbyteArray data, jint len,
                                                                        jint flags) {
  if (!data) return NI_ERR_PARAM;
  jbyte* p = env->GetByteArrayElements(data, nullptr);
  jint rc = ni_abi_send_bytes(node_of(env, self), (int)stream, (int)type, (const uint8_t*)p,
                              (int)(len > 0 ? len : 0), (int)flags);
  env->ReleaseByteArrayElements(data, p, JNI_ABORT);
  return rc;
}

// The three long-running actions return their measured report as a JSON string. They block the calling
// thread (the service runs them off the main thread), because they are turn-based exchanges.
JNIEXPORT jstring JNICALL Java_com_noiseinternet_core_NoiseCore_calibrate(JNIEnv* env, jobject self,
                                                                           jlong now_ms) {
  char* out = (char*)calloc(1 << 16, 1);
  jint rc = ni_abi_calibrate(node_of(env, self), (int64_t)now_ms, out, 1 << 16);
  jstring s = json_or_error(env, rc, out);
  free(out);
  return s;
}

JNIEXPORT jstring JNICALL Java_com_noiseinternet_core_NoiseCore_testLink(JNIEnv* env, jobject self,
                                                                          jlong now_ms, jint payload) {
  char* out = (char*)calloc(1 << 16, 1);
  jint rc = ni_abi_test_link(node_of(env, self), (int64_t)now_ms, (int)payload, out, 1 << 16);
  jstring s = json_or_error(env, rc, out);
  free(out);
  return s;
}

JNIEXPORT jstring JNICALL Java_com_noiseinternet_core_NoiseCore_testInternet(JNIEnv* env, jobject self,
                                                                             jlong now_ms,
                                                                             jstring url) {
  const char* u = url ? env->GetStringUTFChars(url, nullptr) : nullptr;
  char* out = (char*)calloc(1 << 16, 1);
  jint rc = ni_abi_test_internet(node_of(env, self), (int64_t)now_ms, u ? u : "", out, 1 << 16);
  if (u) env->ReleaseStringUTFChars(url, u);
  jstring s = json_or_error(env, rc, out);
  free(out);
  return s;
}

// --- stateless helpers: no node argument, so they live on the Kotlin object PhyTools ---
JNIEXPORT jstring JNICALL Java_com_noiseinternet_core_PhyTools_profilesJson(JNIEnv* env, jobject) {
  return env->NewStringUTF(ni_abi_profiles_json());
}

JNIEXPORT jstring JNICALL Java_com_noiseinternet_core_PhyTools_version(JNIEnv* env, jobject) {
  return env->NewStringUTF(ni_abi_version());
}

// On Android the device list comes from the Java audio APIs (AudioManager.getDevices), so this
// returns the core's NI_ERR_UNSUPPORTED payload; the service merges the two lists and shows both.
JNIEXPORT jstring JNICALL Java_com_noiseinternet_core_PhyTools_audioEnumerate(JNIEnv* env, jobject) {
  char* out = (char*)calloc(1 << 14, 1);
  jint rc = ni_abi_audio_enumerate(out, 1 << 14);
  jstring s = json_or_error(env, rc, out);
  free(out);
  return s;
}

// Stateless PHY helpers, used by the "decode a recording" tool in the app.
JNIEXPORT jint JNICALL Java_com_noiseinternet_core_PhyTools_renderTones(JNIEnv* env, jobject,
                                                                          jint profile, jdouble fs,
                                                                          jdouble amplitude,
                                                                          jint session_id,
                                                                          jbyteArray payload,
                                                                          jfloatArray out) {
  if (!payload || !out) return NI_ERR_PARAM;
  jbyte* p = env->GetByteArrayElements(payload, nullptr);
  jsize n = env->GetArrayLength(payload);
  jfloat* o = env->GetFloatArrayElements(out, nullptr);
  jsize cap = env->GetArrayLength(out);
  jint rc = ni_abi_render((int)profile, (double)fs, (double)amplitude, (uint16_t)session_id,
                          (const uint8_t*)p, (int)n, (float*)o, (int)cap);
  env->ReleaseFloatArrayElements(out, o, rc > 0 ? 0 : JNI_ABORT);
  env->ReleaseByteArrayElements(payload, p, JNI_ABORT);
  return rc;
}

// `meta` (optional, 2 elements) is filled with {measured SNR dB, peak} so the caller sees the same
// numbers the CLI prints for a decode, instead of a second copy of the decoder being written in Kotlin.
JNIEXPORT jbyteArray JNICALL Java_com_noiseinternet_core_PhyTools_decodePcm(JNIEnv* env, jobject,
                                                                             jint profile, jdouble fs,
                                                                             jint session_id,
                                                                             jfloatArray pcm,
                                                                             jdoubleArray meta) {
  if (!pcm) return nullptr;
  jsize n = env->GetArrayLength(pcm);
  jfloat* p = env->GetFloatArrayElements(pcm, nullptr);
  std::vector<uint8_t> out(4096);
  double snr = 0.0, peak = 0.0;
  jint got = ni_abi_decode((int)profile, (double)fs, (uint16_t)session_id, (const float*)p, (int)n,
                           &out[0], (int)out.size(), &snr, &peak);
  env->ReleaseFloatArrayElements(pcm, p, JNI_ABORT);
  if (got < 0) {
    env->ThrowNew(env->FindClass("java/io/IOException"), ni_strerror((int)got));
    return nullptr;
  }
  if (meta && env->GetArrayLength(meta) >= 2) {
    jdouble m[2] = {snr, peak};
    env->SetDoubleArrayRegion(meta, 0, 2, m);
  }
  jbyteArray r = env->NewByteArray((jsize)got);
  env->SetByteArrayRegion(r, 0, (jsize)got, (const jbyte*)&out[0]);
  return r;
}

}  // extern "C"
