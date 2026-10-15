// t_proxy_auth.cpp - the receiver gateway on its own, over a real loopback socket, with no audio and
// no channel model: the question is what happens to a client that presents a wrong token, a client
// that presents the right one, and the response the right one eventually gets back.
//
// This test exists because defect 27 ("neither GUI can set a proxy token") was fixed by giving both
// front ends a token field, and a field that feeds a config nobody checks is not a fix. It talks to
// the shipped ReceiverGateway through a real TCP socket, so what is proven here is the server's
// behaviour, not the front end's wiring - the field-to-config path is a separate, smaller claim.
//
// It also covers the empty-token case, which is the one the GUIs fall back to: an empty token in the
// options must make the gateway generate one rather than accept anyone (defect 41).
#include <string.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "ni/proxy.h"
#include "ni/session.h"   // MsgType / NI_MSG_* / NI_MFLAG_*
#include "ntest.h"

using namespace ni;

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET tsock;
static void ts_close(tsock s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int tsock;
static void ts_close(tsock s) { close(s); }
#endif

namespace {

// A port nothing else in the suite is on, and the test fails loudly rather than silently skipping if
// the machine refuses to give it to us.
int pick_port() {
  for (int p = 18471; p < 18491; ++p) {
    tsock s = (tsock)::socket(AF_INET, SOCK_STREAM, 0);
    if (s == (tsock)-1) continue;
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)p);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int ok = ::bind(s, (sockaddr*)&a, sizeof a) == 0;
    ts_close(s);
    if (ok) return p;
  }
  return -1;
}

tsock connect_to(int port) {
  tsock s = (tsock)::socket(AF_INET, SOCK_STREAM, 0);
  if (s == (tsock)-1) return s;
  sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(s, (sockaddr*)&a, sizeof a) != 0) {
    ts_close(s);
    return (tsock)-1;
  }
  return s;
}

void send_all(tsock s, const std::string& what) {
  size_t off = 0;
  while (off < what.size()) {
    int n = (int)::send(s, what.data() + off, (int)(what.size() - off), 0);
    if (n <= 0) return;
    off += (size_t)n;
  }
}

// Read until the connection closes, the server has sent something and gone quiet, or the deadline
// passes. The gateway is driven by pump() from this thread, so "quiet" has to be a real timeout.
std::string read_until_quiet(tsock s, int quiet_ms) {
  std::string out;
  char buf[2048];
  for (;;) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    timeval tv;
    tv.tv_sec = quiet_ms / 1000;
    tv.tv_usec = (quiet_ms % 1000) * 1000;
    int r = ::select((int)(s + 1), &fds, nullptr, nullptr, &tv);
    if (r <= 0) return out;
    int n = (int)::recv(s, buf, sizeof buf, 0);
    if (n <= 0) return out;
    out.append(buf, (size_t)n);
  }
}

}  // namespace

int test_refuses_wrong_token_and_serves_the_right_one() {
  int port = pick_port();
  NT_CHECK(port > 0);
  if (port <= 0) return 1;   // no port: the check above already failed loudly

  ni::ReceiverGateway::Options o;
  o.port = port;
  o.token = "correct-horse-battery";   // the token a user would type into the GUI field
  o.bind_addr = "127.0.0.1";
  ni::ReceiverGateway gw(o);
  NT_CHECK(gw.start() == NI_OK);

  // Every stream the gateway wants to send is answered instantly with a canned reply, standing in for
  // the acoustic link. Nothing about authentication depends on the link, so this keeps the test about
  // the one thing it is testing.
  int sent_streams = 0;
  bool last_answer_accepted = false;
  std::string last_request;
  gw.set_sender([&](uint16_t stream, int type, const uint8_t* data, size_t n, uint16_t flags) {
    (void)type;
    (void)flags;
    ++sent_streams;
    last_request.assign((const char*)data, n);
    // The answer travels on the giver's stream id for this request (request + 16384, the offset the
    // node's routing uses), and it has to be a whole HTTP response or the client's state machine is
    // still waiting for a head that never ends.
    std::string body = "NOISE TEST PAYLOAD";
    std::string resp = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                       "\r\n\r\n" + body;
    int rc = gw.on_stream((uint16_t)(stream + 16384), NI_MSG_STREAM, (const uint8_t*)resp.data(),
                          resp.size(), NI_MFLAG_FIRST | NI_MFLAG_LAST);
    // The response is produced synchronously inside send_(), which is the case that used to be dropped
    // as "no client for stream": the id has to be registered before the request goes up. (Defect 62.)
    last_answer_accepted = (rc == NI_OK);
    return NI_OK;
  });

  const std::string req =
      "GET http://127.0.0.1/ HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";

  // 1. no token at all -> 407, the code that tells a client the token is missing.
  {
    tsock c = connect_to(port);
    NT_CHECK(c != (tsock)-1);
    // One pump accepts the connection; pump() is what drives accept on this listener.
    gw.pump(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    gw.pump(1);
    send_all(c, req);
    for (int i = 0; i < 60; ++i) {
      gw.pump(1000 + i);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::string resp = read_until_quiet(c, 400);
    ts_close(c);
    NT_CHECK_MSG(resp.find("407") != std::string::npos, resp);
    NT_CHECK_MSG(resp.find("Proxy-Authenticate: Noise") != std::string::npos, resp);
    NT_CHECK_MSG(resp.find("NOISE TEST PAYLOAD") == std::string::npos,
              "an unauthenticated client must not be answered from the link");
  }

  // 2. a wrong token -> 407 as well, and the request never reaches the acoustic side.
  const int before = sent_streams;
  {
    tsock c = connect_to(port);
    NT_CHECK(c != (tsock)-1);
    gw.pump(2000);
    send_all(c, "GET http://127.0.0.1/ HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                "Proxy-Authorization: Noise wrong-token-entirely\r\n\r\n");
    for (int i = 0; i < 60; ++i) {
      gw.pump(2000 + i);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::string resp = read_until_quiet(c, 400);
    ts_close(c);
    NT_CHECK_MSG(resp.find("407") != std::string::npos, resp);
    NT_CHECK_MSG(sent_streams == before, "a wrong token must not put a request on the acoustic link");
  }

  // 3. the right token -> the request goes out over the link and the canned response comes back
  //    through the gateway to this client. This is the half of the story that the empty-token defect
  //    hid: authentication passing is worth nothing if the answer never arrives.
  {
    tsock c = connect_to(port);
    NT_CHECK(c != (tsock)-1);
    gw.pump(4000);
    send_all(c, "GET http://127.0.0.1/ HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                "Proxy-Authorization: Noise correct-horse-battery\r\n\r\n");
    bool saw_request = false;
    for (int i = 0; i < 200; ++i) {
      gw.pump(4000 + i * 5);
      if (sent_streams > before) saw_request = true;
      if (saw_request) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    NT_CHECK_MSG(saw_request, "the right token must put the request on the acoustic link");
    NT_CHECK_MSG(last_request.find("GET http://127.0.0.1/ HTTP/1.1") != std::string::npos,
                 "the request that crossed the link: " + last_request);
    NT_CHECK_MSG(last_request.find("Proxy-Authorization") == std::string::npos,
                 "the bearer token must not be forwarded to the far side: " + last_request);
    std::string resp;
    for (int i = 0; i < 200; ++i) {
      gw.pump(6000 + i * 5);
      resp += read_until_quiet(c, 20);
      if (resp.find("NOISE TEST PAYLOAD") != std::string::npos) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ts_close(c);
    NT_CHECK_MSG(last_answer_accepted,
                 "the gateway must accept an answer for a stream it has just sent on");
    // And the client must get the far end's response, not its own request echoed back: the echo was the
    // old behaviour, because the request was never consumed out of the read buffer. (Defect 64.)
    NT_CHECK_MSG(resp.find("GET http") == std::string::npos,
                 "the client must not receive its own request back: " + resp);
    NT_CHECK_MSG(resp.find("200") != std::string::npos, resp);
    NT_CHECK_MSG(resp.find("NOISE TEST PAYLOAD") != std::string::npos, resp);
  }

  gw.stop();
  return ntest::summary("t_proxy_auth / wrong and right token");
}

int test_empty_token_generates_one() {
  int port = pick_port();
  NT_CHECK(port > 0);
  if (port <= 0) return 1;   // no port: the check above already failed loudly

  ni::ReceiverGateway::Options o;
  o.port = port;
  o.token = "";                      // what both front ends send when the field is left blank
  o.bind_addr = "127.0.0.1";
  ni::ReceiverGateway gw(o);
  NT_CHECK(gw.start() == NI_OK);
  NT_CHECK_MSG(!gw.token().empty(), "an empty token must become a generated one, not a free pass");
  NT_CHECK_MSG(gw.token().size() >= 8, gw.token());

  gw.set_sender([&](uint16_t, int, const uint8_t*, size_t, uint16_t) { return NI_OK; });

  tsock c = connect_to(port);
  NT_CHECK(c != (tsock)-1);
  gw.pump(0);
  // The generated token is not knowable to this client, so this request stands in for every other
  // program on the machine that found the port open.
  send_all(c, "GET http://127.0.0.1/ HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  for (int i = 0; i < 60; ++i) {
    gw.pump(i);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::string resp = read_until_quiet(c, 400);
  ts_close(c);
  NT_CHECK_MSG(resp.find("407") != std::string::npos, resp);
  gw.stop();
  return ntest::summary("t_proxy_auth / empty token");
}

int main() {
  int bad = 0;
  bad += test_refuses_wrong_token_and_serves_the_right_one();
  bad += test_empty_token_generates_one();
  printf("[t_proxy_auth] %s\n", bad ? "FAILED" : "all gateway-auth checks passed");
  return bad ? 1 : 0;
}
