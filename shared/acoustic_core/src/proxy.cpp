// proxy.cpp — the application-layer gateway: local HTTP/CONNECT proxy on the receiver,
// real fetching on the giver, plus the end-to-end internet verifier.
#include "ni/proxy.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <cctype>
#include <sstream>
#include "ni/crc.h"
#include "ni/crypto.h"
#include "ni/session.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define NI_SOCKERR() (WSAGetLastError())
#define NI_EWOULDBLOCK WSAEWOULDBLOCK
#define NI_EINPROGRESS WSAEINPROGRESS
inline void ni_closesock(sock_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int sock_t;
#define NI_SOCKERR() errno
#define NI_EWOULDBLOCK EAGAIN
#define NI_EINPROGRESS EINPROGRESS
inline void ni_closesock(sock_t s) { close(s); }
#endif

namespace ni {

// ------------------------------------------------------------------ helpers
namespace {
bool g_wsa_ready = false;
void sock_init() {
#ifdef _WIN32
  if (!g_wsa_ready) {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
    g_wsa_ready = true;
  }
#endif
}
void set_nonblock(sock_t s) {
#ifdef _WIN32
  u_long v = 1;
  ioctlsocket(s, FIONBIO, &v);
#else
  int f = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, f | O_NONBLOCK);
#endif
}
std::string lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}
bool iequals(const std::string& a, const std::string& b) {
  return a.size() == b.size() && lower(a) == lower(b);
}
}  // namespace

// ------------------------------------------------------------------ request parsing
bool ProxyRequest::parse_head(const std::string& text, ProxyRequest* out, size_t* consumed) {
  size_t eom = text.find("\r\n\r\n");
  if (eom == std::string::npos) {
    if (text.size() > NI_PROXY_MAX_REQ) {
      out->error = "request head too large";
      return false;
    }
    return false;  // incomplete
  }
  if (eom > NI_PROXY_MAX_REQ) {
    out->error = "request head too large";
    return false;
  }
  std::string head = text.substr(0, eom);
  *consumed = eom + 4;
  std::istringstream is(head);
  std::string line;
  if (!std::getline(is, line)) {
    out->error = "no request line";
    return false;
  }
  if (!line.empty() && line.back() == '\r') line.pop_back();
  {
    std::istringstream ls(line);
    if (!(ls >> out->method >> out->target >> out->version)) {
      out->error = "malformed request line";
      return false;
    }
  }
  if (out->method.empty() || out->target.empty() || out->version.rfind("HTTP/", 0) != 0) {
    out->error = "malformed request line";
    return false;
  }
  while (std::getline(is, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    size_t c = line.find(':');
    if (c == std::string::npos) {
      out->error = "malformed header";
      return false;
    }
    std::string k = lower(line.substr(0, c));
    std::string v = line.substr(c + 1);
    size_t a = v.find_first_not_of(" \t");
    v = a == std::string::npos ? "" : v.substr(a);
    if (k.size() > 200 || v.size() > NI_PROXY_MAX_HDR_LINE) {
      out->error = "header too long";
      return false;
    }
    out->headers.emplace_back(k, v);
  }
  out->valid = true;
  out->error.clear();
  return true;
}

int ProxyRequest::header_len() const { return (int)header().size(); }

std::string ProxyRequest::header(int pad) const {
  std::ostringstream o;
  o << method << " " << target << " " << version << "\r\n";
  for (auto& kv : headers) o << kv.first << ": " << kv.second << "\r\n";
  for (int i = 0; i < pad; i++) o << "X-Pad-" << i << ": 0\r\n";
  o << "\r\n";
  return o.str();
}

std::string ProxyResponse::to_text() const {
  std::ostringstream o;
  o << "HTTP/1.1 " << status << " " << reason << "\r\n";
  bool have_len = false;
  for (auto& kv : headers) {
    o << kv.first << ": " << kv.second << "\r\n";
    if (iequals(kv.first, "content-length")) have_len = true;
  }
  if (!have_len && !chunked) o << "Content-Length: " << body.size() << "\r\n";
  o << "X-Noise-Internet: acoustic-link\r\n\r\n";
  return o.str();
}

// ------------------------------------------------------------------ ReceiverGateway
ReceiverGateway::ReceiverGateway(const Options& o) : opt_(o) {
  sock_init();
  if (opt_.token.empty()) {
    Rng r((uint64_t)time(nullptr) ^ 0x9aa3ull);
    uint8_t b[8];
    r.bytes(b, 8);
    opt_.token = ni_hex(b, 8);
  }
  tok_ = opt_.token;
}

ReceiverGateway::~ReceiverGateway() { stop(); }

std::string ReceiverGateway::describe() const {
  std::ostringstream o;
  o << opt_.bind_addr << ":" << opt_.port << " token=" << tok_.substr(0, 4) << "****"
    << (disabled_ ? " [DISABLED by rate limit]" : "") << " streams=" << streams_open();
  return o.str();
}

int ReceiverGateway::streams_open() const { return (int)clients_.size(); }

int ReceiverGateway::start() {
  sock_init();
  listen_fd_ = (int)::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return NI_ERR_IO;
  int one = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
  sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)opt_.port);
  a.sin_addr.s_addr = inet_addr(opt_.bind_addr.c_str());
  if (::bind(listen_fd_, (sockaddr*)&a, sizeof a) != 0) {
    // never fall back to a wider bind address: refusing to expose the proxy is the point
    ni_closesock(listen_fd_);
    listen_fd_ = -1;
    return NI_ERR_IO;
  }
  if (::listen(listen_fd_, 8) != 0) {
    ni_closesock(listen_fd_);
    listen_fd_ = -1;
    return NI_ERR_IO;
  }
  set_nonblock(listen_fd_);
  return NI_OK;
}

void ReceiverGateway::stop() {
  for (auto& kv : clients_)
    if (kv.second.fd >= 0) ni_closesock(kv.second.fd);
  clients_.clear();
  fd_to_stream_.clear();
  if (listen_fd_ >= 0) {
    ni_closesock(listen_fd_);
    listen_fd_ = -1;
  }
}

int ReceiverGateway::reply_error(ClientState& c, int code, const std::string& why) {
  std::ostringstream o;
  o << "HTTP/1.1 " << code << " " << why << "\r\n"
    << "Content-Type: text/plain\r\nProxy-Agent: NoiseInternet/1.0\r\n\r\n"
    << "NOISE INTERNET local proxy: " << why << "\r\n";
  if (code == 407) o << "Proxy-Authenticate: Noise realm=\"noise-internet\"\r\n";
  c.txbuf += o.str();
  c.phase = 4;
  requests_fail++;
  return NI_OK;
}

int ReceiverGateway::handle_readable(ClientState& c, int64_t now_ms) {
  char buf[4096];
  for (;;) {
    int r = (int)::recv(c.fd, buf, sizeof buf, 0);
    if (r > 0) {
      if (c.phase <= 1) {
        c.rxbuf.append(buf, (size_t)r);
        if (c.rxbuf.size() > NI_PROXY_MAX_REQ + NI_PROXY_MAX_BODY) {
          abuse++;
          reply_error(c, 413, "request exceeds the configured limit");
          if (abuse > NI_PROXY_MAX_ABUSE) disabled_ = true;
          return NI_OK;
        }
        size_t used = 0;
        ProxyRequest req;
        if (!ProxyRequest::parse_head(c.rxbuf, &req, &used)) {
          if (!req.error.empty()) {
            abuse++;
            reply_error(c, 400, req.error);
            if (abuse > NI_PROXY_MAX_ABUSE) disabled_ = true;
            return NI_OK;
          }
          return NI_OK;  // wait for more
        }
        c.req = req;
        size_t clen = 0;
        for (auto& kv : c.req.headers)
          if (iequals(kv.first, "content-length")) clen = (size_t)strtoul(kv.second.c_str(), nullptr, 10);
        if (clen > NI_PROXY_MAX_BODY) {
          reply_error(c, 413, "body too large for the acoustic link");
          return NI_OK;
        }
        c.expect_body = clen;
        c.body_so_far = c.rxbuf.size() - used;
        if (c.body_so_far < clen) {
          c.phase = 1;
          c.rxbuf = c.rxbuf.substr(used);
          return NI_OK;
        }
        c.body = c.rxbuf.substr(used, clen);
        c.phase = 2;
        // authentication: the bearer token must be present
        bool authed = opt_.token.empty();
        for (auto& kv : c.req.headers) {
          if (iequals(kv.first, "proxy-authorization")) {
            std::string v = kv.second;
            if (v.rfind("Noise ", 0) == 0) v = v.substr(6);
            authed = (v == opt_.token);
          }
        }
        if (!authed) {
          reply_error(c, 407, "missing or wrong proxy token");
          return NI_OK;
        }
        bool is_connect = iequals(c.req.method, "CONNECT");
        if (is_connect && !opt_.allow_connect) {
          reply_error(c, 405, "CONNECT is disabled on this receiver");
          return NI_OK;
        }
        if (!is_connect && !opt_.allow_http) {
          reply_error(c, 405, "plain HTTP is disabled on this receiver");
          return NI_OK;
        }
        c.connect_mode = is_connect;
        c.stream = next_stream_++;
        if (next_stream_ >= NI_PROXY_MAX_STREAMS * 2) next_stream_ = 1;
        fd_to_stream_[c.fd] = c.stream;
        std::string head = c.req.header();
        std::vector<uint8_t> blob;
        blob.insert(blob.end(), head.begin(), head.end());
        blob.insert(blob.end(), c.body.begin(), c.body.end());
        if (!send_) {
          reply_error(c, 503, "no acoustic session: nothing to forward through");
          return NI_OK;
        }
        int rc = send_(c.stream, NI_MSG_STREAM, blob.data(), blob.size(),
                       NI_MFLAG_FIRST | NI_MFLAG_LAST);
        if (rc != NI_OK) {
          reply_error(c, 503, std::string("link refused the request: ") + ni_strerror(rc));
          return NI_OK;
        }
        c.sent_bytes += blob.size();
        c.phase = c.connect_mode ? 3 : 2;
        c.deadline_ms = now_ms + NI_PROXY_TIMEOUT_MS;
        clients_[c.stream] = c;
        return NI_OK;
      }
      if (c.phase == 3) {  // tunnel: relay client bytes upward
        if (!send_) return NI_OK;
        send_(c.stream, NI_MSG_STREAM, (const uint8_t*)buf, (size_t)r, 0);
        c.sent_bytes += (size_t)r;
      }
      continue;
    }
    if (r == 0) {
      if (c.phase == 3 && send_) send_(c.stream, NI_MSG_CLOSE, nullptr, 0, NI_MFLAG_LAST);
      return NI_ERR_IO;  // client gone
    }
    int e = NI_SOCKERR();
    if (e == NI_EWOULDBLOCK) return NI_OK;
    return NI_ERR_IO;
  }
}

int ReceiverGateway::on_stream(uint16_t stream, int type, const uint8_t* data, size_t n,
                               uint16_t flags) {
  auto it = clients_.find(stream);
  if (it == clients_.end()) return NI_ERR_PARAM;  // response for a stream we already dropped
  ClientState& c = it->second;
  if (type == NI_MSG_ABORT) {
    reply_error(c, 502, "giver reported a failure: " + std::string((const char*)data, ni_min(n, (size_t)200)));
    return NI_OK;
  }
  if (c.phase == 2) {  // waiting for the response head
    c.rxbuf.append((const char*)data, n);
    size_t h = c.rxbuf.find("\r\n\r\n");
    if (h != std::string::npos) {
      std::string head = c.rxbuf.substr(0, h + 4);
      size_t body_have = c.rxbuf.size() - (h + 4);
      if (head.rfind("HTTP/1.1 200 Connection Established", 0) == 0 || c.connect_mode) {
        // tunnel established: everything after the head is relayed raw
        std::string rest = c.rxbuf.substr(h + 4);
        c.txbuf += head;
        c.txbuf += rest;
        c.rxbuf.clear();
        c.phase = 3;
      } else {
        c.txbuf += head;
        c.rxbuf = c.rxbuf.substr(h + 4);
        c.phase = 3;  // body follows as opaque bytes
        c.content_left = 0;
        c.have_content_len = false;
        for (size_t i = 0; i < head.size();) {
          size_t e = head.find("\r\n", i);
          if (e == std::string::npos) break;
          std::string line = head.substr(i, e - i);
          size_t col = line.find(':');
          if (col != std::string::npos) {
            std::string k = lower(line.substr(0, col)), v = line.substr(col + 1);
            while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(v.begin());
            if (k == "content-length") {
              c.content_left = strtoull(v.c_str(), nullptr, 10);
              c.have_content_len = true;
            } else if (k == "transfer-encoding" && lower(v).find("chunked") != std::string::npos) {
              c.chunked = true;
            }
          }
          i = e + 2;
        }
        (void)body_have;
      }
    }
    return NI_OK;
  }
  c.txbuf.append((const char*)data, n);
  if (c.have_content_len) c.content_left = c.content_left > n ? c.content_left - n : 0;
  if ((flags & NI_MFLAG_LAST) || (c.have_content_len && c.content_left == 0) ||
      type == NI_MSG_CLOSE) {
    c.phase = 4;
    requests_ok++;
  }
  c.recv_bytes += n;
  bytes_relayed += n;
  return NI_OK;
}

int ReceiverGateway::pump(int64_t now_ms) {
  if (listen_fd_ < 0 || disabled_) return NI_OK;
  for (;;) {
    sockaddr_in pa;
    socklen_t pl = sizeof pa;
    sock_t fd = ::accept(listen_fd_, (sockaddr*)&pa, &pl);
    if (fd == (sock_t)-1 || fd < 0) break;
    if ((int)clients_.size() >= opt_.max_conns) {
      const char* msg = "HTTP/1.1 503 Too many concurrent streams\r\n\r\n";
      ::send(fd, msg, strlen(msg), 0);
      ni_closesock(fd);
      continue;
    }
    set_nonblock(fd);
    ClientState c;
    c.fd = (int)fd;
    c.deadline_ms = now_ms + 20000;
    uint16_t sid = (uint16_t)(60000 + clients_.size());
    clients_[sid] = c;
    fd_to_stream_[(int)fd] = sid;
  }
  std::vector<uint16_t> dead;
  for (auto& kv : clients_) {
    ClientState& c = kv.second;
    handle_readable(c, now_ms);
    if (!c.txbuf.empty() && c.fd >= 0) {
      int s = (int)::send(c.fd, c.txbuf.data(), c.txbuf.size(), 0);
      if (s > 0) c.txbuf.erase(0, (size_t)s);
    }
    if (c.phase == 4) {
      if (c.txbuf.empty()) dead.push_back(kv.first);
    }
    if (now_ms > c.deadline_ms + NI_PROXY_TIMEOUT_MS) {
      if (c.phase < 4) reply_error(c, 504, "acoustic link timed out");
      c.phase = 4;
    }
  }
  for (uint16_t s : dead) {
    auto it = clients_.find(s);
    if (it == clients_.end()) continue;
    if (it->second.fd >= 0) {
      fd_to_stream_.erase(it->second.fd);
      ni_closesock(it->second.fd);
    }
    clients_.erase(it);
  }
  return NI_OK;
}

// ------------------------------------------------------------------ GiverGateway
GiverGateway::GiverGateway(const Options& o) : opt_(o), allow_http_(o.allow_http) {
  sock_init();
}
GiverGateway::~GiverGateway() {
  for (auto& kv : streams_)
    if (kv.second->sock >= 0) ni_closesock(kv.second->sock);
  streams_.clear();
}

int GiverGateway::resolve(const std::string& host, std::string* ip) {
  addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* r = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &r) != 0 || !r) return NI_ERR_IO;
  char buf[64] = {0};
  if (r->ai_family == AF_INET) {
    sockaddr_in* sa = (sockaddr_in*)r->ai_addr;
    inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof buf);
  }
  freeaddrinfo(r);
  *ip = buf;
  return ip->empty() ? NI_ERR_IO : NI_OK;
}

int GiverGateway::on_stream(uint16_t stream, int type, const uint8_t* data, size_t n,
                            uint16_t flags) {
  auto it = streams_.find(stream);
  if (type == NI_MSG_CLOSE || type == NI_MSG_ABORT) {
    if (it != streams_.end()) {
      if (it->second->sock >= 0) ni_closesock(it->second->sock);
      streams_.erase(it);
    }
    return NI_OK;
  }
  if (it == streams_.end()) {
    if ((flags & NI_MFLAG_FIRST) == 0) return NI_ERR_PARAM;  // never saw the open
    if (streams_.size() >= NI_PROXY_MAX_STREAMS) return NI_ERR_OVERFLOW;
    std::unique_ptr<Stream> s(new Stream());
    s->id = stream;
    it = streams_.emplace(stream, std::move(s)).first;
  }
  Stream& s = *it->second;
  s.reqbuf.append((const char*)data, n);
  if (s.tunnel_open) {
    if (s.sock >= 0) ::send(s.sock, (const char*)data, n, 0);
    return NI_OK;
  }
  if (s.reqbuf.size() > NI_PROXY_MAX_REQ + NI_PROXY_MAX_BODY) {
    if (send_) {
      std::string why = "request exceeds the giver limit";
      send_(stream + 16384, NI_MSG_ABORT, (const uint8_t*)why.data(), why.size(), NI_MFLAG_LAST);
    }
    streams_.erase(it);
    return NI_ERR_OVERFLOW;
  }
  if (flags & NI_MFLAG_LAST) s.head_done = true;
  return NI_OK;
}

namespace {
struct Url {
  std::string scheme, host, path;
  int port = 80;
  bool tls = false;
};
bool parse_url(const std::string& u, Url* o) {
  if (u.rfind("http://", 0) == 0) {
    o->scheme = "http";
    o->tls = false;
    o->port = 80;
  } else if (u.rfind("https://", 0) == 0) {
    o->scheme = "https";
    o->tls = true;
    o->port = 443;
  } else
    return false;
  size_t p = u.find("://") + 3;
  size_t q = u.find('/', p);
  std::string hp = q == std::string::npos ? u.substr(p) : u.substr(p, q - p);
  o->path = q == std::string::npos ? "/" : u.substr(q);
  size_t c = hp.rfind(':');
  if (c != std::string::npos && c > hp.rfind(']')) {
    o->port = atoi(hp.c_str() + c + 1);
    hp = hp.substr(0, c);
  }
  o->host = hp;
  return !o->host.empty();
}
}  // namespace

int GiverGateway::push(Stream& s, const uint8_t* p, size_t n) {
  if (!send_) return NI_ERR_STATE;
  size_t off = 0;
  while (off < n || (off == 0 && n == 0)) {
    size_t take = ni_min((size_t)16384, n - off);
    bool last = (off + take >= n);
    int rc = send_(s.id + 16384, NI_MSG_STREAM, p + off, take,
                   (off == 0 ? NI_MFLAG_FIRST : 0) | (last ? NI_MFLAG_LAST : 0));
    if (rc != NI_OK) return rc;
    off += take;
    if (n == 0) break;
  }
  return NI_OK;
}

int GiverGateway::step(Stream& s, int64_t now_ms) {
  if (!s.head_done) return 0;
  if (s.sock < 0) {
    ProxyRequest req;
    size_t used = 0;
    if (!ProxyRequest::parse_head(s.reqbuf, &req, &used)) {
      if (now_ms - s.start_ms > 5000) {
        std::string why = "incomplete request head";
        send_(s.id + 16384, NI_MSG_ABORT, (const uint8_t*)why.data(), why.size(), NI_MFLAG_LAST);
        return -1;
      }
      return 0;
    }
    s.req = req;
    s.body_off = used;
    std::string host = req.headers.empty() ? "" : "";
    int port = 80;
    bool is_connect = iequals(req.method, "CONNECT");
    // host:port from either the CONNECT target or the absolute URL / Host header
    std::string target = req.target;
    if (is_connect) {
      size_t c = target.rfind(':');
      host = c == std::string::npos ? target : target.substr(0, c);
      port = c == std::string::npos ? 443 : atoi(target.c_str() + c + 1);
    } else {
      Url u;
      if (parse_url(target, &u)) {
        host = u.host;
        port = u.port;
        if (u.tls) {
          std::string why =
              "https:// URLs need a CONNECT tunnel; plain GET over the acoustic link is http only";
          send_(s.id + 16384, NI_MSG_ABORT, (const uint8_t*)why.data(), why.size(), NI_MFLAG_LAST);
          return -1;
        }
        req.target = u.path;  // origin-form for the upstream server
      } else {
        for (auto& kv : req.headers)
          if (iequals(kv.first, "host")) {
            size_t c = kv.second.rfind(':');
            host = c == std::string::npos ? kv.second : kv.second.substr(0, c);
            port = c == std::string::npos ? 80 : atoi(kv.second.c_str() + c + 1);
          }
        if (host.empty()) {
          std::string why = "no host in request";
          send_(s.id + 16384, NI_MSG_ABORT, (const uint8_t*)why.data(), why.size(), NI_MFLAG_LAST);
          return -1;
        }
      }
    }
    if (!is_connect && !allow_http_) {
      std::string why = "plain HTTP disabled on the giver";
      send_(s.id + 16384, NI_MSG_ABORT, (const uint8_t*)why.data(), why.size(), NI_MFLAG_LAST);
      return -1;
    }
    std::string ip;
    if (resolve(host, &ip) != NI_OK) {
      fetches_fail++;
      std::string why = "DNS failed for " + host;
      send_(s.id + 16384, NI_MSG_ABORT, (const uint8_t*)why.data(), why.size(), NI_MFLAG_LAST);
      return -1;
    }
    s.sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s.sock < 0) {
      fetches_fail++;
      return -1;
    }
    sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, ip.c_str(), &sa.sin_addr);
    set_nonblock(s.sock);
    int r = ::connect(s.sock, (sockaddr*)&sa, sizeof sa);
    if (r != 0 && NI_SOCKERR() != NI_EINPROGRESS) {
      fetches_fail++;
      std::string why = "connect failed to " + host;
      send_(s.id + 16384, NI_MSG_ABORT, (const uint8_t*)why.data(), why.size(), NI_MFLAG_LAST);
      ni_closesock(s.sock);
      s.sock = -1;
      return -1;
    }
    s.connected_at_ms = now_ms;
    s.want_connect = is_connect;
    return 0;
  }
  // connected (or connecting): send the request, then pump the socket
  if (!s.sent_req) {
    std::string out;
    if (s.req.method.size()) {
      std::ostringstream o;
      o << s.req.method << " " << s.req.target << " " << s.req.version << "\r\n";
      for (auto& kv : s.req.headers) {
        if (iequals(kv.first, "proxy-authorization") || iequals(kv.first, "proxy-connection"))
          continue;
        o << kv.first << ": " << kv.second << "\r\n";
      }
      o << "Connection: close\r\n"
        << "User-Agent: " << opt_.user_agent << "\r\n\r\n";
      out = o.str();
      if (s.reqbuf.size() > s.body_off) out += s.reqbuf.substr(s.body_off);
    }
    if (!out.empty()) {
      int n = (int)::send(s.sock, out.data(), out.size(), 0);
      if (n <= 0) return 0;
    }
    s.sent_req = true;
    if (s.want_connect) {
      // do not read yet: the TLS client speaks first; just start relaying
      s.tunnel_open = true;
      const char* ok = "HTTP/1.1 200 Connection Established\r\n\r\n";
      push(s, (const uint8_t*)ok, strlen(ok));
      connect_ok++;
      s.outbuf.clear();
    }
  }
  if (s.done) return -1;
  for (;;) {
    char buf[4096];
    int r = (int)::recv(s.sock, buf, sizeof buf, 0);
    if (r > 0) {
      s.total_read += (size_t)r;
      if (s.total_read > opt_.max_response) {
        fetches_fail++;
        std::string why = "response exceeded the giver cap";
        push(s, (const uint8_t*)"\r\n", 2);
        send_(s.id + 16384, NI_MSG_ABORT, (const uint8_t*)why.data(), why.size(), NI_MFLAG_LAST);
        s.done = true;
        return -1;
      }
      if (s.tunnel_open || push(s, (const uint8_t*)buf, (size_t)r) != NI_OK) {
        if (!s.tunnel_open) {
          fetches_fail++;
          // link cannot carry it: abort rather than silently truncating
          std::string why = "acoustic link buffer full";
          send_(s.id + 16384, NI_MSG_ABORT, (const uint8_t*)why.data(), why.size(), NI_MFLAG_LAST);
          s.done = true;
          return -1;
        }
      }
      continue;
    }
    if (r == 0) {
      s.done = true;
      if (!s.tunnel_open) {
        push(s, nullptr, 0);  // empty LAST chunk to close the response
        fetches_ok++;
      } else {
        send_(s.id + 16384, NI_MSG_CLOSE, nullptr, 0, NI_MFLAG_LAST);
      }
      return -1;
    }
    int e = NI_SOCKERR();
    if (e == NI_EWOULDBLOCK) {
      if (now_ms_ - s.start_ms > opt_.io_timeout_ms) {
        s.done = true;
        return -1;
      }
      return 0;
    }
    s.done = true;
    return -1;
  }
}

int GiverGateway::pump(int64_t now_ms) {
  now_ms_ = now_ms;
  std::vector<uint16_t> dead;
  for (auto& kv : streams_) {
    Stream& s = *kv.second;
    if (!s.start_ms) s.start_ms = now_ms;
    if (s.sock < 0 && !s.head_done) continue;
    if (s.sock < 0) {
      // finish the connect handshake bookkeeping
      step(s, now_ms);
      continue;
    }
    if (s.sock >= 0 && !s.sent_req && !s.connected) {
      // a non-blocking connect completes asynchronously; treat "can send" as connected
      fd_set w;
      FD_ZERO(&w);
      FD_SET(s.sock, &w);
      timeval tv{0, 0};
      if (select((int)s.sock + 1, nullptr, &w, nullptr, &tv) > 0) s.connected = true;
      else if (now_ms - s.connected_at_ms > opt_.connect_timeout_ms) {
        connect_fail++;
        std::string why = "upstream connect timeout";
        send_(s.id + 16384, NI_MSG_ABORT, (const uint8_t*)why.data(), why.size(), NI_MFLAG_LAST);
        dead.push_back(kv.first);
        continue;
      }
    }
    if (s.sock >= 0 && s.connected) {
      if (step(s, now_ms) < 0) dead.push_back(kv.first);
    }
  }
  for (uint16_t id : dead) {
    auto it = streams_.find(id);
    if (it != streams_.end()) {
      if (it->second->sock >= 0) ni_closesock(it->second->sock);
      streams_.erase(it);
    }
  }
  return NI_OK;
}

std::string GiverGateway::describe() const {
  std::ostringstream o;
  o << (opt_.allow_http ? "http" : "no-http") << "+" << (opt_.allow_connect ? "connect" : "no-connect")
    << " cap=" << opt_.max_response << "B";
  return o.str();
}

// ------------------------------------------------------------------ direct fetch helper
int GiverGateway::http_fetch(const std::string& url, size_t max_bytes, ProxyResponse* out,
                             std::string* err) {
  Url u;
  if (!parse_url(url, &u)) {
    *err = "bad url";
    return NI_ERR_PARAM;
  }
  if (u.tls) {
    *err = "https requires the CONNECT tunnel (no plain-socket TLS in this helper)";
    return NI_ERR_UNSUPPORTED;
  }
  std::string ip;
  if (resolve(u.host, &ip) != NI_OK) {
    *err = "dns failed";
    return NI_ERR_IO;
  }
  sock_t s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    *err = "socket failed";
    return NI_ERR_IO;
  }
  timeval tv;
  tv.tv_sec = 15;
  tv.tv_usec = 0;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
  sockaddr_in sa;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)u.port);
  inet_pton(AF_INET, ip.c_str(), &sa.sin_addr);
  if (::connect(s, (sockaddr*)&sa, sizeof sa) != 0) {
    *err = "connect failed";
    ni_closesock(s);
    return NI_ERR_IO;
  }
  std::ostringstream req;
  req << "GET " << u.path << " HTTP/1.1\r\nHost: " << u.host << "\r\nConnection: close\r\n"
      << "User-Agent: NoiseInternet/1.0 (verifier)\r\nAccept: */*\r\n\r\n";
  std::string r = req.str();
  if (::send(s, r.data(), r.size(), 0) != (int)r.size()) {
    *err = "send failed";
    ni_closesock(s);
    return NI_ERR_IO;
  }
  std::string all;
  char buf[4096];
  for (;;) {
    int n = (int)::recv(s, buf, sizeof buf, 0);
    if (n <= 0) break;
    all.append(buf, (size_t)n);
    if (all.size() > max_bytes) break;
  }
  ni_closesock(s);
  if (all.empty()) {
    *err = "no response";
    return NI_ERR_IO;
  }
  size_t h = all.find("\r\n\r\n");
  if (h == std::string::npos) {
    *err = "malformed response";
    return NI_ERR_IO;
  }
  std::istringstream is(all.substr(0, h));
  std::string line;
  std::getline(is, line);
  if (line.size() >= 15) out->status = atoi(line.c_str() + 9);
  while (std::getline(is, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t c = line.find(':');
    if (c != std::string::npos)
      out->headers.emplace_back(lower(line.substr(0, c)), line.substr(c + 1));
  }
  out->body = all.substr(h + 4);
  out->reason = "OK";
  return NI_OK;
}

// ------------------------------------------------------------------ internet verifier
void InternetVerify::stage(const std::string& n, bool ok, const std::string& detail, double ms) {
  Stage st;
  st.name = n;
  st.ok = ok;
  st.detail = detail;
  st.ms = ms;
  stages.push_back(st);
}

std::string InternetVerify::to_json() const {
  std::ostringstream o;
  o << "{\"ok\": " << (ok ? "true" : "false") << ", \"url\": \"" << url
    << "\", \"bytes\": " << bytes << ", \"expect_crc32\": " << expect_crc << ", \"got_crc32\": " << got_crc
    << ", \"expect_sha256\": \"" << expect_sha256 << "\", \"got_sha256\": \"" << got_sha256
    << "\", \"total_ms\": " << total_ms << ", \"stages\": [";
  for (size_t i = 0; i < stages.size(); i++) {
    if (i) o << ", ";
    o << "{\"name\": \"" << stages[i].name << "\", \"ok\": " << (stages[i].ok ? "true" : "false")
      << ", \"ms\": " << stages[i].ms << ", \"detail\": \"" << stages[i].detail << "\"}";
  }
  o << "]}";
  return o.str();
}

// Drives the receiver's OWN local proxy over a real socket, exactly as an application would,
// so nothing in this path bypasses the acoustic link.
int ni_verify_internet(const std::string& url, ReceiverGateway& rg, int64_t now_ms_base,
                       InternetVerify* out) {
  out->url = url;
  Url u;
  if (!parse_url(url, &u)) {
    out->stage("url", false, "unparsable url", 0);
    return NI_ERR_PARAM;
  }
  (void)now_ms_base;
  std::string tok = rg.token();
  std::ostringstream req;
  req << "GET " << url << " HTTP/1.1\r\nHost: " << u.host << "\r\nProxy-Authorization: Noise "
      << tok << "\r\nAccept: */*\r\nConnection: close\r\n\r\n";
  std::string body = req.str();
  sock_t s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    out->stage("local-connect", false, "socket() failed", 0);
    return NI_ERR_IO;
  }
  sockaddr_in sa;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons(8471);
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  auto t0 = clock();
  int rc = ::connect(s, (sockaddr*)&sa, sizeof sa);
  double ms_conn = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;
  if (rc != 0) {
    out->stage("local-connect", false, "cannot reach the local proxy listener", ms_conn);
    ni_closesock(s);
    return NI_ERR_IO;
  }
  out->stage("local-connect", true, "connected to the receiver's loopback proxy", ms_conn);
  ::send(s, body.data(), body.size(), 0);
  std::string all;
  for (;;) {
    char buf[4096];
    int n = (int)::recv(s, buf, sizeof buf, 0);
    if (n <= 0) break;
    all.append(buf, (size_t)n);
    if (all.size() > (size_t)4 << 20) break;
  }
  ni_closesock(s);
  size_t h = all.find("\r\n\r\n");
  if (h == std::string::npos) {
    out->stage("proxy-response", false, "no complete response from the proxy", 0);
    return NI_ERR_IO;
  }
  out->bytes = all.size() - h - 4;
  out->stage("proxy-response", true, "response head received over the acoustic link", 0);
  std::string b = all.substr(h + 4);
  out->got_crc = ni_crc32(b.data(), b.size(), 0);
  uint8_t sh[32];
  ni_sha256(b.data(), b.size(), sh);
  out->got_sha256 = ni_hex(sh, 32);
  return NI_OK;
}

}  // namespace ni
