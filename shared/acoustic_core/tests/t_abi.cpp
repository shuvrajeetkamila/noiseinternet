// t_abi.cpp — the flat C ABI the Android JNI bridge and the Windows shell call.
//
// Why this test exists: the whole "one core, many shells" claim rests on `ni_abi_*` behaving, because
// neither platform shell has ever been compiled here. Everything the shells depend on - creating a node
// from JSON, driving it with step/feed/pull, the refusal code an unpaired node gives a send, and the
// stateless render/decode pair the "play a file / decode a recording" tools use - is checked here in
// plain C, with no C++ types crossing the boundary.
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "ni/abi.h"
#include "ni/common.h"
#include "ni/session.h"
#include "ntest.h"

using namespace ni;  // the NI_* status codes and MsgType live in the ni namespace

namespace {

std::string count_str(const std::string& hay, const std::string& needle) {
  int n = 0;
  for (size_t i = 0; (i = hay.find(needle, i)) != std::string::npos; i += needle.size()) n++;
  return std::to_string(n);
}

const char* kGoodConfig =
    "{\"name\":\"abi-test\",\"role\":\"giver\",\"profile\":3,\"fs\":48000,\"session_id\":7,"
    "\"pairing_secret\":\"long enough to be unguessable\",\"duplex\":0}";

// The config both apps ship with, except for the passphrase the user is expected to type. Start() must
// refuse it: without a passphrase no challenge is ever sent, so the two ends cannot pair, and the
// failure the user sees ("it never pairs") used to have no cause visible anywhere.
const char* kNoSecretConfig =
    "{\"name\":\"abi-test\",\"role\":\"giver\",\"profile\":10,\"fs\":48000,\"session_id\":7,"
    "\"pairing_secret\":\"\",\"duplex\":0}";
const char* kShortSecretConfig =
    "{\"name\":\"abi-test\",\"role\":\"giver\",\"profile\":10,\"fs\":48000,\"session_id\":7,"
    "\"pairing_secret\":\"short\",\"duplex\":0}";

}  // namespace

int main() {
  // ---- version and the profile table, which the UIs render verbatim ----
  const char* ver = ni_abi_version();
  NT_CHECK_MSG(ver && *ver, "ni_abi_version must return a non-empty string");
  const char* pj = ni_abi_profiles_json();
  NT_CHECK(pj != nullptr);
  std::string profiles = pj ? pj : "";
  NT_CHECK_MSG(profiles.find("DBPSK-MID") != std::string::npos,
               "the profile table the spinners are built from must name the profiles");
  NT_CHECK_MSG(profiles.find("payload_bits") != std::string::npos ||
                   profiles.find("max_payload") != std::string::npos,
               "the table must expose the per-profile payload limit the UI uses to size a message");
  // 16 profiles in the C++ table. If this ever prints a different count, the JSON spec file, the UI
  // lists and the modem have drifted - which is a real bug, not a cosmetic one.
  NT_CHECK_MSG(atoi(count_str(profiles, "\"id\"").c_str()) == 16,
               "profile count in the ABI table must match the C++ table (16)");

  // ---- a bad config must refuse loudly, not quietly build something else ----
  char err[256];
  err[0] = 0;
  NiNode* bad = ni_abi_create("this is not json at all", err, (int)sizeof(err));
  NT_CHECK_MSG(bad == nullptr, "ni_abi_create must reject a config that is not an object");
  NT_CHECK_MSG(err[0] != 0, "and must say why");

  // ---- a start that cannot pair must be refused, in words a support conversation can use ----
  for (const char* cfg : {kNoSecretConfig, kShortSecretConfig}) {
    err[0] = 0;
    NiNode* ns = ni_abi_create(cfg, err, (int)sizeof(err));
    NT_CHECK_MSG(ns != nullptr, "a config with an empty or short passphrase is still a valid config");
    if (ns) {
      int rc = ni_abi_start(ns);
      NT_CHECK_MSG(rc != NI_OK, "start must refuse a passphrase that cannot pair");
      // The reason is asserted through the status JSON, because that is the surface both apps render:
      // a refusal the UI cannot show is a refusal the user cannot act on.
      char* sj = ni_abi_status_json(ns);
      std::string w = sj ? sj : "";
      ni_abi_free(sj);
      NT_CHECK_MSG(w.find("passphrase") != std::string::npos && w.find("8") != std::string::npos,
                   "and the status JSON the two panels render must name the passphrase and the length");
      ni_abi_destroy(ns);
    }
  }

  // ---- the lifecycle a shell actually drives ----
  err[0] = 0;
  NiNode* n = ni_abi_create(kGoodConfig, err, (int)sizeof(err));
  NT_CHECK_MSG(n != nullptr, "ni_abi_create with a valid config");
  if (n) {
    NT_EQ(ni_abi_start(n), NI_OK);
    NT_EQ(ni_abi_step(n, 1000), NI_OK);
    NT_EQ(ni_abi_step(n, 1100), NI_OK);

    char* st = ni_abi_status_json(n);
    std::string s = st ? st : "";
    ni_abi_free(st);  // the ABI allocates; the caller frees, and double-freeing would show up here
    NT_CHECK_MSG(s.find("\"role\"") != std::string::npos, "status JSON carries the role");
    NT_CHECK_MSG(s.find("\"state_name\"") != std::string::npos, "status JSON carries the state name");
    NT_CHECK_MSG(s.find("NOT TESTED") != std::string::npos || s.find("audio") != std::string::npos,
                 "status JSON must carry the audio caveat so no shell can render a clean bill of "
                 "health without it");

    // Input validation, which is what the FFI boundary is for: a null buffer is refused, a zero-length
    // read from AudioRecord is a no-op with a real pointer, and neither may ever crash the process.
    NT_EQ(ni_abi_feed_pcm(n, nullptr, 0), NI_ERR_PARAM);
    NT_EQ(ni_abi_pull_pcm(n, nullptr, 0), NI_ERR_PARAM);
    {
      float zero[64] = {0.f};
      NT_EQ(ni_abi_feed_pcm(n, zero, 0), NI_OK);   // "nothing captured this callback" is normal
      NT_EQ(ni_abi_pull_pcm(n, zero, 64) >= 0, true);
    }
    NT_CHECK(ni_abi_pull_pcm_needed(n) >= 0);

    // An unpaired node must refuse application data. The shell shows this refusal; it must never have to
    // guess whether bytes went out.
    const uint8_t body[16] = {0};
    int rc = ni_abi_send_bytes(n, 4242, NI_MSG_FILE, body, (int)sizeof(body), 0);
    NT_CHECK_MSG(rc < 0, "send_bytes on an unpaired node must fail");
    if (rc != NI_ERR_NOPEER) ntest::note("unpaired send returned " + std::to_string(rc) +
                                         " rather than NI_ERR_NOPEER; still a refusal");

    char* rep = ni_abi_report_json(n);
    std::string r = rep ? rep : "";
    ni_abi_free(rep);
    NT_CHECK_MSG(r.find("\"generated\"") != std::string::npos || r.find("{") == 0,
                 "the exportable report must be a JSON document");

    NT_EQ(ni_abi_stop(n), NI_OK);
    NT_EQ(ni_abi_destroy(n), NI_OK);
  }

  // ---- the stateless PHY helpers, which is what "play this file" / "decode this recording" use ----
  {
    const int pid = 3;  // DBPSK-WIDE: round-trips on the model without a peer
    std::string text = "noise internet abi round trip 0123456789";
    std::vector<uint8_t> payload(text.begin(), text.end());
    std::vector<float> tones(48000 * 2, 0.f);
    int n = ni_abi_render(pid, 48000.0, 0.85, 7, &payload[0], (int)payload.size(), &tones[0],
                          (int)tones.size());
    NT_CHECK_MSG(n > 0, "ni_abi_render produced samples");
    if (n > 0) {
      // The shells own the silence around a burst (the modem emits the burst itself), so the test adds
      // the same guard the CLI's --pad option does. Without it the receiver has no window in which to
      // estimate its noise floor before the signal starts.
      std::vector<float> cap((size_t)n + 4096, 0.f);
      memcpy(&cap[2048], &tones[0], sizeof(float) * (size_t)n);
      std::vector<uint8_t> got(4096, 0);
      double snr = -999.0, peak = -999.0;
      int gotn = ni_abi_decode(pid, 48000.0, 7, &cap[0], (int)cap.size(), &got[0], (int)got.size(),
                               &snr, &peak);
      NT_CHECK_MSG(gotn == (int)payload.size(), "decoded length equals the rendered payload");
      if (gotn == (int)payload.size())
        NT_CHECK(memcmp(&got[0], &payload[0], (size_t)gotn) == 0);
      NT_CHECK_MSG(peak > 0.0 && peak <= 1.0001, "reported peak must be a real measurement");
      NT_CHECK_MSG(snr > 0.0, "loopback SNR must be finite and positive for a clean generated burst");
    } else {
      ntest::note("ni_abi_render failed; the decode half of this block did not run");
    }
  }

  // ---- device enumeration: on a build with no backend it must report that, not an empty list ----
  {
    char buf[4096];
    buf[0] = 0;
    int rc = ni_abi_audio_enumerate(buf, (int)sizeof(buf));
    // Convention, and it matters to the shells: this one returns bytes written (positive) or a negative
    // NI_*, and the *content* of the document is what tells a UI whether hardware exists. So the
    // guarantee under test is that the document cannot be read as "enumeration worked, zero devices" -
    // the backend flag and the reason must be in it.
    NT_CHECK_MSG(rc > 0 || rc < 0, "enumeration either writes a document or fails");
    if (rc > 0) {
      std::string j = buf;
      NT_CHECK_MSG(j.find("\"backend\"") != std::string::npos,
                   "the document must state whether an audio backend is compiled in");
      NT_CHECK_MSG(j.find("\"count\"") != std::string::npos && j.find("\"devices\"") != std::string::npos,
                   "and must separate the device count from the device list");
      NT_CHECK_MSG(j.find("\"note\"") != std::string::npos,
                   "and must carry the reason, so the UI shows it instead of guessing");
#if !defined(NI_WITH_WASAPI)
      NT_CHECK_MSG(j.find("\"backend\": false") != std::string::npos,
                   "this build has no backend, and the document must say so rather than imply "
                   "a machine with a sound card that merely has no devices");
#endif
    }
  }

  return ntest::summary("t_abi");
}
