#include <cstdint>
#include "ts_control.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <mbedtls/base64.h>

namespace ts {
namespace {

constexpr size_t kMaxFramedMessage = 4 * 1024 * 1024;  // 壊れた長さで暴走しないための上限

const char kHexDigits[] = "0123456789abcdef";

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// JSON の文字列としてエスケープする（制御文字と " \ だけ処理すれば足りる）。
std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// 大文字小文字を無視して探す（HTTP ヘッダ名の比較用）。
const char* find_ci(const char* haystack, size_t len, const char* needle)
{
    const size_t nlen = std::strlen(needle);
    if (nlen == 0 || len < nlen) return nullptr;
    for (size_t i = 0; i + nlen <= len; ++i) {
        size_t j = 0;
        for (; j < nlen; ++j) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) break;
        }
        if (j == nlen) return haystack + i;
    }
    return nullptr;
}

}  // namespace

std::string key_to_string(const char* prefix, const uint8_t key[32])
{
    std::string s = prefix;
    for (int i = 0; i < 32; ++i) {
        s += kHexDigits[key[i] >> 4];
        s += kHexDigits[key[i] & 0x0F];
    }
    return s;
}

bool key_from_string(const std::string& s, const char* prefix, uint8_t out[32])
{
    const size_t plen = std::strlen(prefix);
    if (s.size() != plen + 64) return false;
    if (s.compare(0, plen, prefix) != 0) return false;
    for (int i = 0; i < 32; ++i) {
        const int hi = hex_value(s[plen + i * 2]);
        const int lo = hex_value(s[plen + i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

size_t build_upgrade_request(char* out, size_t cap, const char* host, const uint8_t* msg1,
                            size_t msg1_len)
{
    // ハンドシェイクは base64（標準アルファベット、パディングあり）でヘッダに載せる。
    // ボディは空。RTT を 1 往復減らすための設計。
    unsigned char b64[256];
    size_t        b64_len = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len, msg1, msg1_len) != 0) return 0;

    const int n = std::snprintf(out, cap,
                                "POST /ts2021 HTTP/1.1\r\n"
                                "Host: %s\r\n"
                                "Upgrade: tailscale-control-protocol\r\n"
                                "Connection: upgrade\r\n"
                                "X-Tailscale-Handshake: %.*s\r\n"
                                "Content-Length: 0\r\n"
                                "\r\n",
                                host, static_cast<int>(b64_len), reinterpret_cast<char*>(b64));
    if (n <= 0 || static_cast<size_t>(n) >= cap) return 0;
    return static_cast<size_t>(n);
}

UpgradeResult parse_upgrade_response(const char* in, size_t len)
{
    UpgradeResult r;
    // ヘッダ終端を探す。ここから後ろは Noise のバイト列なので捨ててはいけない。
    const char* end = nullptr;
    for (size_t i = 0; i + 3 < len; ++i) {
        if (in[i] == '\r' && in[i + 1] == '\n' && in[i + 2] == '\r' && in[i + 3] == '\n') {
            end = in + i + 4;
            break;
        }
    }
    if (!end) return r;  // kIncomplete

    r.header_len            = static_cast<size_t>(end - in);
    const size_t header_len = r.header_len;

    // "HTTP/1.1 101 ..." を読む。
    // atoi をそのまま使うと、数字が無いステータス行で len を越えて読み進む
    // （ネットワークから来るバイト列なので範囲外読み込みになる）。
    if (header_len < 12 || std::memcmp(in, "HTTP/1.", 7) != 0) {
        r.status = UpgradeResult::Status::kBadStatus;
        return r;
    }
    {
        char digits[8] = {};
        size_t n = 0;
        size_t i = 9;
        while (i < header_len && in[i] == ' ') ++i;
        while (i < header_len && in[i] >= '0' && in[i] <= '9' && n + 1 < sizeof(digits)) {
            digits[n++] = in[i++];
        }
        if (n == 0) {
            r.status = UpgradeResult::Status::kBadStatus;
            return r;
        }
        r.http_status = std::atoi(digits);
    }
    if (r.http_status != 101) {
        r.status = UpgradeResult::Status::kBadStatus;
        return r;
    }
    if (!find_ci(in, header_len, "tailscale-control-protocol")) {
        r.status = UpgradeResult::Status::kBadUpgrade;
        return r;
    }
    r.status = UpgradeResult::Status::kOk;
    return r;
}

size_t take_framed_message(const uint8_t* in, size_t len, const uint8_t** payload,
                           size_t* payload_len)
{
    if (len < 4) return 0;
    // **リトルエンディアン**。ここをビッグエンディアンで読むと長さが化ける。
    const uint32_t n = static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
                       (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
    if (n > kMaxFramedMessage) return SIZE_MAX;
    if (len < 4 + static_cast<size_t>(n)) return 0;
    if (payload) *payload = in + 4;
    if (payload_len) *payload_len = n;
    return 4 + static_cast<size_t>(n);
}

std::string build_register_request(const RegisterParams& p)
{
    std::string s = "{";
    s += "\"Version\":" + std::to_string(p.capability_version);
    s += ",\"NodeKey\":\"" + json_escape(p.node_key) + "\"";
    if (!p.auth_key.empty()) {
        s += ",\"Auth\":{\"AuthKey\":\"" + json_escape(p.auth_key) + "\"}";
    }
    s += ",\"Hostinfo\":{\"Hostname\":\"" + json_escape(p.hostname) + "\"";
    s += ",\"OS\":\"" + json_escape(p.os) + "\"";
    s += ",\"OSVersion\":\"ESP-IDF\"}";
    // ゼロ値の Expiry は「期限なし」。省くと Go 側で必須扱いされることがある。
    s += ",\"Expiry\":\"0001-01-01T00:00:00Z\"";
    s += "}";
    return s;
}

std::string build_map_request(const MapParams& p)
{
    std::string s = "{";
    s += "\"Version\":" + std::to_string(p.capability_version);
    s += ",\"NodeKey\":\"" + json_escape(p.node_key) + "\"";
    if (!p.disco_key.empty()) s += ",\"DiscoKey\":\"" + json_escape(p.disco_key) + "\"";
    s += ",\"Stream\":" + std::string(p.stream ? "true" : "false");
    s += ",\"KeepAlive\":true";
    // zstd デコーダを載せたくないので圧縮は使わない。
    s += ",\"Compress\":\"\"";
    s += ",\"Hostinfo\":{\"Hostname\":\"" + json_escape(p.hostname) + "\",\"OS\":\"linux\"";
    // NetInfo は **常に入れる**。無いと Headscale が
    // 「node sent update but has no NetInfo in request or database」として扱い、
    // ピアに配る netmap にこのノードのエンドポイントが載らない（実機で確認）。
    // 結果、相手は DISCO の送り先が分からず永久に繋がらない。
    // DERP は使っていないので PreferredDERP は 0（「DERP ホームなし」の意味）。
    s += ",\"NetInfo\":{\"PreferredDERP\":" + std::to_string(p.preferred_derp);
    s += ",\"WorkingUDP\":true,\"LinkType\":\"wifi\"}";
    s += "}";
    if (!p.endpoints.empty()) {
        s += ",\"Endpoints\":[";
        for (size_t i = 0; i < p.endpoints.size(); ++i) {
            if (i) s += ",";
            s += "\"" + json_escape(p.endpoints[i]) + "\"";
        }
        s += "]";
        // 1 = local（STUN も portmap も使っていないので全部これ）
        s += ",\"EndpointTypes\":[";
        for (size_t i = 0; i < p.endpoints.size(); ++i) {
            if (i) s += ",";
            s += "1";
        }
        s += "]";
    }
    s += "}";
    return s;
}

bool json_find_string(const std::string& json, const char* key, std::string* out)
{
    const std::string pattern = std::string("\"") + key + "\":";
    size_t            pos     = json.find(pattern);
    if (pos == std::string::npos) return false;
    pos += pattern.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    std::string value;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos;
            switch (json[pos]) {
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                default: value += json[pos]; break;
            }
        } else {
            value += json[pos];
        }
        ++pos;
    }
    if (pos >= json.size()) return false;  // 終端の " が無い
    if (out) *out = value;
    return true;
}

bool json_find_bool(const std::string& json, const char* key, bool* out)
{
    const std::string pattern = std::string("\"") + key + "\":";
    size_t            pos     = json.find(pattern);
    if (pos == std::string::npos) return false;
    pos += pattern.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (json.compare(pos, 4, "true") == 0) {
        if (out) *out = true;
        return true;
    }
    if (json.compare(pos, 5, "false") == 0) {
        if (out) *out = false;
        return true;
    }
    return false;
}

bool json_find_first_in_array(const std::string& json, const char* key, std::string* out)
{
    const std::string pattern = std::string("\"") + key + "\":";
    size_t            pos     = json.find(pattern);
    if (pos == std::string::npos) return false;
    pos += pattern.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != '[') return false;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    const size_t start = pos;
    while (pos < json.size() && json[pos] != '"') ++pos;
    if (pos >= json.size()) return false;
    if (out) *out = json.substr(start, pos - start);
    return true;
}

RegisterResult parse_register_response(const std::string& json)
{
    RegisterResult r;
    json_find_bool(json, "MachineAuthorized", &r.machine_authorized);
    json_find_bool(json, "NodeKeyExpired", &r.node_key_expired);
    json_find_string(json, "AuthURL", &r.auth_url);
    json_find_string(json, "Error", &r.error);
    return r;
}

}  // namespace ts
