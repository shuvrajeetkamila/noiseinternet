// proxy.h — application-layer gateway: local HTTP/CONNECT proxy on the receiver, real
// fetching on the giver, both carried by the message layer of the acoustic link.
//
// Security properties enforced here (prompt 7.1 / 8):
//   * the listener binds 127.0.0.1 only, never 0.0.0.0
//   * a per-session bearer token must be presented (Proxy-Authorization: Noise <token>)
//   * bounded per-stream buffers, bounded total in-flight memory, hard request timeout
//   * malformed/oversized requests are rejected with a status, and repeated abuse trips the
//     rate limiter and disables the listener
//   * CONNECT tunnels are byte relays: TLS is NOT intercepted, certificate validation stays
//     with the application
#ifndef NI_PROXY_H
#define NI_PROXY_H
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "ni/common.h"

namespace ni {

enum : int {
  NI_PROXY_MAX_REQ = 32768,        // request head bytes
  NI_PROXY_MAX_HDR_LINE = 4096,
  NI_PROXY_MAX_BODY = 1u << 20,     // 1 MiB POST body
  NI_PROXY_STREAM_CAP = 8u << 20,   // 8 MiB buffered per response stream
  NI_PROXY_TOTAL_CAP = 24u << 20,   // 24 MiB in flight
  NI_PROXY_MAX_STREAMS = 12,
  NI_PROXY_TIMEOUT_MS = 45000,
  NI_PROXY_MAX_ABUSE = 8,
};

struct ProxyRequest {
  std::string method, target, version;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  bool valid = false;
  std::string error;
  std::string header(int pad = 0) const;  // canonical text form for forwarding
  int header_len() const;
  static bool parse_head(const std::string& text, ProxyRequest* out, size_t* consumed);
};

struct ProxyResponse {
  int status = 0;
  std::string reason, body;
  std::vector<std::pair<std::string, std::string>> headers;
  bool chunked = false;
  std::string to_text() const;
};

// ---------------- receiver side: local listener + stream mapper ----------------
class LinkSender;  // fwd
class ReceiverGateway {
 public:
  struct Options {
    std::string bind_addr = "127.0.0.1";
    int port = 8471;
    std::string token;          // bearer token; empty => randomly generated at start
    bool allow_http = true;
    bool allow_connect = true;
    int max_conns = 8;
  };
  explicit ReceiverGateway(const Options& o);
  ~ReceiverGateway();
  int start();  // returns NI_OK / NI_ERR_IO when the port is taken
  void stop();
  const std::string& token() const { return tok_; }
  std::string describe() const;
  // Called by the node when a message arrives for a stream this gateway owns.
  int on_stream(uint16_t stream, int type, const uint8_t* data, size_t n, uint16_t flags);
  // Wire to the link: called when a local client sent something that must go over the air.
  void set_sender(std::function<int(uint16_t, int, const uint8_t*, size_t, uint16_t)> f) {
    send_ = f;
  }
  int pump(int64_t now_ms);  // timers, accept, per-client state machines
  struct ClientState {
    int fd = -1;
    uint16_t stream = 0;
    std::string rxbuf;
    std::string txbuf;
    int phase = 0;  // 0 read head,1 body,2 awaiting response,3 relay,4 closing
    bool connect_mode = false;
    std::string body;
    size_t body_so_far = 0;
    uint64_t content_left = 0;
    bool have_content_len = false;
    bool chunked = false;
    int64_t deadline_ms = 0;
    size_t expect_body = 0;
    ProxyRequest req;
    uint64_t sent_bytes = 0, recv_bytes = 0;
  };
  int streams_open() const;
  uint64_t requests_ok = 0, requests_fail = 0, abuse = 0, bytes_relayed = 0;

 private:
  Options opt_;
  std::string tok_;
  int listen_fd_ = -1;
  bool disabled_ = false;
  uint16_t next_stream_ = 1;
  std::map<uint16_t, ClientState> clients_;
  std::map<int, uint16_t> fd_to_stream_;
  std::function<int(uint16_t, int, const uint8_t*, size_t, uint16_t)> send_;
  int reply_error(ClientState& c, int code, const std::string& why);
  int handle_readable(ClientState& c, int64_t now_ms);
};

// ---------------- giver side: performs the real network work ----------------
class GiverGateway {
 public:
  struct Options {
    bool allow_http = true;
    bool allow_connect = true;
    int connect_timeout_ms = 8000;
    int io_timeout_ms = 30000;
    size_t max_response = 4u << 20;  // 4 MiB hard cap on a fetched object
    std::string user_agent = "NoiseInternet/1.0 (acoustic gateway)";
    bool verify_internet_probe = true;
  };
  explicit GiverGateway(const Options& o);
  ~GiverGateway();
  void set_sender(std::function<int(uint16_t, int, const uint8_t*, size_t, uint16_t)> f) {
    send_ = f;
  }
  int on_stream(uint16_t stream, int type, const uint8_t* data, size_t n, uint16_t flags);
  int pump(int64_t now_ms);
  // Direct, in-process helpers used by the connectivity verifier and the tests.
  static int resolve(const std::string& host, std::string* ip);
  static int http_fetch(const std::string& url, size_t max_bytes, ProxyResponse* out,
                        std::string* err);
  std::string describe() const;
  uint64_t fetches_ok = 0, fetches_fail = 0, connect_ok = 0, connect_fail = 0;

 private:
  struct Stream {
    uint16_t id = 0;
    std::string reqbuf;
    bool head_done = false;
    ProxyRequest req;
    int sock = -1;              // upstream socket for both GET and CONNECT
    size_t body_off = 0;
    bool want_connect = false;
    bool sent_req = false;
    bool connected = false;
    int64_t connected_at_ms = 0;
    std::string outbuf;         // bytes to send back over the air
    size_t sent_off = 0;
    bool tunnel_open = false;
    bool done = false;
    int64_t start_ms = 0;
    size_t total_read = 0;
  };
  Options opt_;
  int64_t now_ms_ = 0;
  bool allow_http_ = true;
  std::map<uint16_t, std::unique_ptr<Stream>> streams_;
  std::function<int(uint16_t, int, const uint8_t*, size_t, uint16_t)> send_;
  int step(Stream& s, int64_t now_ms);
  int push(Stream& s, const uint8_t* p, size_t n);
};

// ---------------- end-to-end verifier (prompt 7.4) ---------------------------
// Runs a real external request through the receiver's gateway and compares against a direct
// fetch by the giver. Every stage is recorded separately so nothing is inferred.
struct InternetVerify {
  struct Stage {
    std::string name;
    bool ok = false;
    std::string detail;
    double ms = 0.0;
  };
  std::vector<Stage> stages;
  bool ok = false;
  std::string url;
  uint32_t expect_crc = 0, got_crc = 0;
  std::string expect_sha256, got_sha256;
  size_t bytes = 0;
  double total_ms = 0.0;
  std::string to_json() const;
  void stage(const std::string& n, bool ok, const std::string& detail, double ms);
};
int ni_verify_internet(const std::string& url, ReceiverGateway& rg, int64_t now_ms_base,
                       InternetVerify* out);

}  // namespace ni
#endif
