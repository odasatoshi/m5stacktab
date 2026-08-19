#include "skk_dict.hpp"

#include <cstring>

namespace ime {
namespace {

constexpr size_t kHeaderSize = 16;

uint32_t read_u32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

bool SkkDict::open(const void* data, size_t len)
{
    index_ = nullptr;
    if (!data || len < kHeaderSize) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    if (std::memcmp(p, "SKKD", 4) != 0) return false;
    if (read_u32(p + 4) != 1) return false;

    const uint32_t count   = read_u32(p + 8);
    const uint32_t strings = read_u32(p + 12);
    // インデックスと文字列領域がバッファ内に収まっているか。壊れた辞書で暴走させない。
    if (strings < kHeaderSize + static_cast<uint64_t>(count) * 4 || strings > len) return false;
    if (count == 0) return false;

    base_  = p;
    size_  = len;
    count_ = count;
    index_ = reinterpret_cast<const uint32_t*>(p + kHeaderSize);
    return true;
}

const char* SkkDict::key_at(uint32_t i) const
{
    const uint32_t off = index_[i];
    if (off >= size_) return nullptr;
    return reinterpret_cast<const char*>(base_ + off);
}

bool SkkDict::lookup(const std::string& yomi, std::vector<std::string>& out) const
{
    out.clear();
    if (!index_ || yomi.empty()) return false;

    // キーはバイト順にソートされているので二分探索する。
    uint32_t lo = 0, hi = count_;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const char*    key = key_at(mid);
        if (!key) return false;
        const int cmp = std::strcmp(key, yomi.c_str());
        if (cmp == 0) {
            // キーの直後（NUL の次）が候補列。
            const char* val = key + std::strlen(key) + 1;
            if (reinterpret_cast<const uint8_t*>(val) >= base_ + size_) return false;
            // "候補1/候補2" を分解する。";" 以降は注釈なので落とす。
            const char* p = val;
            while (*p) {
                const char* slash = std::strchr(p, '/');
                std::string cand(p, slash ? static_cast<size_t>(slash - p) : std::strlen(p));
                const size_t semi = cand.find(';');
                if (semi != std::string::npos) cand.resize(semi);
                if (!cand.empty()) out.push_back(cand);
                if (!slash) break;
                p = slash + 1;
            }
            return !out.empty();
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

}  // namespace ime
