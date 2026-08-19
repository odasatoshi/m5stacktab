#pragma once
// SKK 辞書の検索。tools/build_dict.py が作るバイナリを読むだけなので、
// ESP-IDF にも実機にも依存しない（ホストでテストできる）。
//
// バイナリ形式（すべてリトルエンディアン）:
//   magic   char[4]  "SKKD"
//   version uint32   1
//   count   uint32   エントリ数
//   strings uint32   文字列領域の先頭オフセット
//   index   uint32[count]  各エントリのキー開始オフセット（キーのバイト順でソート済み）
//   strings "よみ\0候補1/候補2\0よみ\0候補1\0..."
//
// フラッシュに置いて esp_partition_mmap でそのまま読める形にしてある（RAM に展開しない）。
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ime {

class SkkDict {
public:
    // data は寿命が呼び出し側の管理下にあるバッファ（mmap 領域など）。コピーしない。
    bool open(const void* data, size_t len);
    bool is_open() const { return index_ != nullptr; }

    // よみ（UTF-8 ひらがな）に対する候補を返す。見つからなければ false。
    bool lookup(const std::string& yomi, std::vector<std::string>& out) const;

    uint32_t count() const { return count_; }

private:
    const char*     key_at(uint32_t i) const;

    const uint8_t*  base_    = nullptr;
    size_t          size_    = 0;
    const uint32_t* index_   = nullptr;
    uint32_t        count_   = 0;
};

}  // namespace ime
