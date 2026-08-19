#include <cstdint>
#include "ts_client.hpp"

#include <cstdio>
#include <algorithm>
#include <cstring>

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "h2.hpp"
#include "noise.hpp"
#include "ts_control.hpp"
#include "ts_noise.hpp"

namespace ts {
namespace {

// HTTP/2 のストリーム ID はクライアント側は奇数。
constexpr uint32_t kStreamRegister = 1;
constexpr uint32_t kStreamMap      = 3;

int connect_tcp(const char* host, uint16_t port, int timeout_sec)
{
    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", port);

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res     = nullptr;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        timeval tv{timeout_sec, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    return fd;
}

bool send_all(int fd, const void* p, size_t len)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    while (len > 0) {
        const ssize_t n = send(fd, b, len, 0);
        if (n <= 0) return false;
        b += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

void Client::set_keys(const uint8_t machine_priv[32], const uint8_t node_priv[32])
{
    std::memcpy(machine_priv_, machine_priv, 32);
    std::memcpy(node_priv_, node_priv, 32);
    wg::default_crypto().dh_pubkey(node_pub_, node_priv_);
}

// /key?v=<capver> から Noise 公開鍵を取る。平文 HTTP で良い（鍵は公開情報）。
bool Client::fetch_server_key(uint8_t out[32])
{
    st_.state = ClientStatus::State::kFetchingKey;
    const int fd = connect_tcp(cfg_.host.c_str(), cfg_.port, 5);
    if (fd < 0) {
        st_.error = "cannot connect for /key";
        return false;
    }
    char      req[256];
    const int n = std::snprintf(req, sizeof(req),
                                "GET /key?v=%u HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                                cfg_.capability_version, cfg_.host.c_str());
    bool ok = (n > 0) && send_all(fd, req, static_cast<size_t>(n));

    std::string resp;
    if (ok) {
        char buf[512];
        for (;;) {
            const ssize_t r = recv(fd, buf, sizeof(buf), 0);
            if (r <= 0) break;
            resp.append(buf, static_cast<size_t>(r));
            if (resp.size() > 8192) break;
        }
    }
    close(fd);

    std::string key_str;
    if (!json_find_string(resp, "publicKey", &key_str)) {
        st_.error = "no publicKey in /key response";
        return false;
    }
    if (!key_from_string(key_str, "mkey:", out)) {
        st_.error = "bad publicKey format";
        return false;
    }
    return true;
}

bool Client::run_once()
{
    const auto& c = wg::default_crypto();
    stop_         = false;
    st_           = ClientStatus{};

    uint8_t server_pub[32];
    if (!fetch_server_key(server_pub)) {
        st_.state = ClientStatus::State::kFailed;
        return false;
    }

    Handshake hs(c);
    if (!hs.init(machine_priv_, server_pub, cfg_.capability_version)) {
        st_.error = "handshake init failed";
        st_.state = ClientStatus::State::kFailed;
        return false;
    }
    uint8_t msg1[kInitiationLen];
    if (!hs.create_initiation(msg1)) {
        st_.error = "create_initiation failed (no entropy?)";
        st_.state = ClientStatus::State::kFailed;
        return false;
    }

    st_.state    = ClientStatus::State::kConnecting;
    const int fd = connect_tcp(cfg_.host.c_str(), cfg_.port, 65);  // long-poll の keepalive より長く
    if (fd < 0) {
        st_.error = "connect failed";
        st_.state = ClientStatus::State::kFailed;
        return false;
    }

    struct Closer {
        int fd;
        ~Closer() { if (fd >= 0) close(fd); }
    } closer{fd};

    char         req[1024];
    const size_t req_len = build_upgrade_request(req, sizeof(req), cfg_.host.c_str(), msg1,
                                                 sizeof(msg1));
    if (req_len == 0 || !send_all(fd, req, req_len)) {
        st_.error = "sending upgrade request failed";
        st_.state = ClientStatus::State::kFailed;
        return false;
    }

    st_.state = ClientStatus::State::kHandshaking;
    std::vector<uint8_t> in;
    UpgradeResult        up;
    for (;;) {
        uint8_t       buf[2048];
        const ssize_t r = recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) {
            st_.error = "closed while reading upgrade response";
            st_.state = ClientStatus::State::kFailed;
            return false;
        }
        in.insert(in.end(), buf, buf + r);
        up = parse_upgrade_response(reinterpret_cast<const char*>(in.data()), in.size());
        if (up.status != UpgradeResult::Status::kIncomplete) break;
        if (in.size() > 16384) {
            st_.error = "upgrade response too large";
            st_.state = ClientStatus::State::kFailed;
            return false;
        }
    }
    if (up.status != UpgradeResult::Status::kOk) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "upgrade rejected (http %d)", up.http_status);
        st_.error = buf;
        st_.state = ClientStatus::State::kFailed;
        return false;
    }
    // ヘッダの後ろに msg2 が続いていることが多いので捨てない。
    in.erase(in.begin(), in.begin() + static_cast<long>(up.header_len));

    while (in.size() < kResponseLen) {
        uint8_t       buf[2048];
        const ssize_t r = recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) {
            st_.error = "closed while reading noise response";
            st_.state = ClientStatus::State::kFailed;
            return false;
        }
        in.insert(in.end(), buf, buf + r);
    }
    if (in[0] == kMsgError) {
        const size_t len = static_cast<size_t>((in[1] << 8) | in[2]);
        st_.error = "server error: " + std::string(reinterpret_cast<const char*>(in.data() + 3),
                                                  std::min(len, in.size() - 3));
        st_.state = ClientStatus::State::kFailed;
        return false;
    }
    Session sess{};
    if (!hs.consume_response(in.data(), sess)) {
        st_.error = "noise response rejected (capability version mismatch?)";
        st_.state = ClientStatus::State::kFailed;
        return false;
    }
    in.erase(in.begin(), in.begin() + kResponseLen);

    Record rec(c);
    rec.set_session(sess);

    std::vector<uint8_t> plain;
    auto pump = [&](int timeout_ms) -> bool {
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        uint8_t       buf[2048];
        const ssize_t r = recv(fd, buf, sizeof(buf), 0);
        if (r > 0) {
            in.insert(in.end(), buf, buf + r);
        } else if (r == 0) {
            return false;  // 切断
        }
        for (;;) {
            uint8_t      out[kMaxPlaintextLen];
            size_t       consumed = 0;
            const size_t got = rec.open(out, sizeof(out), in.data(), in.size(), &consumed);
            if (consumed == SIZE_MAX) return false;
            if (consumed == 0) break;
            in.erase(in.begin(), in.begin() + static_cast<long>(consumed));
            plain.insert(plain.end(), out, out + got);
        }
        return true;
    };
    auto seal_send = [&](const uint8_t* p, size_t len) -> bool {
        while (len > 0) {
            // 平文は 4077 バイトごとに分割する。超えるとサーバが切断する。
            const size_t chunk = (len < kMaxPlaintextLen) ? len : kMaxPlaintextLen;
            uint8_t      wire[kMaxMessageSize];
            const size_t n = rec.seal(wire, sizeof(wire), p, chunk);
            if (n == 0 || !send_all(fd, wire, n)) return false;
            p += chunk;
            len -= chunk;
        }
        return true;
    };

    // EarlyNoise（来ないこともある）
    if (!pump(3000)) {
        st_.error = "closed after noise handshake";
        st_.state = ClientStatus::State::kFailed;
        return false;
    }
    {
        std::string json;
        size_t      consumed = 0;
        if (parse_early_noise(plain.data(), plain.size(), &json, &consumed)) {
            plain.erase(plain.begin(), plain.begin() + static_cast<long>(consumed));
        }
    }

    // HTTP/2 開始
    {
        uint8_t      buf[256];
        const size_t n = h2_build_preface(buf, sizeof(buf));
        if (n == 0 || !seal_send(buf, n)) {
            st_.error = "sending http/2 preface failed";
            st_.state = ClientStatus::State::kFailed;
            return false;
        }
    }

    // 受信フレームを処理しながら、必要な ACK を返す。
    std::string body_register, framed_map_owner;
    std::vector<uint8_t> map_stream;
    bool                 sent_register = false, sent_map = false;

    auto handle_frames = [&]() -> bool {
        size_t off = 0;
        while (off < plain.size()) {
            H2Frame f;
            size_t  consumed = 0;
            if (!h2_parse_frame(plain.data() + off, plain.size() - off, &f, &consumed)) break;
            off += consumed;

            switch (f.type) {
                case H2Type::kSettings:
                    if ((f.flags & kFlagAck) == 0) {
                        uint8_t buf[64];
                        const size_t n = h2_build_settings_ack(buf, sizeof(buf));
                        if (!seal_send(buf, n)) return false;
                    }
                    break;
                case H2Type::kPing:
                    if ((f.flags & kFlagAck) == 0 && f.payload_len >= 8) {
                        uint8_t buf[64];
                        const size_t n = h2_build_ping_ack(buf, sizeof(buf), f.payload);
                        if (!seal_send(buf, n)) return false;
                    }
                    break;
                case H2Type::kData: {
                    if (f.stream_id == kStreamRegister) {
                        body_register.append(reinterpret_cast<const char*>(f.payload),
                                             f.payload_len);
                    } else if (f.stream_id == kStreamMap) {
                        map_stream.insert(map_stream.end(), f.payload, f.payload + f.payload_len);
                    }
                    // 受信ウィンドウを返す。これを送らないと 64KB で止まる。
                    if (f.payload_len > 0) {
                        uint8_t buf[64];
                        size_t  n = h2_build_window_update(buf, sizeof(buf), 0,
                                                          static_cast<uint32_t>(f.payload_len));
                        if (!seal_send(buf, n)) return false;
                        n = h2_build_window_update(buf, sizeof(buf), f.stream_id,
                                                   static_cast<uint32_t>(f.payload_len));
                        if (!seal_send(buf, n)) return false;
                    }
                    break;
                }
                case H2Type::kGoaway:
                    st_.error = "server sent GOAWAY";
                    return false;
                default:
                    break;  // HEADERS / WINDOW_UPDATE / RST_STREAM は読み捨てて良い
            }
        }
        plain.erase(plain.begin(), plain.begin() + static_cast<long>(off));
        return true;
    };

    st_.state = ClientStatus::State::kRegistering;
    for (int i = 0; i < 600 && !stop_; ++i) {
        if (!pump(1000)) {
            if (st_.error.empty()) st_.error = "connection closed";
            st_.state = ClientStatus::State::kFailed;
            return false;
        }
        if (!handle_frames()) {
            if (st_.error.empty()) st_.error = "http/2 error";
            st_.state = ClientStatus::State::kFailed;
            return false;
        }

        // SETTINGS をやり取りしたら register を送る。
        if (!sent_register) {
            RegisterParams rp;
            rp.capability_version = cfg_.capability_version;
            rp.node_key           = key_to_string("nodekey:", node_pub_);
            rp.auth_key           = cfg_.auth_key;
            rp.hostname           = cfg_.hostname;
            const std::string body = build_register_request(rp);
            std::vector<uint8_t> buf(body.size() + 1024);
            const size_t n = h2_build_post(buf.data(), buf.size(), kStreamRegister,
                                           cfg_.host.c_str(), "/machine/register",
                                           reinterpret_cast<const uint8_t*>(body.data()),
                                           body.size());
            if (n == 0 || !seal_send(buf.data(), n)) {
                st_.error = "sending register failed";
                st_.state = ClientStatus::State::kFailed;
                return false;
            }
            sent_register = true;
            continue;
        }

        if (!st_.registered && !body_register.empty()) {
            const auto rr = parse_register_response(body_register);
            if (!rr.error.empty()) {
                st_.error = "register rejected: " + rr.error;
                st_.state = ClientStatus::State::kFailed;
                return false;
            }
            if (!rr.auth_url.empty()) {
                // auth key が無い / 無効なとき。対話ログインは実装しない。
                st_.error = "interactive login required: " + rr.auth_url;
                st_.state = ClientStatus::State::kFailed;
                return false;
            }
            if (!rr.machine_authorized) {
                st_.error = "machine not authorized";
                st_.state = ClientStatus::State::kFailed;
                return false;
            }
            st_.registered = true;
        }

        if (st_.registered && !sent_map) {
            MapParams mp;
            mp.capability_version = cfg_.capability_version;
            mp.node_key           = key_to_string("nodekey:", node_pub_);
            mp.disco_key          = key_to_string("discokey:", node_pub_);
            mp.hostname           = cfg_.hostname;
            mp.endpoints          = cfg_.endpoints;
            mp.stream             = true;
            const std::string body = build_map_request(mp);
            std::vector<uint8_t> buf(body.size() + 1024);
            const size_t n = h2_build_post(buf.data(), buf.size(), kStreamMap, cfg_.host.c_str(),
                                           "/machine/map",
                                           reinterpret_cast<const uint8_t*>(body.data()),
                                           body.size());
            if (n == 0 || !seal_send(buf.data(), n)) {
                st_.error = "sending map request failed";
                st_.state = ClientStatus::State::kFailed;
                return false;
            }
            sent_map  = true;
            st_.state = ClientStatus::State::kMapping;
            continue;
        }

        // netmap のフレーミングを剥がす（4 バイト LE 長 + JSON）
        for (;;) {
            const uint8_t* payload = nullptr;
            size_t         plen    = 0;
            const size_t   used =
                take_framed_message(map_stream.data(), map_stream.size(), &payload, &plen);
            if (used == 0) break;
            if (used == SIZE_MAX) {
                st_.error = "bad netmap framing";
                st_.state = ClientStatus::State::kFailed;
                return false;
            }
            const std::string msg(reinterpret_cast<const char*>(payload), plen);
            map_stream.erase(map_stream.begin(), map_stream.begin() + static_cast<long>(used));

            ++st_.map_messages;
            bool keepalive = false;
            if (json_find_bool(msg, "KeepAlive", &keepalive) && keepalive) {
                ++st_.keepalives;
                continue;  // KeepAlive のときは他フィールドを見ない
            }
            std::string addr;
            if (json_find_first_in_array(msg, "Addresses", &addr)) st_.assigned_address = addr;
            std::string domain;
            if (json_find_string(msg, "Domain", &domain)) st_.domain = domain;
            if (on_map_) on_map_(msg);
        }
    }

    st_.error = stop_ ? "stopped" : "timed out";
    return st_.state == ClientStatus::State::kMapping;
}

}  // namespace ts
