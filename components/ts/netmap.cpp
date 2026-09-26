#include <cstdint>
#include "netmap.hpp"

#include <cstring>

#include <cJSON.h>

namespace ts {
namespace {

std::string get_string(const cJSON* obj, const char* key)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : std::string();
}

bool get_bool(const cJSON* obj, const char* key, bool def = false)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!v) return def;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) != 0;
    return def;
}

int get_int(const cJSON* obj, const char* key, int def = 0)
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

Peer parse_peer(const cJSON* obj)
{
    Peer p;
    p.id        = static_cast<int64_t>(get_int(obj, "ID"));
    p.name      = get_string(obj, "Name");
    p.node_key  = get_string(obj, "Key");
    p.disco_key = get_string(obj, "DiscoKey");
    p.endpoints = get_string_array(obj, "Endpoints");
    p.home_derp = get_int(obj, "HomeDERP");
    p.online    = get_bool(obj, "Online");

    // capver 112 以降は AllowedIPs が省略されることがあり、その場合は Addresses と同義。
    p.allowed_ips = get_string_array(obj, "AllowedIPs");
    if (p.allowed_ips.empty()) p.allowed_ips = get_string_array(obj, "Addresses");
    return p;
}

std::vector<Peer> parse_peer_array(const cJSON* root, const char* key, bool* present)
{
    std::vector<Peer> out;
    const cJSON*      arr = cJSON_GetObjectItemCaseSensitive(root, key);
    if (present) *present = (arr != nullptr);
    if (!arr || !cJSON_IsArray(arr)) return out;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, arr)
    {
        if (cJSON_IsObject(item)) out.push_back(parse_peer(item));
    }
    return out;
}

}  // namespace

bool parse_netmap(const std::string& json, NetMap* out)
{
    if (!out) return false;
    *out = NetMap{};

    cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
    if (!root) return false;

    struct Guard {
        cJSON* p;
        ~Guard() { cJSON_Delete(p); }
    } guard{root};

    if (!cJSON_IsObject(root)) return false;

    // KeepAlive のときは他のフィールドを見てはいけない（仕様）。
    if (get_bool(root, "KeepAlive")) {
        out->keepalive = true;
        return true;
    }

    out->domain = get_string(root, "Domain");

    if (const cJSON* node = cJSON_GetObjectItemCaseSensitive(root, "Node");
        node && cJSON_IsObject(node)) {
        out->node_key  = get_string(node, "Key");
        out->disco_key = get_string(node, "DiscoKey");
        out->addresses = get_string_array(node, "Addresses");
    }

    out->peers         = parse_peer_array(root, "Peers", &out->has_peers);
    out->peers_changed = parse_peer_array(root, "PeersChanged", nullptr);

    if (const cJSON* removed = cJSON_GetObjectItemCaseSensitive(root, "PeersRemoved");
        removed && cJSON_IsArray(removed)) {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, removed)
        {
            if (cJSON_IsNumber(item)) {
                out->peers_removed.push_back(static_cast<int64_t>(item->valuedouble));
            }
        }
    }
    return true;
}

namespace {

// "192.168.0.5" をネットワークバイトオーダの uint32 にする。
// 解析できなければ false。lwIP に依存させたくないので自分で書く
// （ホストでテストするため）。
bool parse_ipv4(const std::string& s, uint32_t* out)
{
    uint32_t octets[4] = {};
    size_t   pos       = 0;
    for (int i = 0; i < 4; ++i) {
        if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') return false;
        uint32_t v     = 0;
        int      digits = 0;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            v = v * 10 + static_cast<uint32_t>(s[pos] - '0');
            ++pos;
            if (++digits > 3 || v > 255) return false;
        }
        octets[i] = v;
        if (i < 3) {
            if (pos >= s.size() || s[pos] != '.') return false;
            ++pos;
        }
    }
    if (pos != s.size()) return false;  // 末尾にごみがあるものは弾く
    // ネットワークバイトオーダ = 最初の octet が下位バイト（lwIP の並び）。
    *out = octets[0] | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
    return true;
}

// "192.168.0.5:41641" からホスト部分を取る。IPv6 は取れなくて良い
// （IPv4 として解析できないものは呼び出し側が捨てる）。
bool host_part(const std::string& ep, std::string* out)
{
    const size_t colon = ep.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;
    *out = ep.substr(0, colon);
    return true;
}

}  // namespace

std::string pick_endpoint(const std::vector<std::string>& endpoints, uint32_t my_addr,
                          uint32_t my_mask)
{
    std::string first_v4;
    for (const auto& ep : endpoints) {
        std::string host;
        uint32_t    addr = 0;
        if (!host_part(ep, &host) || !parse_ipv4(host, &addr)) continue;  // IPv6 などは飛ばす
        if (my_mask != 0 && my_addr != 0 && (addr & my_mask) == (my_addr & my_mask)) return ep;
        if (first_v4.empty()) first_v4 = ep;
    }
    return first_v4;
}

void apply_netmap(std::vector<Peer>* table, const NetMap& map)
{
    if (!table || map.keepalive) return;
    if (map.has_peers) *table = map.peers;
    for (const auto& changed : map.peers_changed) {
        bool replaced = false;
        for (auto& p : *table) {
            if (p.id == changed.id) {
                p        = changed;
                replaced = true;
                break;
            }
        }
        if (!replaced) table->push_back(changed);
    }
    for (int64_t id : map.peers_removed) {
        for (auto it = table->begin(); it != table->end(); ++it) {
            if (it->id == id) {
                table->erase(it);
                break;
            }
        }
    }
}

namespace {

std::string dns_fold(std::string s)
{
    while (!s.empty() && s.back() == '.') s.pop_back();
    for (auto& ch : s) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return s;
}

}  // namespace

std::string peer_ipv4(const Peer& peer)
{
    for (const auto& a : peer.allowed_ips) {
        const size_t slash = a.find('/');
        if (slash == std::string::npos || a.compare(slash, std::string::npos, "/32") != 0) continue;
        uint32_t          v4 = 0;
        const std::string ip = a.substr(0, slash);
        if (parse_ipv4(ip, &v4)) return ip;
    }
    return {};
}

const Peer* find_peer(const std::vector<Peer>& peers, const std::string& host)
{
    const std::string want = dns_fold(host);
    if (want.empty()) return nullptr;
    const bool short_name = want.find('.') == std::string::npos;
    uint32_t   v4         = 0;
    const bool is_ip      = parse_ipv4(want, &v4);
    for (const auto& p : peers) {
        if (is_ip) {
            if (peer_ipv4(p) == want) return &p;
            continue;
        }
        const std::string name = dns_fold(p.name);
        if (name == want) return &p;
        if (short_name && name.compare(0, want.size(), want) == 0 && name.size() > want.size() &&
            name[want.size()] == '.') {
            return &p;
        }
    }
    return nullptr;
}

}  // namespace ts
