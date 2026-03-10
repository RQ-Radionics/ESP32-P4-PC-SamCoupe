// Part of SimCoupe - A SAM Coupe emulator
//
// Video.cpp: ESP32 video output via LT8912B HDMI bridge (sim_display component)
//
// Display: 640×480 RGB888 (pclk=30MHz, ~71Hz refresh).
// SAM framebuffer: visiblearea=0 → 512×192 pixels, 1 byte/pixel (palette index).
// Scaling: 1× horizontal, 2× vertical → 512×384 image centred in 640×480.
// Padding: 64px black left/right, 48px black top/bottom.
//
// pGuiScreen (OSD): 512×384 (already vertically doubled by Frame::End memcpy).
// Rendered 1:1 into the central 512×384 region of the display.
//
// Bandwidth: 640×480×3 = 921,600 bytes/frame (vs 2,359,296 at 1024×768 — 2.56× less).

#include "SimCoupe.h"
#include "Video.h"
#include "Frame.h"
#include "SAMIO.h"

#include "sim_display.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include <algorithm>
#include <string.h>

static const char* TAG = "video";

// ── Display geometry ──────────────────────────────────────────────────────────
// Must match sdkconfig.defaults CONFIG_SIM_DISPLAY_HACT/VACT.
static constexpr int DST_W      = 640;
static constexpr int DST_H      = 480;
static constexpr int DST_STRIDE = DST_W * 3;  // bytes per display row (RGB888)

// SAM active area with visiblearea=0: 512×192 pixels.
// Scaled 1×H + 2×V → 512×384. Centred in 640×480:
//   pad_x = (640 - 512) / 2 = 64 px left and right
//   pad_y = (480 - 384) / 2 = 48 px top and bottom
static constexpr int SAM_W = 512;

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

    // RGB888 palette: 128 entries × 3 bytes (R, G, B)
    uint8_t m_palette_r[128]{};
    uint8_t m_palette_g[128]{};
    uint8_t m_palette_b[128]{};

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
    // Undo centring offset, then undo 1×H / 2×V scaling
    x = x - (DST_W - SAM_W) / 2;
    y = (y - (DST_H - 192 * 2) / 2) / 2;
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

    BuildPalette();
    m_initialized = true;
    ESP_LOGI(TAG, "ESP32Video: %dx%d display, SAM %dx192 -> 1xH 2xV -> centred (pad %dx%d)",
             DST_W, DST_H, SAM_W, (DST_W - SAM_W) / 2, (DST_H - 192 * 2) / 2);
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
        m_palette_r[i] = palette[i].red;
        m_palette_g[i] = palette[i].green;
        m_palette_b[i] = palette[i].blue;
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
    bool vdiag = (s_vdiag_skip == 0 && s_vdiag_count < 5);
    if (s_vdiag_skip > 0) s_vdiag_skip--;
    if (vdiag) vt0 = esp_timer_get_time();

    uint8_t* dst = static_cast<uint8_t*>(sim_display_get_back_buffer());
    if (!dst)
        return;

    const int src_w = fb.Width();
    const int src_h = fb.Height();

    // Detect pGuiScreen vs pFrameBuffer.
    // With visiblearea=0: pFrameBuffer=512×192, pGuiScreen=512×384.
    // GUI screen height (384) > DST_H/2 (240).
    const bool is_gui = (src_h > DST_H / 2);

    // Scaled output dimensions: 1×H always; 2×V for normal fb, 1×V for GUI
    const int scaled_w = src_w;
    const int scaled_h = is_gui ? src_h : src_h * 2;

    // Centre offsets
    const int off_x = (DST_W - scaled_w) / 2;  // 64 for 512-wide
    const int off_y = (DST_H - scaled_h) / 2;  // 48 for 384-tall

    // Source column range (clip if image wider than display)
    const int src_x_start = (off_x < 0) ? -off_x : 0;
    const int src_x_end   = (off_x < 0) ? std::min(src_w, DST_W) : src_w;
    const int dst_x_start = std::max(off_x, 0);
    const int dst_x_bytes = dst_x_start * 3;
    const int render_w    = src_x_end - src_x_start;
    const int render_bytes = render_w * 3;
    const bool has_h_pad  = (off_x > 0);
    const int right_pad_off = dst_x_bytes + render_bytes;
    const int right_pad_len = DST_STRIDE - right_pad_off;

    // Clear top padding rows
    if (off_y > 0)
    {
        for (int dy = 0; dy < off_y && dy < DST_H; ++dy)
            memset(dst + dy * DST_STRIDE, 0, DST_STRIDE);
    }

    // Source row range
    const int src_y_start = (off_y < 0) ? (-off_y) / (is_gui ? 1 : 2) : 0;
    const int src_y_end   = is_gui
        ? std::min(src_h, DST_H - std::max(off_y, 0))
        : std::min(src_h, (DST_H - std::max(off_y, 0) + 1) / 2);

    for (int sy = src_y_start; sy < src_y_end; ++sy)
    {
        const uint8_t* src_line = fb.GetLine(sy) + src_x_start;

        if (is_gui)
        {
            // GUI: 1 source row → 1 display row
            int dy = off_y + sy;
            if (dy < 0 || dy >= DST_H) continue;

            uint8_t* dst_row = dst + dy * DST_STRIDE;
            if (has_h_pad)
            {
                memset(dst_row, 0, dst_x_bytes);
                if (right_pad_len > 0)
                    memset(dst_row + right_pad_off, 0, right_pad_len);
            }

            uint8_t* p = dst_row + dst_x_bytes;
            for (int sx = 0; sx < render_w; ++sx)
            {
                uint8_t idx = src_line[sx] & 0x7F;
                *p++ = m_palette_r[idx];
                *p++ = m_palette_g[idx];
                *p++ = m_palette_b[idx];
            }
        }
        else
        {
            // Normal framebuffer: 1 source row → 2 display rows (2× vertical).
            // Build one RGB row into a stack buffer, then memcpy to both rows.
            // This halves palette lookups vs writing each row independently.
            uint8_t row_buf[SAM_W * 3];
            uint8_t* p = row_buf;
            for (int sx = 0; sx < render_w; ++sx)
            {
                uint8_t idx = src_line[sx] & 0x7F;
                *p++ = m_palette_r[idx];
                *p++ = m_palette_g[idx];
                *p++ = m_palette_b[idx];
            }

            int dy0 = off_y + sy * 2;
            int dy1 = dy0 + 1;

            for (int dy : {dy0, dy1})
            {
                if (dy < 0 || dy >= DST_H) continue;
                uint8_t* dst_row = dst + dy * DST_STRIDE;
                if (has_h_pad)
                {
                    memset(dst_row, 0, dst_x_bytes);
                    if (right_pad_len > 0)
                        memset(dst_row + right_pad_off, 0, right_pad_len);
                }
                memcpy(dst_row + dst_x_bytes, row_buf, render_bytes);
            }
        }
    }

    // Clear bottom padding rows
    {
        int bottom_start = std::max(off_y + scaled_h, 0);
        for (int dy = bottom_start; dy < DST_H; ++dy)
            memset(dst + dy * DST_STRIDE, 0, DST_STRIDE);
    }

    if (vdiag) vt1 = esp_timer_get_time();

    // Flush only the active area rows to PSRAM — skip the black padding rows
    // (top off_y rows and bottom DST_H - off_y - scaled_h rows) which never
    // change after the first frame.  This reduces the msync from 921,600 bytes
    // (full 640×480×3) to the active region only.
    //
    // Active region: rows [off_y .. off_y+scaled_h) for normal fb,
    //                rows [off_y .. off_y+src_h)    for GUI.
    // For visiblearea=0: off_y=48, scaled_h=384 → rows 48..431 = 384 rows.
    // Byte range: [48*640*3 .. (48+384)*640*3) = [92160 .. 829440) = 737,280 bytes.
    // vs full flush: 921,600 bytes — saves 20% msync bandwidth.
    //
    // NOTE: on the very first frame the padding rows are already zeroed and
    // flushed by sim_display_init(), so skipping them here is safe.
    {
        const int active_top    = std::max(off_y, 0);
        const int active_bottom = std::min(off_y + scaled_h, DST_H);
        if (active_bottom > active_top) {
            size_t region_offset = (size_t)active_top * DST_STRIDE;
            size_t region_size   = (size_t)(active_bottom - active_top) * DST_STRIDE;
            sim_display_flush_region(region_offset, region_size);
        } else {
            sim_display_flush();  // fallback (shouldn't happen)
        }
    }

    if (vdiag) {
        vt2 = esp_timer_get_time();
        ESP_LOGI(TAG, "video frame %d: render=%lld us  flush=%lld us  total=%lld us",
                 s_vdiag_count,
                 (long long)(vt1 - vt0),
                 (long long)(vt2 - vt1),
                 (long long)(vt2 - vt0));
        s_vdiag_count++;
    }
}
