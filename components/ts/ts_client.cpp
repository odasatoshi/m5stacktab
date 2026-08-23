#include <cstdint>
#include "ts_client.hpp"

#include <cstdio>
#include <algorithm>
#include <cstring>

#include <cerrno>
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
constexpr uint32_t kStreamRegister = 1;  // 最初の register。以後は +2 ずつ増やす
// 対話ログインで承認を待つ間、register を投げ直す間隔（秒）。
// **人間がスマホを取り出して認証するまで分単位かかる**ので、詰めても意味がない。
constexpr int      kAuthPollSec    = 3;

int connect_tcp(const char* host, uint16_t port, int timeout_sec)
{
    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", port);

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res     = nullptr;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return -1;

    // 候補を順に試す。最初の 1 件で諦めると、AAAA が先に返る環境で
    // IPv6 の経路が無いだけで失敗してしまう。
    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, 0);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        timeval tv{timeout_sec, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        // 送信側にも期限を入れる。入れないと相手が読まないときに永久にブロックする。
        timeval snd{15, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
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

bool Client::set_keys(const uint8_t machine_priv[32], const uint8_t node_priv[32],
                      const uint8_t disco_priv[32])
{
    const auto& c = wg::default_crypto();
    std::memcpy(machine_priv_, machine_priv, 32);
    std::memcpy(node_priv_, node_priv, 32);
    if (!c.dh_pubkey(node_pub_, node_priv_)) return false;

    // disco key は node key とは別にする。同じにすると disco の秘密鍵が
    // WireGuard の秘密鍵と一致してしまい、鍵分離が失われる。
    uint8_t tmp[32];
    if (!disco_priv) {
        if (!c.random_bytes(tmp, 32)) return false;
        disco_priv = tmp;
    }
    return c.dh_pubkey(disco_pub_, disco_priv);
}

// /key?v=<capver> から Noise 公開鍵を取る。平文 HTTP で良い（鍵は公開情報）。
bool Client::fetch_server_key(uint8_t out[32])
{
    st_.state = ClientStatus::State::kFetchingKey;
    const int fd = connect_tcp(cfg_.host.c_str(), cfg_.port, 5);
    if (fd < 0) {
        set_error("cannot connect for /key");
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
        set_error("no publicKey in /key response");
        return false;
    }
    if (!key_from_string(key_str, "mkey:", out)) {
        set_error("bad publicKey format");
        return false;
    }
    return true;
}

ClientStatus Client::snapshot() const
{
    std::lock_guard<std::mutex> g(mu_);
    return st_;
}

void Client::set_error(std::string e)
{
    std::lock_guard<std::mutex> g(mu_);
    st_.error = std::move(e);
}

void Client::set_assigned_address(std::string a)
{
    std::lock_guard<std::mutex> g(mu_);
    st_.assigned_address = std::move(a);
}

void Client::set_domain(std::string d)
{
    std::lock_guard<std::mutex> g(mu_);
    st_.domain = std::move(d);
}

void Client::set_auth_url(std::string u)
{
    std::lock_guard<std::mutex> g(mu_);
    st_.auth_url = std::move(u);
}

void Client::reset_status()
{
    std::lock_guard<std::mutex> g(mu_);
    st_ = ClientStatus{};
}

bool Client::run_once()
{
    const auto& c = wg::default_crypto();
    stop_         = false;
    reset_status();

    uint8_t server_pub[32];
    if (!fetch_server_key(server_pub)) {
        st_.state = ClientStatus::State::kFailed;
        return false;
    }

    Handshake hs(c);
    if (!hs.init(machine_priv_, server_pub, cfg_.capability_version)) {
        set_error("handshake init failed");
        st_.state = ClientStatus::State::kFailed;
        return false;
    }
    uint8_t msg1[kInitiationLen];
    if (!hs.create_initiation(msg1)) {
        set_error("create_initiation failed (no entropy?)");
        st_.state = ClientStatus::State::kFailed;
        return false;
    }

    st_.state    = ClientStatus::State::kConnecting;
    const int fd = connect_tcp(cfg_.host.c_str(), cfg_.port, 65);  // long-poll の keepalive より長く
    if (fd < 0) {
        set_error("connect failed");
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
        set_error("sending upgrade request failed");
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
            set_error("closed while reading upgrade response");
            st_.state = ClientStatus::State::kFailed;
            return false;
        }
        in.insert(in.end(), buf, buf + r);
        up = parse_upgrade_response(reinterpret_cast<const char*>(in.data()), in.size());
        if (up.status != UpgradeResult::Status::kIncomplete) break;
        if (in.size() > 16384) {
            set_error("upgrade response too large");
            st_.state = ClientStatus::State::kFailed;
            return false;
        }
    }
    if (up.status != UpgradeResult::Status::kOk) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "upgrade rejected (http %d)", up.http_status);
        set_error(buf);
        st_.state = ClientStatus::State::kFailed;
        return false;
    }
    // ヘッダの後ろに msg2 が続いていることが多いので捨てない。
    in.erase(in.begin(), in.begin() + static_cast<long>(up.header_len));

    // エラーレコード (type 3) は 51 バイトより短い。先に判定しないと
    // 「51 バイト待つ → 切断」でサーバの説明を捨ててしまう。
    for (;;) {
        if (in.size() >= 3 && in[0] == kMsgError) {
            const size_t elen = static_cast<size_t>((in[1] << 8) | in[2]);
            while (in.size() < 3 + elen) {
                uint8_t       buf[512];
                const ssize_t r = recv(fd, buf, sizeof(buf), 0);
                if (r <= 0) break;
                in.insert(in.end(), buf, buf + r);
            }
            const size_t avail = (in.size() > 3) ? std::min(elen, in.size() - 3) : 0;
            set_error(std::string("server rejected the handshake: ") +
                      std::string(reinterpret_cast<const char*>(in.data() + 3), avail));
            st_.state = ClientStatus::State::kFailed;
            return false;
        }
        if (in.size() >= kResponseLen) break;
        uint8_t       buf[2048];
        const ssize_t r = recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) {
            set_error("closed while reading noise response");
            st_.state = ClientStatus::State::kFailed;
            return false;
        }
        in.insert(in.end(), buf, buf + r);
    }
    Session sess{};
    if (!hs.consume_response(in.data(), sess)) {
        set_error("noise response rejected (capability version mismatch?)");
        st_.state = ClientStatus::State::kFailed;
        return false;
    }
    in.erase(in.begin(), in.begin() + kResponseLen);

    Record rec(c);
    rec.set_session(sess);

    // 受信バッファには上限を設ける。設けないとサーバが送るだけでヒープが枯渇する。
    constexpr size_t kMaxBuffered = 256 * 1024;

    std::vector<uint8_t> plain;
    auto pump = [&](int timeout_ms) -> bool {
        // まず溜まっている分を処理する。recv を先に呼ぶと、既にバッファにある
        // レコードを扱う前にタイムアウト分だけ待ってしまう。
        bool progressed = false;
        for (;;) {
            uint8_t      out[kMaxPlaintextLen];
            size_t       consumed = 0;
            const size_t got = rec.open(out, sizeof(out), in.data(), in.size(), &consumed);
            if (consumed == SIZE_MAX) {
                set_error("record decrypt failed");
                return false;
            }
            if (consumed == 0) break;
            in.erase(in.begin(), in.begin() + static_cast<long>(consumed));
            plain.insert(plain.end(), out, out + got);
            progressed = true;
        }
        if (progressed) return true;

        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        uint8_t       buf[2048];
        const ssize_t r = recv(fd, buf, sizeof(buf), 0);
        if (r == 0) {
            set_error("connection closed by peer");
            return false;
        }
        if (r < 0) {
            // タイムアウトだけを続行扱いにする。他のエラーで回し続けると
            // 死んだソケットで CPU を焼き、しかも「稼働中」と誤認する。
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                set_error(std::string("recv failed: ") + std::strerror(errno));
                return false;
            }
            return true;
        }
        in.insert(in.end(), buf, buf + r);
        if (in.size() > kMaxBuffered || plain.size() > kMaxBuffered) {
            set_error("receive buffer limit exceeded");
            return false;
        }
        for (;;) {
            uint8_t      out[kMaxPlaintextLen];
            size_t       consumed = 0;
            const size_t got = rec.open(out, sizeof(out), in.data(), in.size(), &consumed);
            if (consumed == SIZE_MAX) {
                set_error("record decrypt failed");
                return false;
            }
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

    // EarlyNoise（来ないこともある）。「来ない」と「まだ届いていない」を区別して、
    // 途中まで届いている場合は残りを待つ。混同すると HTTP/2 を先に送ってしまい、
    // \xff\xff\xff... がフレーム長として読まれて永久にストールする。
    for (int i = 0; i < 5; ++i) {
        const auto r = parse_early_noise(plain.data(), plain.size(), nullptr, nullptr);
        if (r == EarlyNoiseResult::kFound) {
            std::string json;
            size_t      consumed = 0;
            parse_early_noise(plain.data(), plain.size(), &json, &consumed);
            plain.erase(plain.begin(), plain.begin() + static_cast<long>(consumed));
            break;
        }
        if (r == EarlyNoiseResult::kNotPresent && !plain.empty()) break;  // HTTP/2 が始まっている
        if (!pump(1000)) {
            set_error(st_.error.empty() ? "closed after noise handshake" : st_.error);
            st_.state = ClientStatus::State::kFailed;
            return false;
        }
    }

    // HTTP/2 開始
    {
        uint8_t      buf[256];
        const size_t n = h2_build_preface(buf, sizeof(buf));
        if (n == 0 || !seal_send(buf, n)) {
            set_error("sending http/2 preface failed");
            st_.state = ClientStatus::State::kFailed;
            return false;
        }
    }

    // 受信フレームを処理しながら、必要な ACK を返す。
    std::string          body_register;
    std::vector<uint8_t> map_stream;
    bool                 sent_register = false, sent_map = false;
    bool                 register_done = false;   // register の stream に END_STREAM が来たか
    // **HTTP/2 のストリーム id は増える奇数**でなければならない。対話ログイン (#59) では
    // register を何度も投げ直すので、固定の 1 / 3 では 2 回目が protocol error になる。
    uint32_t             next_sid = kStreamRegister;
    uint32_t             reg_sid  = 0;
    uint32_t             map_sid  = 0;
    auto                 take_sid = [&]() { const uint32_t s = next_sid; next_sid += 2; return s; };
    // 対話ログインで待っている間の状態。
    std::string          shown_auth_url;
    int                  register_after = 0;  // ループ回数（pump が 1 秒待つので秒とほぼ同じ）

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
                    if (f.stream_id == reg_sid) {
                        body_register.append(reinterpret_cast<const char*>(f.payload),
                                             f.payload_len);
                        if (f.flags & kFlagEndStream) register_done = true;
                        if (body_register.size() > kMaxBuffered) {
                            set_error("register response too large");
                            return false;
                        }
                    } else if (f.stream_id == map_sid) {
                        map_stream.insert(map_stream.end(), f.payload, f.payload + f.payload_len);
                        if (map_stream.size() > kMaxBuffered) {
                            set_error("netmap buffer limit exceeded");
                            return false;
                        }
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
                case H2Type::kHeaders:
                    // 応答ステータスを見る。404/500 を素通りさせると、本文が JSON でないために
                    // 「machine not authorized」のような誤ったエラーになる。
                    if (!h2_headers_is_status_200(f.payload, f.payload_len)) {
                        set_error("control plane returned a non-200 status");
                        return false;
                    }
                    if ((f.flags & kFlagEndStream) && f.stream_id == reg_sid) {
                        register_done = true;
                    }
                    break;
                case H2Type::kRstStream:
                    // long-poll のストリームを切られたら再接続が必要。放置すると
                    // 何も受け取らないまま「稼働中」を続けてしまう。
                    set_error("server reset the stream");
                    return false;
                case H2Type::kGoaway:
                    set_error("server sent GOAWAY");
                    return false;
                default:
                    break;  // WINDOW_UPDATE などは読み捨てて良い
            }
        }
        plain.erase(plain.begin(), plain.begin() + static_cast<long>(off));
        return true;
    };

    st_.state = ClientStatus::State::kRegistering;
    for (int i = 0; i < 600 && !stop_; ++i) {
        if (!pump(1000)) {
            if (st_.error.empty()) set_error("connection closed");
            st_.state = ClientStatus::State::kFailed;
            return false;
        }
        if (!handle_frames()) {
            if (st_.error.empty()) set_error("http/2 error");
            st_.state = ClientStatus::State::kFailed;
            return false;
        }

        // SETTINGS をやり取りしたら register を送る。
        // 対話ログイン中は register_after 秒だけ空けてから投げ直す。
        if (!sent_register && i >= register_after) {
            RegisterParams rp;
            rp.capability_version = cfg_.capability_version;
            rp.node_key           = key_to_string("nodekey:", node_pub_);
            rp.auth_key           = cfg_.auth_key;
            rp.hostname           = cfg_.hostname;
            const std::string body = build_register_request(rp);
            std::vector<uint8_t> buf(body.size() + 1024);
            reg_sid        = take_sid();
            const size_t n = h2_build_post(buf.data(), buf.size(), reg_sid,
                                           cfg_.host.c_str(), "/machine/register",
                                           reinterpret_cast<const uint8_t*>(body.data()),
                                           body.size());
            if (n == 0 || !seal_send(buf.data(), n)) {
                set_error("sending register failed");
                st_.state = ClientStatus::State::kFailed;
                return false;
            }
            sent_register = true;
            continue;
        }

        // 本文が複数の DATA に分かれることがあるので END_STREAM を待つ。
        // 途中の JSON を解析すると必ず失敗する。
        if (!st_.registered && register_done && !body_register.empty()) {
            const auto rr = parse_register_response(body_register);
            if (!rr.error.empty()) {
                set_error("register rejected: " + rr.error);
                st_.state = ClientStatus::State::kFailed;
                return false;
            }
            if (!rr.auth_url.empty()) {
                // auth key が無い / 無効なとき (#59)。
                if (!cfg_.interactive) {
                    set_error("interactive login required: " + rr.auth_url);
                    st_.state = ClientStatus::State::kFailed;
                    return false;
                }
                // **端末は URL を見せて待つだけ。** 認証は人間が手元のブラウザで
                // 済ませる（Google なり GitHub なり）ので、ここに OAuth は要らない。
                if (shown_auth_url != rr.auth_url) {
                    shown_auth_url = rr.auth_url;
                    set_auth_url(rr.auth_url);
                    if (on_auth_url_) on_auth_url_(rr.auth_url);
                }
                st_.state = ClientStatus::State::kAuthPending;
                // 承認されるまで投げ直す。**新しいストリームで**投げること
                // （take_sid が次の奇数を返す）。
                body_register.clear();
                register_done  = false;
                sent_register  = false;
                register_after = i + kAuthPollSec;
                continue;
            }
            // 通ったら URL は消す。残すと承認後も QR が出たままになる。
            if (!shown_auth_url.empty()) set_auth_url("");
            if (!rr.machine_authorized) {
                set_error("machine not authorized");
                st_.state = ClientStatus::State::kFailed;
                return false;
            }
            st_.registered = true;
        }

        if (st_.registered && !sent_map) {
            MapParams mp;
            mp.capability_version = cfg_.capability_version;
            mp.node_key           = key_to_string("nodekey:", node_pub_);
            mp.disco_key          = key_to_string("discokey:", disco_pub_);
            mp.hostname           = cfg_.hostname;
            mp.endpoints          = cfg_.endpoints;
            mp.stream             = true;
            const std::string body = build_map_request(mp);
            std::vector<uint8_t> buf(body.size() + 1024);
            map_sid        = take_sid();
            const size_t n = h2_build_post(buf.data(), buf.size(), map_sid, cfg_.host.c_str(),
                                           "/machine/map",
                                           reinterpret_cast<const uint8_t*>(body.data()),
                                           body.size());
            if (n == 0 || !seal_send(buf.data(), n)) {
                set_error("sending map request failed");
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
                set_error("bad netmap framing");
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
            if (json_find_first_in_array(msg, "Addresses", &addr)) set_assigned_address(addr);
            std::string domain;
            if (json_find_string(msg, "Domain", &domain)) set_domain(domain);
            if (on_map_) on_map_(msg);
        }
    }

    set_error(stop_ ? "stopped" : "timed out");
    return st_.state == ClientStatus::State::kMapping;
}

}  // namespace ts
