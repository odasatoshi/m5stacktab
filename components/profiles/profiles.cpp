#include "profiles.hpp"

#include <cstdio>
#include <cstdlib>

#include <cJSON.h>

namespace prof {
namespace {

std::string get_string(const cJSON* obj, const char* key)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : std::string();
}

int get_int(const cJSON* obj, const char* key, int def)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (v && cJSON_IsNumber(v)) ? static_cast<int>(v->valuedouble) : def;
}

std::vector<std::string> get_string_array(const cJSON* obj, const char* key)
{
    std::vector<std::string> out;
    const cJSON*             arr = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!arr || !cJSON_IsArray(arr)) return out;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, arr)
    {
        if (cJSON_IsString(item) && item->valuestring) out.emplace_back(item->valuestring);
    }
    return out;
}

// **鍵は keys/ 配下のファイル名だけを受ける。** パスを書けるようにすると
// JSON から SD の外（VFS の別マウント）まで指せてしまうし、そもそも
// 「JSON に秘密が混ざらない」という約束が緩む。
bool bad_key_name(const std::string& s)
{
    if (s.empty() || s.size() > 64) return true;
    return s.find('/') != std::string::npos || s.find('\\') != std::string::npos ||
           s == "." || s == "..";
}

void warn(Config* cfg, const std::string& msg) { cfg->warnings.push_back(msg); }

}  // namespace

const char* type_name(Type t)
{
    switch (t) {
        case Type::kSsh: return "ssh";
        case Type::kWireGuard: return "wireguard";
        case Type::kTailscale: return "tailscale";
    }
    return "?";
}

bool split_cidr(const std::string& cidr, std::string* addr, int* prefix, int def_prefix)
{
    const size_t slash = cidr.find('/');
    const std::string a = (slash == std::string::npos) ? cidr : cidr.substr(0, slash);
    if (a.empty()) return false;
    // 4 オクテットの点表記だけ受ける（この先で ip4addr_aton に渡すため）。
    int  octets = 0;
    int  digits = 0;
    int  value  = 0;
    for (size_t i = 0; i <= a.size(); ++i) {
        const char c = (i < a.size()) ? a[i] : '.';
        if (c >= '0' && c <= '9') {
            if (++digits > 3) return false;
            value = value * 10 + (c - '0');
        } else if (c == '.') {
            if (digits == 0 || value > 255) return false;
            ++octets;
            digits = 0;
            value  = 0;
        } else {
            return false;
        }
    }
    if (octets != 4) return false;

    int p = def_prefix;
    if (slash != std::string::npos) {
        const std::string ps = cidr.substr(slash + 1);
        if (ps.empty() || ps.size() > 2) return false;
        p = 0;
        for (char c : ps) {
            if (c < '0' || c > '9') return false;
            p = p * 10 + (c - '0');
        }
        if (p > 32) return false;
    }
    if (addr) *addr = a;
    if (prefix) *prefix = p;
    return true;
}

bool valid_endpoint(const std::string& endpoint)
{
    const size_t colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;
    if (!split_cidr(endpoint.substr(0, colon), nullptr, nullptr)) return false;
    const std::string port = endpoint.substr(colon + 1);
    if (port.empty() || port.size() > 5) return false;
    int v = 0;
    for (char c : port) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    return v > 0 && v <= 65535;
}

std::string prefix_to_mask(int prefix)
{
    if (prefix < 0 || prefix > 32) return {};
    const uint32_t m = (prefix == 0) ? 0u : (0xFFFFFFFFu << (32 - prefix));
    // 実際は 15 文字までだが、gcc の -Wformat-truncation は %u を 10 桁で
    // 見積もるので、それが収まる幅にしておく（ホスト CI が -Werror）。
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (unsigned)((m >> 24) & 0xFF),
                  (unsigned)((m >> 16) & 0xFF), (unsigned)((m >> 8) & 0xFF), (unsigned)(m & 0xFF));
    return buf;
}

const Profile* find(const Config& cfg, const std::string& name)
{
    if (name.empty()) return nullptr;
    for (const auto& p : cfg.profiles) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

Config parse(const std::string& json)
{
    Config cfg;
    if (json.size() > kMaxFileBytes) {
        cfg.error = "ファイルが大きすぎる (上限 " + std::to_string(kMaxFileBytes) + " バイト)";
        return cfg;
    }
    cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
    if (!root) {
        cfg.error = "JSON として読めない";
        return cfg;
    }
    if (!cJSON_IsObject(root)) {
        cfg.error = "最上位がオブジェクトではない";
        cJSON_Delete(root);
        return cfg;
    }
    const int version = get_int(root, "version", 0);
    if (version != 1) {
        // **知らない版は読まない。** 意味が変わっているかもしれないものを
        // 推測で解釈すると、間違った先に繋いだことに気づけない。
        cfg.error = "version が 1 ではない (" + std::to_string(version) + ")";
        cJSON_Delete(root);
        return cfg;
    }
    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "profiles");
    if (!arr || !cJSON_IsArray(arr)) {
        cfg.error = "profiles が配列ではない";
        cJSON_Delete(root);
        return cfg;
    }

    const cJSON* item = nullptr;
    int          index = -1;
    cJSON_ArrayForEach(item, arr)
    {
        ++index;
        if (cfg.profiles.size() >= kMaxProfiles) {
            cfg.error = "プロファイルが多すぎる (上限 " + std::to_string(kMaxProfiles) + " 件)";
            cfg.profiles.clear();
            cJSON_Delete(root);
            return cfg;
        }
        const std::string where = "profiles[" + std::to_string(index) + "]";
        if (!cJSON_IsObject(item)) {
            warn(&cfg, where + ": オブジェクトではないので飛ばした");
            continue;
        }
        Profile p;
        p.name = get_string(item, "name");
        if (p.name.empty() || p.name.size() > 40) {
            warn(&cfg, where + ": name が無い（または長すぎる）ので飛ばした");
            continue;
        }
        // **重複だけは全体を失敗させる。** どちらに繋がったのか分からなくなる。
        if (find(cfg, p.name)) {
            cfg.error = "name が重複している: \"" + p.name + "\"";
            cfg.profiles.clear();
            cJSON_Delete(root);
            return cfg;
        }

        const std::string type = get_string(item, "type");
        if (type == "ssh") {
            p.type = Type::kSsh;
        } else if (type == "wireguard") {
            p.type = Type::kWireGuard;
        } else if (type == "tailscale") {
            p.type = Type::kTailscale;
        } else {
            warn(&cfg, where + " \"" + p.name + "\": 未知の type \"" + type + "\" なので飛ばした");
            continue;
        }

        bool ok = true;
        switch (p.type) {
            case Type::kSsh: {
                p.host = get_string(item, "host");
                p.user = get_string(item, "user");
                p.via  = get_string(item, "via");
                p.port = static_cast<uint16_t>(get_int(item, "port", 22));
                const std::string auth = get_string(item, "auth");
                p.password = get_string(item, "password");
                p.key      = get_string(item, "key");
                if (p.host.empty() || p.user.empty()) {
                    warn(&cfg, where + " \"" + p.name + "\": host か user が無いので飛ばした");
                    ok = false;
                    break;
                }
                if (get_int(item, "port", 22) <= 0 || get_int(item, "port", 22) > 65535) {
                    warn(&cfg, where + " \"" + p.name + "\": port が範囲外なので飛ばした");
                    ok = false;
                    break;
                }
                // **key の検査は auth に関係なくやる。** password 認証の項目にも
                // key を書けてしまうので、`else if` にすると
                // "auth":"password" + "key":"../../evil" が素通りする。
                if (!p.key.empty() && bad_key_name(p.key)) {
                    warn(&cfg, where + " \"" + p.name +
                                   "\": key はディレクトリを含まないファイル名にする");
                    ok = false;
                    break;
                }
                if (auth == "password") {
                    // **パスワードは既定にしない。** JSON に書いてあればそれを使うが、
                    // 無ければ繋ぐときに画面から入力させる（SD は抜けば誰でも読める）。
                    p.ask_password = p.password.empty();
                }
                break;
            }
            case Type::kWireGuard: {
                p.address     = get_string(item, "address");
                p.private_key = get_string(item, "private_key");
                const cJSON* peer = cJSON_GetObjectItemCaseSensitive(item, "peer");
                if (peer && cJSON_IsObject(peer)) {
                    p.peer.pubkey      = get_string(peer, "pubkey");
                    p.peer.endpoint    = get_string(peer, "endpoint");
                    p.peer.allowed_ips = get_string_array(peer, "allowed_ips");
                }
                if (!split_cidr(p.address, nullptr, nullptr)) {
                    warn(&cfg, where + " \"" + p.name + "\": address が IPv4/CIDR ではない");
                    ok = false;
                    break;
                }
                if (bad_key_name(p.private_key)) {
                    warn(&cfg, where + " \"" + p.name +
                                   "\": private_key はディレクトリを含まないファイル名にする");
                    ok = false;
                    break;
                }
                if (p.peer.pubkey.empty() || p.peer.endpoint.empty()) {
                    warn(&cfg, where + " \"" + p.name + "\": peer の pubkey / endpoint が無い");
                    ok = false;
                    break;
                }
                if (!valid_endpoint(p.peer.endpoint)) {
                    warn(&cfg, where + " \"" + p.name +
                                   "\": peer.endpoint は <IPv4>:<port> にする（名前は引けない）: " +
                                   p.peer.endpoint);
                    ok = false;
                    break;
                }
                // **/0 は受けない。** netif のマスクが 0.0.0.0 になると lwIP の
                // ip4_route が何にでも一致し、ピアへ送る暗号化 UDP まで
                // トンネルに吸い込まれて自分の首を絞める。
                for (const auto& a : p.peer.allowed_ips) {
                    std::string ignored;
                    int         pfx = 32;
                    if (!split_cidr(a, &ignored, &pfx)) {
                        warn(&cfg, where + " \"" + p.name + "\": allowed_ips が IPv4/CIDR ではない: " + a);
                        ok = false;
                        break;
                    }
                    if (pfx == 0) {
                        warn(&cfg, where + " \"" + p.name +
                                       "\": allowed_ips に 0.0.0.0/0 は書けない"
                                       "（netif の経路は 1 本しか持てないので全部を飲み込む）");
                        ok = false;
                        break;
                    }
                }
                break;
            }
            case Type::kTailscale: {
                p.control = get_string(item, "control");
                p.authkey = get_string(item, "authkey");
                const int port = get_int(item, "port", 80);
                if (port <= 0 || port > 65535) {
                    warn(&cfg, where + " \"" + p.name + "\": port が範囲外なので飛ばした");
                    ok = false;
                    break;
                }
                p.port = static_cast<uint16_t>(port);
                if (p.control.empty()) {
                    warn(&cfg, where + " \"" + p.name + "\": control が無い");
                    ok = false;
                    break;
                }
                if (bad_key_name(p.authkey)) {
                    warn(&cfg, where + " \"" + p.name +
                                   "\": authkey はディレクトリを含まないファイル名にする");
                    ok = false;
                }
                break;
            }
        }
        if (ok) cfg.profiles.push_back(std::move(p));
    }
    cJSON_Delete(root);

    // via の参照先はここで見ておく。**全体は失敗させない** — その 1 件は
    // VPN 抜きなら繋げるし、他のプロファイルは無関係。
    for (const auto& p : cfg.profiles) {
        if (p.via.empty()) continue;
        const Profile* target = find(cfg, p.via);
        if (!target) {
            warn(&cfg, "\"" + p.name + "\": via \"" + p.via + "\" が見つからない");
        } else if (target->type == Type::kSsh) {
            warn(&cfg, "\"" + p.name + "\": via \"" + p.via + "\" は VPN ではない");
        }
    }
    return cfg;
}

}  // namespace prof
