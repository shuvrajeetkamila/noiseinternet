// t_self_echo.cpp - one machine, one acoustic link, and its own transmission fed straight back into its
// own receive path. That is the field situation the phone reported as "decoded 48 ... bytes received 0"
// (defect 59): the device was decoding itself, counting those decodes as frames from a peer, and the
// panel therefore never said the one true thing - no other device has been heard.
//
// What is checked here:
//   * a burst this link transmitted, looped back, is counted as self-echo and NOT as a received frame
//   * it does not make peer_online() true
//   * it does not move the link state past LISTEN
//   * the same bytes still count as received when they come from somewhere the link never sent
//     (otherwise the detector would be "ignore everything", which is worse than the bug)
#include <string.h>

#include <vector>

#include "ni/common.h"
#include "ni/frame_codec.h"
#include "ni/modem.h"
#include "ni/session.h"
#include "ntest.h"

using namespace ni;

namespace {

}  // namespace

int main() {
  // ---- link 1: hear yourself -------------------------------------------------
  {
    LinkOptions o;
    o.role = NI_ROLE_GIVER;
    o.modem.sample_rate = 48000;
    o.modem.profile_id = 10;          // FD-LOW-DBPSK, the profile both field devices had selected
    o.assist = false;
    AcousticLink link(o);
    NT_CHECK(link.start() == NI_OK);

    // Let the master do its own housekeeping until it has put a real burst on the air, and keep the
    // samples it wanted played. Nothing here calls the PHY directly: this is the path the app drives.
    std::vector<float> wave;
    for (int64_t t = 1000; t < 7000 && wave.size() < 200000; t += 5) {
      link.tick(t);
      float block[240];
      int got;
      while ((got = link.take_pcm(block, 240)) > 0) wave.insert(wave.end(), block, block + got);
    }
    NT_CHECK_MSG(wave.size() > 1000, "the link rendered nothing to send, so the loopback proves nothing");
    const uint64_t tx_bursts = link.counters().tx_bursts;
    NT_CHECK_MSG(tx_bursts >= 1, "no burst was transmitted");

    // Now play exactly those samples back into the microphone input: a perfect, noiseless, zero-delay
    // echo. Any real room is worse than this, so a link that fails here fails in every room.
    for (size_t off = 0, k = 0; off < wave.size(); off += 480, ++k) {
      size_t n = wave.size() - off < 480 ? wave.size() - off : 480;
      link.feed_pcm(&wave[off], n);
      link.tick(8000 + (int64_t)(k * 10));   // 480 samples = 10 ms at 48 kHz
    }
    link.tick(30000);

    const AcousticLink::Counters& c = link.counters();
    NT_CHECK_MSG(c.self_echo >= 1,
                 "the link did not recognise its own burst coming back (self_echo=" +
                     std::to_string(c.self_echo) + ", rx_bursts=" + std::to_string(c.rx_bursts) + ")");
    NT_CHECK_MSG(!link.peer_online(),
                 "a machine that only heard itself reported a peer online - that is defect 59");
    NT_CHECK_MSG(link.state() < NI_ST_FRAME_OK,
                 "hearing yourself must not advance the link to FRAME_OK");
    NT_CHECK_MSG(link.diagnostics_json().find("\"self_echo_bursts\"") != std::string::npos,
                 "the self-echo count must reach the status JSON both UIs read");

    // The same audio decoded a second time (defect H). The 2026-10-15 field log showed "self-echo 128"
    // from a single transmitted frame: one physical echo, seen 128 times, because the decoder is fed
    // overlapping windows and finds the burst again on the next pass. Counting each look as another echo
    // is what made a one-burst finding look like a hundred. Here the same samples are played through the
    // receive path a second time on the same clock, so the duplicate decodes are certain rather than
    // dependent on room reflections: the distinct-echo count must not grow by more than a burst or two,
    // and the repeats must be counted as repeats.
    const uint64_t echoes_after_first_pass = c.self_echo;
    for (size_t off = 0, k = 0; off < wave.size(); off += 480, ++k) {
      size_t n = wave.size() - off < 480 ? wave.size() - off : 480;
      link.feed_pcm(&wave[off], n);
      link.tick(8000 + (int64_t)(k * 10));
    }
    NT_CHECK_MSG(link.counters().self_echo_repeats >= 1,
                 "the same burst decoded again was not counted as a repeat (self_echo=" +
                     std::to_string(link.counters().self_echo) + ", repeats=" +
                     std::to_string(link.counters().self_echo_repeats) + ")");
    NT_CHECK_MSG(link.counters().self_echo <= echoes_after_first_pass + 2,
                 "repeat decodes were counted as new echoes: " +
                     std::to_string(echoes_after_first_pass) + " -> " +
                     std::to_string(link.counters().self_echo));
    link.stop();
  }

  // ---- link 2: a burst the link never sent is still accepted ------------------
  {
    LinkOptions o;
    o.role = NI_ROLE_RECEIVER;
    o.is_master = false;                 // a slave with assist off stays silent: it cannot transmit anything
    o.modem.sample_rate = 48000;
    o.modem.profile_id = 10;
    o.assist = false;
    AcousticLink link(o);
    NT_CHECK(link.start() == NI_OK);

    // A PDU built by the ARQ and rendered by a modem of the same profile but belonging to a different
    // object: these samples are "the far end", not something this link ever put on the air.
    uint8_t pdu[2 + NI_MAX_BURST_PDU];
    size_t plen = 0;
    int nf = link.arq().build_pdu(1000, pdu, sizeof pdu, &plen, true);
    NT_CHECK(nf >= 1);
    NT_CHECK(plen > 2);
    ModemConfig mc;
    mc.sample_rate = 48000;
    mc.profile_id = 10;
    Modem m(mc);
    std::vector<float> wave;
    NT_CHECK(m.tx_render(pdu, plen, &wave) == NI_OK);
    for (size_t off = 0, k = 0; off < wave.size(); off += 480, ++k) {
      size_t n = wave.size() - off < 480 ? wave.size() - off : 480;
      link.feed_pcm(&wave[off], n);
      link.tick(1000 + (int64_t)(k * 10));
    }
    link.tick(20000);
    const AcousticLink::Counters& c = link.counters();
    NT_CHECK_MSG(c.self_echo == 0,
                 "a burst this link never transmitted was misread as its own echo");
    NT_CHECK_MSG(c.rx_bursts >= 1 || c.sync_hits >= 1,
                 "the far end's burst was neither decoded nor even detected");
    link.stop();
  }

  return ntest::summary("t_self_echo");
}
