// Part of SimCoupe - A SAM Coupe emulator
//
// Audio.cpp: ESP32 audio output via ES8311 codec (sim_audio component)
//
// SimCoupe calls:
//   Audio::Init()         — called once at startup
//   Audio::Exit()         — called at shutdown
//   Audio::AddData()      — called every frame with stereo int16_t PCM
//
// We delegate to sim_audio which owns the ES8311 + I2S driver.
// sim_display_init() must have been called first so we can share its I2C bus.

#include "SimCoupe.h"
#include "Audio.h"
#include "Options.h"
#include "Sound.h"

#include "sim_audio.h"
#include "sim_display.h"

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "audio";

static bool s_initialized    = false;
static bool s_externally_owned = false;  // true if main.cpp owns the audio lifecycle

bool Audio::Init()
{
    // If sim_audio was already initialized by main.cpp (sharing the display
    // I2C bus), adopt it without re-initializing — and mark it as externally
    // owned so Audio::Exit() won't tear it down (Sound::Init calls Exit first,
    // which would delete the I2S channel while DMA is still running).
    if (sim_audio_get_tx_handle() != nullptr)
    {
        s_initialized     = true;
        s_externally_owned = true;
        return true;
    }

    Exit();

    // sim_display_init() must already have been called — share its I2C bus
    i2c_master_bus_handle_t bus = sim_display_get_i2c_bus();
    esp_err_t err;
    if (bus)
    {
        err = sim_audio_init_with_bus(bus);
    }
    else
    {
        // Fallback: create own I2C bus (display not yet initialized)
        err = sim_audio_init();
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "sim_audio_init failed: 0x%x", err);
        return false;
    }

    sim_audio_set_volume(80);
    s_initialized     = true;
    s_externally_owned = false;
    return true;
}

void Audio::Exit()
{
    if (!s_initialized)
        return;

    if (s_externally_owned)
    {
        // main.cpp owns the I2S/codec lifecycle — don't tear it down.
        // Sound::Init() calls Exit() before re-initializing; if we called
        // sim_audio_deinit() here the DMA ISR would fire with a deleted
        // channel handle (msg_queue == NULL) and assert in FreeRTOS.
        ESP_LOGD(TAG, "Audio::Exit — externally owned, skipping deinit");
        s_initialized = false;
        return;
    }

    sim_audio_deinit();
    s_initialized = false;
}

// Called every frame with stereo 16-bit PCM.
// pData_ points to int16_t samples (L, R, L, R, ...).
// len_bytes is the byte count (num_samples * 2).
// Returns a fill ratio [0.0, 1.0] — we approximate from the write result.
float Audio::AddData(uint8_t* pData_, int len_bytes)
{
    if (!s_initialized || !pData_ || len_bytes <= 0)
        return 0.0f;

    int num_samples = len_bytes / sizeof(int16_t);

    // Block until the I2S DMA ring has room for all samples.
    // This makes I2S the master clock for the emulation loop — the natural
    // backpressure of the DMA running at exactly 44100 Hz regulates timing.
    // portMAX_DELAY avoids the 20ms timeout that was causing partial writes
    // and incorrect fill ratios.
    {
        static int s_diag = 0;
        if (s_diag < 10) {
            int64_t t0 = esp_timer_get_time();
            int written_diag = sim_audio_write(
                reinterpret_cast<const int16_t*>(pData_), num_samples, portMAX_DELAY);
            int64_t dt = esp_timer_get_time() - t0;
            ESP_LOGI(TAG, "audio write %d: blocked %lld us, wrote %d/%d samples",
                     s_diag, dt, written_diag, num_samples);
            s_diag++;
            if (written_diag < 0) return 0.0f;
            return 0.5f;
        }
    }
    int written = sim_audio_write(
        reinterpret_cast<const int16_t*>(pData_),
        num_samples,
        portMAX_DELAY);

    if (written < 0)
        return 0.0f;

    // Return 0.5 (half full) so Sound.cpp's speed-adjust scale stays at 1.0
    // and does NOT call sleep_until — timing is already handled by I2S above.
    return 0.5f;
}
