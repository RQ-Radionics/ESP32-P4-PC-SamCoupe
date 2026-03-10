// Part of SimCoupe - A SAM Coupe emulator
//
// OSD.cpp: ESP32-P4 "OS-dependent" functions
//
// On ESP32 + FreeRTOS there is no SDL, no desktop OS, no clipboard.
// Files live on the SD card mounted at /sdcard.

#include "SimCoupe.h"
#include "OSD.h"
#include "Options.h"

#include "esp_log.h"

static const char* TAG = "osd";

bool OSD::Init()
{
    // Force audio sync on ESP32: let I2S DMA backpressure regulate timing.
    // std::this_thread::sleep_until (used by Sound.cpp when audiosync=false)
    // relies on high_resolution_clock which maps to system_clock on this
    // toolchain (is_steady=false). With an unset RTC (epoch=0), sleep_until
    // behaves incorrectly and stalls the emulation loop, causing ~14% speed.
    // With audiosync=true, Sound.cpp skips sleep_until entirely and timing
    // is driven by i2s_channel_write blocking at 44100 Hz.
    SetOption(audiosync, true);

    // Force visiblearea=0 (no border, 512×192 framebuffer).
    // The display is 640×480; the SAM image is scaled 1×H + 2×V → 512×384,
    // centred with 64px left/right and 48px top/bottom black padding.
    // Larger visiblearea values produce wider framebuffers that overflow the
    // 640px display width (e.g. visiblearea=2 → 576px → 1152px scaled).
    SetOption(visiblearea, 0);

    return true;
}

void OSD::Exit()
{
    // Nothing to do
}

std::string OSD::MakeFilePath(PathType type, const std::string& filename)
{
    std::string base;

    switch (type)
    {
    case PathType::Settings:
        base = "/sdcard/simcoupe";
        break;

    case PathType::Input:
        if (!GetOption(inpath).empty())
            base = GetOption(inpath);
        else
            base = "/sdcard/simcoupe/disks";
        break;

    case PathType::Output:
        if (!GetOption(outpath).empty())
            base = GetOption(outpath);
        else
            base = "/sdcard/simcoupe/output";
        break;

    case PathType::Resource:
        base = "/sdcard/simcoupe";
        break;

    default:
        base = "/sdcard/simcoupe";
        break;
    }

    // Ensure directory exists
    if (!fs::exists(base))
    {
        std::error_code ec;
        fs::create_directories(base, ec);
    }

    if (filename.empty())
        return base;

    return (fs::path(base) / filename).string();
}

bool OSD::IsHidden(const std::string& path)
{
    auto p = fs::path(path);
    return p.has_filename() && p.filename().string().front() == '.';
}

std::string OSD::GetClipboardText()
{
    // No clipboard on ESP32
    return {};
}

void OSD::SetClipboardText(const std::string& /*str*/)
{
    // No clipboard on ESP32
}

void OSD::DebugTrace(const std::string& str)
{
    ESP_LOGD(TAG, "%s", str.c_str());
}
