 // Part of SimCoupe - A SAM Coupe emulator
//
// CPU.cpp: Z80 processor emulation and main emulation loop
//
//  Copyright (c) 1999-2015 Simon Owen
//  Copyright (c) 2000-2003 Dave Laundon
//  Copyright (c) 1996-2001 Allan Skillman
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

// Changes 1999-2000 by Simon Owen:
//  - general revamp and reformat, with execution now polled for each frame
//  - very rough contended memory timings by doubling basic timings
//  - frame/line interrupt and flash frequency values corrected

// Changes 2000-2001 by Dave Laundon
//  - perfect contended memory timings on each memory/port access
//  - new cpu event model to reduce the per-instruction overhead
//  - MIDI OUT interrupt timings corrected

#include "SimCoupe.h"
#include "CPU.h"
#include "esp_timer.h"
#include "esp_log.h"
static const char* TAG_PERF = "z80perf";

#include "BlueAlpha.h"
#include "Debug.h"
#include "Events.h"
#include "Frame.h"
#include "GUI.h"
#include "Input.h"
#include "Keyin.h"
#include "SAMIO.h"
#include "Memory.h"
#include "Mouse.h"
#include "Options.h"
#include "Tape.h"
#include "UI.h"

sam_cpu cpu;

////////////////////////////////////////////////////////////////////////////////
//  H E L P E R   M A C R O S


bool g_fBreak, g_fPaused;
int g_nTurbo;

constexpr auto max_boot_frames{ 200 };
int boot_frames;

#ifdef _DEBUG
bool debug_break;
#endif


namespace CPU
{
uint32_t frame_cycles;
bool reset_asserted = false;
uint16_t last_in_port, last_out_port;
uint8_t last_in_val, last_out_val;

bool Init(bool fFirstInit_/*=false*/)
{
    bool fRet = true;

    if (fFirstInit_)
    {
        InitEvents();
        AddEvent(EventType::FrameInterrupt, 0);
        AddEvent(EventType::InputUpdate, CPU_CYCLES_PER_FRAME * 3 / 4);

        cpu.on_reset(false);
        fRet &= Memory::Init(true) && IO::Init();
    }

    Reset(true);
    Reset(false);

    return fRet;
}

void Exit(bool fReInit_)
{
    IO::Exit(fReInit_);
    Memory::Exit(fReInit_);

    // TODO: remove after switching to use "physical" offsets instead of pointers
    if (!fReInit_)
        Breakpoint::RemoveAll();
}



void ExecuteChunk()
{
    if (reset_asserted)
    {
        CPU::frame_cycles = CPU_CYCLES_PER_FRAME;
        CheckEvents(CPU::frame_cycles);
        return;
    }

    // ── Z80 per-frame timing diagnostic (5 frames after frame 60) ───────────
    // Counts instructions and wall time per frame to compute ns/insn and
    // ns/z80cycle — tells us how expensive the Z80 decoder is per instruction.
    static int s_perf_frames = 0;   // frames logged so far
    static int s_total_frames = 0;  // total frames executed (including reset)
    static int64_t s_perf_t0 = 0;
    static uint32_t s_perf_insns = 0;
    s_total_frames++;
    // Start logging after frame 400 (~8s after boot, well after USB enumeration)
    bool perf_active = (s_total_frames > 400) && (s_perf_frames < 5);
    if (perf_active) {
        s_perf_t0 = esp_timer_get_time();
        s_perf_insns = 0;
    }

    for (g_fBreak = false; !g_fBreak; )
    {
        cpu.on_step();
        if (perf_active) s_perf_insns++;

        CheckEvents(CPU::frame_cycles);

        if ((~IO::State().status & STATUS_INT_MASK) && Memory::full_contention)
            cpu.on_handle_active_int();

        if (cpu.get_iregp_kind() != z80::iregp::hl)
            continue;

#ifdef _DEBUG
        if (debug_break)
        {
            Debug::Start();
            debug_break = false;
        }
#endif

        if (Breakpoint::breakpoints.empty())
            continue;

        Debug::AddTraceRecord();

        if (auto bp_index = Breakpoint::Hit())
        {
            CheckEvents(CPU::frame_cycles);
            Debug::Start(bp_index);
        }
    }

    if (perf_active) {
        int64_t dt_us = esp_timer_get_time() - s_perf_t0;
        // frame_cycles = Z80 cycles executed this frame
        // insns = instructions executed
        // ns_per_insn = dt_us*1000 / insns
        uint32_t cyc = CPU::frame_cycles;
        uint32_t ns_per_insn = (s_perf_insns > 0) ? (uint32_t)(dt_us * 1000 / s_perf_insns) : 0;
        uint32_t ns_per_cycle = (cyc > 0) ? (uint32_t)(dt_us * 1000 / cyc) : 0;
        ESP_LOGI(TAG_PERF,
                 "frame %d: %lld us, %lu insns, %lu z80cyc, %lu ns/insn, %lu ns/z80cyc",
                 s_perf_frames, dt_us, (unsigned long)s_perf_insns,
                 (unsigned long)cyc,
                 (unsigned long)ns_per_insn, (unsigned long)ns_per_cycle);
        s_perf_frames++;
    }

    if (boot_frames > 0 && !--boot_frames)
        g_nTurbo &= ~TURBO_BOOT;
}

void Run()
{
    // ── Loop timing diagnostic — logs breakdown for first 5 complete frames ──
    static int s_run_frames = 0;
    static int64_t s_t_execute, s_t_frameend, s_t_flyback, s_t_checkevents;

    while (UI::CheckEvents())
    {
        if (g_fPaused)
            continue;

        int64_t t0 = esp_timer_get_time();
        if (!Debug::IsActive() && !GUI::IsModal())
            ExecuteChunk();
        int64_t t1 = esp_timer_get_time();

        Frame::End();
        int64_t t2 = esp_timer_get_time();

        int64_t t2b = 0, t2c = 0, t2d = 0;
        if (CPU::frame_cycles >= CPU_CYCLES_PER_FRAME)
        {
            EventFrameEnd(CPU_CYCLES_PER_FRAME);
            t2b = esp_timer_get_time();
            IO::FrameUpdate();
            t2c = esp_timer_get_time();
            Debug::FrameEnd();
            Frame::Flyback();
            t2d = esp_timer_get_time();
            CPU::frame_cycles %= CPU_CYCLES_PER_FRAME;
        }
        int64_t t3 = esp_timer_get_time();

        if (s_run_frames < 5 && s_run_frames >= 0)
        {
            // Log frames 400-404 (~8s after boot, well after USB enumeration at ~5s)
            static int s_run_skip = 400;
            if (s_run_skip > 0) { s_run_skip--; }
            else {
                ESP_LOGI(TAG_PERF,
                         "run frame %d: execute=%lld  frame_end=%lld  io_frameupdate=%lld  flyback=%lld  total=%lld  (us)",
                         s_run_frames,
                         (long long)(t1 - t0),
                         (long long)(t2 - t1),
                         (long long)(t2c - t2b),
                         (long long)(t2d - t2c),
                         (long long)(t3 - t0));
                s_run_frames++;
            }
        }
    }

    TRACE("Quitting main emulation loop...\n");
}

void Reset(bool active)
{
    if (GetOption(fastreset) && reset_asserted && !active)
    {
        g_nTurbo |= TURBO_BOOT;
        boot_frames = max_boot_frames;
    }

    reset_asserted = active;
    if (reset_asserted)
    {
        cpu.on_reset(true);

        Keyin::Stop();
        Tape::Stop();

        IO::Init();
        Memory::Init();

        Debug::Refresh();
    }
}

void NMI()
{
    cpu.initiate_nmi();
    Debug::Refresh();
}

} // namespace CPU
