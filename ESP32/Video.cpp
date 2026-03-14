// Part of SimCoupe - A SAM Coupe emulator
//
// Video.cpp: ESP32 video output via LT8912B HDMI bridge (sim_display component)
//
// Display: 640×480 RGB888 (pclk=24MHz, 60Hz) — CEA-861 VIC=1 (VGA 640×480).
// SAM framebuffer: visiblearea=0 → 512×192 pixels, 1 byte/pixel (palette index).
// Scaling: 1×H, 2×V → 512×384, centred in 640×480 (OFF_X=64, OFF_Y=48).
// Aspect ratio: 4:3 exact. Small black borders on all sides.
//
// DST_STRIDE = 640×3 = 1920 bytes per display row.
// Framebuffer: 640×480×3 = 900KB (×3 less than 720p → 6× less PSRAM writes).
// m_row_buf holds one full display row (1920 bytes, DRAM):
//   - border pixels pre-zeroed at Init() — stay black permanently
//   - active area [OFF_X .. OFF_X+SAM_W): each SAM pixel written 1×
// Each dirty SAM row is expanded into m_row_buf then copied 2× into PSRAM.
//
// Dirty line tracking: each SAM row is compared with the previous frame.
// Only changed rows are expanded and written to PSRAM.
//
// pGuiScreen (OSD): 512×384 — rendered 1×H 1×V, centred in 640×480.

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
static constexpr int DST_W      = 640;
static constexpr int DST_H      = 480;
static constexpr int DST_STRIDE = DST_W * 3;   // 1920 bytes per row

// SAM active area: 512×192 → 1×H 2×V → 512×384, centred in 640×480
static constexpr int SAM_W      = 512;
static constexpr int SAM_H      = 192;
static constexpr int SCALED_W   = SAM_W;        // 512 (1×H)
static constexpr int SCALED_H   = SAM_H * 2;   // 384 (2×V)
static constexpr int OFF_X      = (DST_W - SCALED_W) / 2;  // 64
static constexpr int OFF_Y      = (DST_H - SCALED_H) / 2;  // 48

// ── ESP32Video: IVideoBase implementation ────────────────────────────────────

class ESP32Video final : public IVideoBase
{
public:
    bool Init() override;
    Rect DisplayRect() const override;
    void ResizeWindow(int height) const override { /* fixed display */ }
    std::pair<int, int> MouseRelative() override { return {0, 0}; }
    void OptionsChanged() override { BuildPalette(); memset(m_prev, 0xFF, sizeof(m_prev)); }
    void Update(const FrameBuffer& fb) override;

private:
    void BuildPalette();

    // RGB888 palette: 128 entries as packed structs for sequential access
    struct PaletteEntry { uint8_t r, g, b; };
    PaletteEntry m_palette[128]{};

    // DRAM row buffer: one full display row (1920 bytes).
    // Border pixels (left OFF_X and right OFF_X columns) zeroed at Init().
    // Active area [OFF_X*3 .. (OFF_X+SCALED_W)*3): SAM pixels written 1×.
    alignas(4) uint8_t m_row_buf[DST_W * 3];   // 1920 bytes

    // Previous frame snapshot for dirty detection (DRAM, 512×192 = 98304 bytes).
    // When a row is dirty it is written to BOTH framebuffers so they stay in
    // sync — dirty tracking is then safe with double buffering.
    alignas(4) uint8_t m_prev[SAM_W * SAM_H];

    bool m_initialized = false;
    bool m_was_gui     = false;  // true if previous frame was GUI/OSD
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
    x = (x - OFF_X);       // 1× horizontal scale
    y = (y - OFF_Y) / 2;   // 2× vertical scale
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

    // Zero framebuffer(s) — ensures clean state on first frame.
    size_t fb_size = DST_W * DST_H * 3;
    memset(fb0, 0, fb_size);
    esp_cache_msync(fb0, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (fb1) {
        memset(fb1, 0, fb_size);
        esp_cache_msync(fb1, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

    // Pre-zero m_row_buf — border pixels stay black permanently.
    memset(m_row_buf, 0, sizeof(m_row_buf));

    BuildPalette();
    memset(m_prev, 0xFF, sizeof(m_prev));
    m_initialized = true;
    ESP_LOGI(TAG, "ESP32Video: %dx%d (VGA), SAM %dx%d -> 1xH 2xV -> %dx%d centred (OFF_X=%d OFF_Y=%d)",
             DST_W, DST_H, SAM_W, SAM_H, SCALED_W, SCALED_H, OFF_X, OFF_Y);
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
        // DPI at 24MHz sends bytes in R,B,G order to LT8912B (hardware quirk).
        // Compensate by swapping G and B channels in the palette.
        m_palette[i].r = palette[i].red;
        m_palette[i].g = palette[i].blue;
        m_palette[i].b = palette[i].green;
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

    // GUI screen (512×384): rendered 1×H 1×V → 512×384, centred in 640×480.
    // Normal framebuffer (512×192): rendered 1×H 2×V → 512×384, centred in 640×480.
    const bool is_gui = (src_h > SAM_H);

    // Detect GUI→normal transition: clear both framebuffers to black and
    // invalidate m_prev so dirty-line tracking forces a full repaint.
    // The OSD occupies gui_off_y=30..449 but the game area starts at OFF_Y=48,
    // so rows 30-47 would otherwise retain OSD pixels indefinitely.
    if (!is_gui && m_was_gui) {
        void* fb0 = nullptr;
        void* fb1 = nullptr;
        sim_display_get_framebuffer(&fb0, &fb1);
        size_t fb_size = DST_W * DST_H * 3;
        if (fb0) { memset(fb0, 0, fb_size); esp_cache_msync(fb0, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M); }
        if (fb1) { memset(fb1, 0, fb_size); esp_cache_msync(fb1, fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M); }
        memset(m_prev, 0xFF, sizeof(m_prev));
    }
    m_was_gui = is_gui;

    if (is_gui)
    {
        // GUI: src_w x src_h -> 1xH 1xV, centred in 640x480.
        // src_w=560, src_h=420 -> gui_off_x=40, gui_off_y=30.
        const int gui_off_x = (src_w < DST_W) ? (DST_W - src_w) / 2 : 0;
        const int gui_off_y = (src_h < DST_H) ? (DST_H - src_h) / 2 : 0;
        const int gui_w = (src_w <= DST_W) ? src_w : DST_W;
        const int rows  = (src_h <= DST_H - gui_off_y) ? src_h : DST_H - gui_off_y;
        for (int sy = 0; sy < rows; ++sy)
        {
            const uint8_t* src_line = fb.GetLine(sy);
            uint8_t* p = m_row_buf + gui_off_x * 3;
            for (int sx = 0; sx < gui_w; ++sx) {
                const PaletteEntry& e = m_palette[src_line[sx] & 0x7F];
                *p++ = e.r; *p++ = e.g; *p++ = e.b;
            }
            memcpy(dst + (gui_off_y + sy) * DST_STRIDE, m_row_buf, DST_STRIDE);
        }
        sim_display_flush_region((size_t)gui_off_y * DST_STRIDE,
                                 (size_t)rows * DST_STRIDE);
        sim_display_swap();
    }
    else
    {
        // Normal: 512×192 → 1×H 2×V → 512×384, centred (OFF_X=64, OFF_Y=48).
        // Write dirty lines to BOTH framebuffers to keep double-buffer in sync.
        void* fb0_ptr = nullptr;
        void* fb1_ptr = nullptr;
        sim_display_get_framebuffer(&fb0_ptr, &fb1_ptr);
        uint8_t* other = (dst == (uint8_t*)fb0_ptr) ? (uint8_t*)fb1_ptr : (uint8_t*)fb0_ptr;

        int first_dirty = -1;
        int last_dirty  = -1;

        for (int sy = 0; sy < src_h; ++sy)
        {
            const uint8_t* src_line = fb.GetLine(sy);
            uint8_t* prev_line = m_prev + sy * src_w;

            if (memcmp(src_line, prev_line, src_w) == 0)
                continue;

            memcpy(prev_line, src_line, src_w);

            // Expand 1×H into active area of m_row_buf (borders stay black)
            if (vdiag) s_expand_us -= esp_timer_get_time();
            uint8_t* p = m_row_buf + OFF_X * 3;
            for (int sx = 0; sx < src_w; ++sx)
            {
                const PaletteEntry& e = m_palette[src_line[sx] & 0x7F];
                *p++ = e.r; *p++ = e.g; *p++ = e.b;
            }
            if (vdiag) s_expand_us += esp_timer_get_time();

            // Copy 2×V into both framebuffers
            if (vdiag) s_copy_us -= esp_timer_get_time();
            int dy0 = OFF_Y + sy * 2;
            memcpy(dst + dy0       * DST_STRIDE, m_row_buf, DST_STRIDE);
            memcpy(dst + (dy0 + 1) * DST_STRIDE, m_row_buf, DST_STRIDE);
            if (other) {
                memcpy(other + dy0       * DST_STRIDE, m_row_buf, DST_STRIDE);
                memcpy(other + (dy0 + 1) * DST_STRIDE, m_row_buf, DST_STRIDE);
            }
            if (vdiag) s_copy_us += esp_timer_get_time();

            if (first_dirty < 0) first_dirty = sy;
            last_dirty = sy;
        }

        if (vdiag) vt1 = esp_timer_get_time();
        if (first_dirty >= 0)
        {
            int disp_first = OFF_Y + first_dirty * 2;
            int disp_last  = OFF_Y + last_dirty  * 2 + 2;
            size_t flush_bytes = (size_t)(disp_last - disp_first) * DST_STRIDE;

            static uint32_t s_vid_frame = 0; s_vid_frame++;
            if ((s_vid_frame % 250) == 0) {
                int64_t t0d = esp_timer_get_time();
                sim_display_flush_region((size_t)disp_first * DST_STRIDE, flush_bytes);
                int64_t t1d = esp_timer_get_time();
                ESP_LOGI("vidperf", "dirty=%d/%d msync=%lldus flush_kb=%u",
                         last_dirty - first_dirty + 1, SAM_H,
                         (long long)(t1d - t0d),
                         (unsigned)(flush_bytes / 1024));
            } else {
                sim_display_flush_region((size_t)disp_first * DST_STRIDE, flush_bytes);
            }
        }
        // Swap once per frame — present back buffer regardless of dirty count.
        // (On a fully static frame with no dirty lines the display content is
        //  unchanged, but we still swap so s_back_buf stays in sync.)
        sim_display_swap();
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
