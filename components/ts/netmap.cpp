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

}  // namespace ts
