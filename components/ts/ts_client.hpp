#pragma once
// ts2021 の制御プレーンに接続する状態機械。
//
// 流れ: /key で鍵取得 → TCP → POST /ts2021 → Noise 確立 → EarlyNoise →
//       HTTP/2 プリフェイス → POST /machine/register → POST /machine/map (long-poll)
//
// ソケットは BSD socket API を直接使う（lwIP でもホストでも同じ）。
// ホストで実際の Headscale に対して通ることを確認した手順をそのまま実装している。
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ts {

struct ClientConfig {
    std::string host;                       // 制御プレーンのホスト名 or IP
    uint16_t    port               = 80;    // 平文で良い（Noise が保護する）
    std::string auth_key;                   // tskey-auth-... / hskey-auth-...
    std::string hostname           = "tab5";
    uint16_t    capability_version = 131;
    std::vector<std::string> endpoints;     // 自分の WireGuard エンドポイント
};

struct ClientStatus {
    enum class State {
        kIdle,
        kFetchingKey,
        kConnecting,
        kHandshaking,
        kRegistering,
        kMapping,     // netmap を long-poll 中（正常稼働）
        kFailed,
    };
    State       state = State::kIdle;
    std::string error;
    std::string assigned_address;  // "100.64.0.2/32"
    std::string domain;
    uint32_t    map_messages = 0;
    uint32_t    keepalives   = 0;
    bool        registered   = false;
};

class Client {
public:
    // machine key と node key は呼び出し側が持つ（NVS に保存して再利用する）。
    void set_keys(const uint8_t machine_priv[32], const uint8_t node_priv[32]);
    void set_config(const ClientConfig& cfg) { cfg_ = cfg; }

    // netmap を受け取ったときに呼ばれる（生の JSON）。ピア情報の解析は呼び出し側。
    void set_map_handler(std::function<void(const std::string&)> fn) { on_map_ = std::move(fn); }

    // 1 回接続して long-poll に入る。戻るのは切断か失敗のとき。
    // 呼び出し側がリトライを制御する（バックオフを入れる）。
    bool run_once();

    // 実行中に止める。
    void stop() { stop_ = true; }

    const ClientStatus& status() const { return st_; }

private:
    bool fetch_server_key(uint8_t out[32]);

    ClientConfig cfg_;
    ClientStatus st_;
    uint8_t      machine_priv_[32] = {};
    uint8_t      node_priv_[32]    = {};
    uint8_t      node_pub_[32]     = {};
    volatile bool stop_            = false;
    std::function<void(const std::string&)> on_map_;
};

}  // namespace ts
