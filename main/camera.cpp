#include "camera.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <esp_timer.h>

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_video_init.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>

namespace cam {
namespace {

const char* TAG = "cam";

// MIPI-CSI の V4L2 デバイス。esp_video は /dev/video0 に出す。
constexpr const char* kDevice = "/dev/video0";

// **2 枚要る。** MIPI-CSI のドライバはバックアップバッファを持たない設定なので、
// 1 枚だと DMA の差し替え先が無い。
constexpr uint32_t kBufferCount = 2;

bool s_ready = false;
int  s_fd    = -1;

}  // namespace

std::string fourcc(uint32_t v)
{
    // **表示用。** 未知の値でも落とさず、そのまま 4 文字で見せる（切り分けに要る）。
    char b[5] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                 static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF), '\0'};
    for (int i = 0; i < 4; ++i) {
        if (b[i] < 0x20 || b[i] > 0x7E) b[i] = '?';
    }
    return b;
}

bool ready() { return s_ready; }

esp_err_t init()
{
    if (s_ready) return ESP_OK;

    // **SCCB のバスは自分で作らず、M5GFX が持っているものを借りる。**
    // センサは G31/G32 = M5GFX が I2C_NUM_1 で握っているバスにぶら下がっている。
    // `init_sccb = true` で作らせると `I2C bus id(1) has already been acquired` で
    // 落ちる（実機で確認）。別のポートで同じピンに master bus を作る手もあるが、
    // GPIO マトリクスの出力選択を奪い合う（CLAUDE.md）ので取らない。
    //
    // `i2c_master_get_bus_handle()` は**既に取得済みのポートのハンドルを返す**ので、
    // 所有権は M5GFX のまま、esp_video には使わせるだけにできる。
    i2c_master_bus_handle_t bus = nullptr;
    if (esp_err_t err = i2c_master_get_bus_handle(1, &bus); err != ESP_OK || !bus) {
        // **display.init() より前に呼ぶとここに来る。** M5GFX がバスを作る前なので
        // 借りる相手が居ない。
        ESP_LOGE(TAG, "I2C_NUM_1 のハンドルを取れない: %s (display.init() は済んでいるか)",
                 esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }
    esp_video_init_csi_config_t csi = {};
    csi.sccb_config.init_sccb       = false;
    csi.sccb_config.i2c_handle      = bus;
    // **100kHz にしておく。** M5GFX がタッチ (GT911) を読む速度と揃える。
    csi.sccb_config.freq            = 100000;
    // Tab5 はリセットも PWDN も配線が無い。電源は display.init() が入れている。
    csi.reset_pin                   = GPIO_NUM_NC;
    csi.pwdn_pin                    = GPIO_NUM_NC;
    // **LDO は自分で取らせる。** CSI PHY は chan3 / 2500mV で、M5GFX の DSI と
    // 同じ設定なので共有できるはず。取り合いで落ちるなら dont_init_ldo を立てる。
    csi.dont_init_ldo               = false;

    esp_video_init_config_t cfg = {};
    cfg.csi                     = &csi;

    if (esp_err_t err = esp_video_init(&cfg); err != ESP_OK) {
        // **理由をそのまま出す。** ここで落ちる原因は「SCCB が 0x36 に届かない」
        // 「LDO が取れない」「センサの ID が合わない」のどれかで、
        // esp_video / esp_cam_sensor 側のログに出ている。
        ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_fd = open(kDevice, O_RDWR);
    if (s_fd < 0) {
        ESP_LOGE(TAG, "open(%s) failed: errno=%d", kDevice, errno);
        return ESP_ERR_NOT_FOUND;
    }
    s_ready = true;
    ESP_LOGI(TAG, "camera ready (%s)", kDevice);
    return ESP_OK;
}

esp_err_t probe(Info* out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    v4l2_capability cap = {};
    if (ioctl(s_fd, VIDIOC_QUERYCAP, &cap) != 0) {
        ESP_LOGE(TAG, "VIDIOC_QUERYCAP failed: errno=%d", errno);
        return ESP_FAIL;
    }
    out->driver = reinterpret_cast<const char*>(cap.driver);
    out->card   = reinterpret_cast<const char*>(cap.card);

    v4l2_format fmt = {};
    fmt.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed: errno=%d", errno);
        return ESP_FAIL;
    }
    out->width        = fmt.fmt.pix.width;
    out->height       = fmt.fmt.pix.height;
    out->pixelformat  = fmt.fmt.pix.pixelformat;
    out->bytesperline = fmt.fmt.pix.bytesperline;
    out->sizeimage    = fmt.fmt.pix.sizeimage;
    return ESP_OK;
}

esp_err_t capture(Frame* out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    // **バッファは 2 枚要る。** MIPI-CSI のドライバはバックアップバッファを持たない
    // 設定 (`..._BACKUP_BUFFER` が既定 n) なので、1 枚だと DMA の差し替え先が無い。
    v4l2_requestbuffers req = {};
    req.count               = kBufferCount;
    req.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory              = V4L2_MEMORY_MMAP;
    if (ioctl(s_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed: errno=%d", errno);
        return ESP_FAIL;
    }

    void*  mapped[kBufferCount]     = {};
    size_t mapped_len[kBufferCount] = {};
    bool   streaming                = false;
    int    type                     = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    // **後始末は 1 か所にまとめる。** 途中で失敗したときに unmap と REQBUFS(0) を
    // 忘れると、**1 回の失敗ごとに 3.6MB の PSRAM が消える**（成功経路だけ直しても
    // 気づけない。失敗するのはたいてい繰り返し試しているときなので、なお悪い）。
    auto cleanup = [&]() {
        if (streaming) ioctl(s_fd, VIDIOC_STREAMOFF, &type);
        for (uint32_t i = 0; i < kBufferCount; ++i) {
            if (mapped[i] && mapped[i] != MAP_FAILED) munmap(mapped[i], mapped_len[i]);
        }
        // 1280x720 の RGB565 が 2 枚で 3.6MB。返さないと握ったままになる
        // （実機で 30.2MB → 26.5MB のまま戻らないのを確認した）。
        v4l2_requestbuffers rel = {};
        rel.count               = 0;
        rel.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        rel.memory              = V4L2_MEMORY_MMAP;
        if (ioctl(s_fd, VIDIOC_REQBUFS, &rel) != 0) {
            ESP_LOGW(TAG, "バッファを返せない: errno=%d (PSRAM を握ったままになる)", errno);
        }
    };

    for (uint32_t i = 0; i < req.count && i < kBufferCount; ++i) {
        v4l2_buffer b = {};
        b.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory      = V4L2_MEMORY_MMAP;
        b.index       = i;
        if (ioctl(s_fd, VIDIOC_QUERYBUF, &b) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF(%u) failed: errno=%d", (unsigned)i, errno);
            cleanup();
            return ESP_FAIL;
        }
        mapped[i] = mmap(nullptr, b.length, PROT_READ, MAP_SHARED, s_fd, b.m.offset);
        if (mapped[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap(%u) failed: errno=%d", (unsigned)i, errno);
            cleanup();
            return ESP_ERR_NO_MEM;
        }
        mapped_len[i] = b.length;
        if (ioctl(s_fd, VIDIOC_QBUF, &b) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF(%u) failed: errno=%d", (unsigned)i, errno);
            cleanup();
            return ESP_FAIL;
        }
    }

    if (ioctl(s_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed: errno=%d", errno);
        cleanup();
        return ESP_FAIL;
    }
    streaming = true;

    const int64_t t0  = esp_timer_get_time();
    v4l2_buffer   got = {};
    got.type          = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    got.memory        = V4L2_MEMORY_MMAP;
    if (ioctl(s_fd, VIDIOC_DQBUF, &got) != 0) {
        ESP_LOGE(TAG, "VIDIOC_DQBUF failed: errno=%d", errno);
        cleanup();
        return ESP_FAIL;
    }
    // **index は信用して使う前に確かめる。** mmap していない枠を返されると
    // null を舐めることになる。
    if (got.index >= kBufferCount || !mapped[got.index] || mapped[got.index] == MAP_FAILED) {
        ESP_LOGE(TAG, "DQBUF returned unmapped index %u", (unsigned)got.index);
        cleanup();
        return ESP_FAIL;
    }

    out->bytes = got.bytesused;
    out->us    = static_cast<uint32_t>(esp_timer_get_time() - t0);

    // **画が来ているかを数字で見る。** RGB565 の緑 (6bit) を 8bit に伸ばして
    // 最小・最大・平均を取る。**全部 0 のバッファでも DQBUF は成功する**ので、
    // 画面が見られない環境ではこれが「本当に撮れたか」の唯一の手がかりになる。
    const auto*  p   = static_cast<const uint8_t*>(mapped[got.index]);
    const size_t n   = got.bytesused < mapped_len[got.index] ? got.bytesused
                                                             : mapped_len[got.index];
    uint32_t     sum = 0;
    uint32_t     cnt = 0;
    uint8_t      lo  = 255;
    uint8_t      hi  = 0;
    // 全画素は舐めない（1280x720 で 92 万画素）。16 画素ごとで足りる。
    for (size_t i = 0; i + 1 < n; i += 32) {
        const uint16_t px = static_cast<uint16_t>(p[i] | (p[i + 1] << 8));
        const uint8_t  g  = static_cast<uint8_t>(((px >> 5) & 0x3F) << 2);
        lo = g < lo ? g : lo;
        hi = g > hi ? g : hi;
        sum += g;
        ++cnt;
    }
    out->min  = cnt ? lo : 0;
    out->max  = cnt ? hi : 0;
    out->mean = cnt ? static_cast<uint8_t>(sum / cnt) : 0;
    ioctl(s_fd, VIDIOC_QBUF, &got);

    cleanup();
    return ESP_OK;
}

}  // namespace cam
