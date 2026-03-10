// Part of SimCoupe - A SAM Coupe emulator
//
// Video.cpp: ESP32 video output via LT8912B HDMI bridge (sim_display component)
//
// Display: 512×480 RGB888 (pclk=30MHz, ~71Hz refresh).
// SAM framebuffer: visiblearea=0 → 512×192 pixels, 1 byte/pixel (palette index).
// Scaling: 1×H, 2×V → 512×384 image centred vertically in 512×480.
// Padding: 0px left/right (exact fit), 48px black top/bottom.
//
// With DST_W=512 = SAM_W, there is NO horizontal padding.
// DST_STRIDE = 512×3 = 1536 bytes = exactly one expanded SAM row.
// Two vertically-doubled rows (dy0, dy1) are contiguous in the framebuffer,
// so a single memcpy of 3072 bytes replaces two separate 1536-byte copies.
//
// pGuiScreen (OSD): 512×384 — rendered 1:1 starting at row 48.
//
// Framebuffer bandwidth: 512×480×3 = 737,280 bytes (vs 921,600 at 640×480).

#include "SimCoupe.h"
#include "Video.h"
#include "Frame.h"
#include "SAMIO.h"

#include "sim_display.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include <string.h>

static const char* TAG = "video";

// ── Display geometry ──────────────────────────────────────────────────────────
// Must match sdkconfig.defaults CONFIG_SIM_DISPLAY_HACT/VACT.
static constexpr int DST_W      = 512;
static constexpr int DST_H      = 480;
static constexpr int DST_STRIDE = DST_W * 3;   // 1536 bytes per row

// SAM active area: 512×192 → 2×V → 512×384, centred in 512×480
static constexpr int SAM_W      = 512;
static constexpr int SAM_H      = 192;
static constexpr int SCALED_H   = SAM_H * 2;   // 384
static constexpr int OFF_Y      = (DST_H - SCALED_H) / 2;  // 48

// ── ESP32Video: IVideoBase implementation ────────────────────────────────────

class ESP32Video final : public IVideoBase
{
public:
    bool Init() override;
    Rect DisplayRect() const override;
    void ResizeWindow(int height) const override { /* fixed display */ }
    std::pair<int, int> MouseRelative() override { return {0, 0}; }
    void OptionsChanged() override { BuildPalette(); }
    void Update(const FrameBuffer& fb) override;

private:
    void BuildPalette();

    // RGB888 palette: 128 entries as packed structs for sequential access
    struct PaletteEntry { uint8_t r, g, b; };
    PaletteEntry m_palette[128]{};

    // DRAM row buffer: palette expand writes here, then 2×memcpy to PSRAM.
    // With DST_W=512=SAM_W, dy0 and dy1 are contiguous in PSRAM, but we still
    // use two separate memcpy(1536) calls — one per row — which is faster than
    // duplicating into a 3072-byte buffer and doing one memcpy(3072).
    alignas(4) uint8_t m_row_buf[DST_STRIDE];  // one expanded row (1536 bytes)

    bool m_initialized = false;
};

// ── Video namespace (called by SimCoupe core) ─────────────────────────────────

static std::unique_ptr<IVideoBase> s_video;

namespace Video
{

bool Init()
{
    Exit();
    auto v = std::make_unique<ESP32Video>();
    if (!v->Init())
        return false;
    s_video = std::move(v);
    return true;
}

void Exit()
{
    s_video.reset();
}

void NativeToSam(int& x, int& y)
{
    // No horizontal offset (DST_W == SAM_W); undo 2×V scaling and top pad
    y = (y - OFF_Y) / 2;
}

void ResizeWindow(int /*height*/) { /* fixed */ }

Rect DisplayRect()
{
    if (s_video)
        return s_video->DisplayRect();
    return {0, 0, DST_W, DST_H};
}

std::pair<int, int> MouseRelative()
{
    if (s_video)
        return s_video->MouseRelative();
    return {0, 0};
}

void OptionsChanged()
{
    if (s_video)
        s_video->OptionsChanged();
}

void Update(const FrameBuffer& fb)
{
    if (s_video)
        s_video->Update(fb);
}

} // namespace Video

// ── UI::CreateVideo (called from UI.cpp) ──────────────────────────────────────

#include "UI.h"

std::unique_ptr<IVideoBase> UI::CreateVideo()
{
    auto v = std::make_unique<ESP32Video>();
    if (v->Init())
        return v;
    return nullptr;
}

// ── ESP32Video implementation ─────────────────────────────────────────────────

bool ESP32Video::Init()
{
    void* fb0 = nullptr;
    void* fb1 = nullptr;
    if (sim_display_get_framebuffer(&fb0, &fb1) != ESP_OK || !fb0)
    {
        ESP_LOGE(TAG, "sim_display not initialized");
        return false;
    }

    // Zero both framebuffers once — top/bottom padding rows stay black forever.
    // Update() only writes the active area [OFF_Y .. OFF_Y+SCALED_H).
    size_t fb_size = DST_W * DST_H * 3;
    memset(fb0, 0, fb_size);
    memset(fb1, 0, fb_size);
    esp_cache_msync(fb0, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_cache_msync(fb1, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    BuildPalette();
    m_initialized = true;
    ESP_LOGI(TAG, "ESP32Video: %dx%d display, SAM %dx%d -> 1xH 2xV -> pad top/bot %dpx",
             DST_W, DST_H, SAM_W, SAM_H, OFF_Y);
    return true;
}

Rect ESP32Video::DisplayRect() const
{
    return {0, 0, DST_W, DST_H};
}

void ESP32Video::BuildPalette()
{
    auto palette = IO::Palette();
    for (size_t i = 0; i < palette.size() && i < 128; ++i)
    {
        m_palette[i].r = palette[i].red;
        m_palette[i].g = palette[i].green;
        m_palette[i].b = palette[i].blue;
    }
}

void ESP32Video::Update(const FrameBuffer& fb)
{
    if (!m_initialized)
        return;

    // ── Video timing diagnostic (5 frames after frame 400) ───────────────────
    static int  s_vdiag_skip  = 400;
    static int  s_vdiag_count = 0;
    int64_t vt0 = 0, vt1 = 0, vt2 = 0;
    int64_t s_expand_us = 0, s_copy_us = 0;
    bool vdiag = (s_vdiag_skip == 0 && s_vdiag_count < 5);
    if (s_vdiag_skip > 0) s_vdiag_skip--;
    if (vdiag) vt0 = esp_timer_get_time();

    uint8_t* dst = static_cast<uint8_t*>(sim_display_get_back_buffer());
    if (!dst)
        return;

    const int src_w = fb.Width();   // 512 (normal) or 512 (GUI)
    const int src_h = fb.Height();  // 192 (normal) or 384 (GUI)

    // GUI screen (512×384): rendered 1:1 starting at row OFF_Y.
    // Normal framebuffer (512×192): 2× vertical scaling.
    const bool is_gui = (src_h > SAM_H);

    if (is_gui)
    {
        // GUI: 1 source row → 1 display row, direct palette expand to PSRAM.
        // src_h=384, fits exactly in [OFF_Y .. OFF_Y+384) = [48..432).
        const int rows = (src_h <= DST_H - OFF_Y) ? src_h : DST_H - OFF_Y;
        for (int sy = 0; sy < rows; ++sy)
        {
            const uint8_t* src_line = fb.GetLine(sy);
            uint8_t* p = dst + (OFF_Y + sy) * DST_STRIDE;
            for (int sx = 0; sx < src_w; ++sx)
            {
                const PaletteEntry& e = m_palette[src_line[sx] & 0x7F];
                *p++ = e.r; *p++ = e.g; *p++ = e.b;
            }
        }
        // Flush active GUI area
        sim_display_flush_region((size_t)OFF_Y * DST_STRIDE,
                                 (size_t)rows * DST_STRIDE);
    }
    else
    {
        // Normal framebuffer: 1 source row → 2 consecutive display rows.
        //
        // With DST_W=512=SAM_W, DST_STRIDE=1536 = exactly one expanded row.
        // dy0 and dy1 are contiguous in PSRAM (no horizontal padding gap).
        // We expand to a 1536-byte DRAM row_buf, then copy to both PSRAM rows.
        // Two separate memcpy(1536) calls are used — benchmarking showed this
        // is faster than duplicating row_buf and doing one memcpy(3072).

        for (int sy = 0; sy < src_h; ++sy)
        {
            const uint8_t* src_line = fb.GetLine(sy);

            // Expand one SAM row into DRAM row_buf
            if (vdiag) s_expand_us -= esp_timer_get_time();
            uint8_t* p = m_row_buf;
            for (int sx = 0; sx < src_w; ++sx)
            {
                const PaletteEntry& e = m_palette[src_line[sx] & 0x7F];
                *p++ = e.r; *p++ = e.g; *p++ = e.b;
            }
            if (vdiag) s_expand_us += esp_timer_get_time();

            // Copy to both consecutive PSRAM rows (dy0 and dy1 = dy0+1).
            // With DST_W=512=SAM_W, dy0 and dy1 are contiguous in PSRAM
            // (no horizontal padding), but two separate memcpy(1536) calls
            // are faster than duplicating row_buf and doing one memcpy(3072).
            if (vdiag) s_copy_us -= esp_timer_get_time();
            int dy0 = OFF_Y + sy * 2;
            memcpy(dst + dy0 * DST_STRIDE,         m_row_buf, DST_STRIDE);
            memcpy(dst + (dy0 + 1) * DST_STRIDE,   m_row_buf, DST_STRIDE);
            if (vdiag) s_copy_us += esp_timer_get_time();
        }

        // Flush active area: rows [OFF_Y .. OFF_Y+SCALED_H) = [48..432)
        // = 384 rows × 1536 bytes = 589,824 bytes
        if (vdiag) vt1 = esp_timer_get_time();
        sim_display_flush_region((size_t)OFF_Y * DST_STRIDE,
                                 (size_t)SCALED_H * DST_STRIDE);
        if (vdiag) {
            vt2 = esp_timer_get_time();
            ESP_LOGI(TAG, "video frame %d: expand=%lld us  copy2psram=%lld us  flush=%lld us  total=%lld us",
                     s_vdiag_count,
                     (long long)s_expand_us,
                     (long long)s_copy_us,
                     (long long)(vt2 - vt1),
                     (long long)(vt2 - vt0));
            s_vdiag_count++;
        }
    }
}
