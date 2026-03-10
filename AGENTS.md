# Agent Instructions

This project uses **bd** (beads) for issue tracking. Run `bd onboard` to get started.

## Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --claim  # Claim work atomically
bd close <id>         # Complete work
bd sync               # Sync with git
```

## Non-Interactive Shell Commands

**ALWAYS use non-interactive flags** with file operations to avoid hanging on confirmation prompts.

Shell commands like `cp`, `mv`, and `rm` may be aliased to include `-i` (interactive) mode on some systems, causing the agent to hang indefinitely waiting for y/n input.

**Use these forms instead:**
```bash
# Force overwrite without prompting
cp -f source dest           # NOT: cp source dest
mv -f source dest           # NOT: mv source dest
rm -f file                  # NOT: rm file

# For recursive operations
rm -rf directory            # NOT: rm -r directory
cp -rf source dest          # NOT: cp -r source dest
```

**Other commands that may prompt:**
- `scp` - use `-o BatchMode=yes` for non-interactive
- `ssh` - use `-o BatchMode=yes` to fail instead of prompting
- `apt-get` - use `-y` flag
- `brew` - use `HOMEBREW_NO_AUTO_UPDATE=1` env var

<!-- BEGIN BEADS INTEGRATION -->
## Issue Tracking with bd (beads)

**IMPORTANT**: This project uses **bd (beads)** for ALL issue tracking. Do NOT use markdown TODOs, task lists, or other tracking methods.

### Why bd?

- Dependency-aware: Track blockers and relationships between issues
- Version-controlled: Built on Dolt with cell-level merge
- Agent-optimized: JSON output, ready work detection, discovered-from links
- Prevents duplicate tracking systems and confusion

### Quick Start

**Check for ready work:**

```bash
bd ready --json
```

**Create new issues:**

```bash
bd create "Issue title" --description="Detailed context" -t bug|feature|task -p 0-4 --json
bd create "Issue title" --description="What this issue is about" -p 1 --deps discovered-from:bd-123 --json
```

**Claim and update:**

```bash
bd update <id> --claim --json
bd update bd-42 --priority 1 --json
```

**Complete work:**

```bash
bd close bd-42 --reason "Completed" --json
```

### Issue Types

- `bug` - Something broken
- `feature` - New functionality
- `task` - Work item (tests, docs, refactoring)
- `epic` - Large feature with subtasks
- `chore` - Maintenance (dependencies, tooling)

### Priorities

- `0` - Critical (security, data loss, broken builds)
- `1` - High (major features, important bugs)
- `2` - Medium (default, nice-to-have)
- `3` - Low (polish, optimization)
- `4` - Backlog (future ideas)

### Workflow for AI Agents

1. **Check ready work**: `bd ready` shows unblocked issues
2. **Claim your task atomically**: `bd update <id> --claim`
3. **Work on it**: Implement, test, document
4. **Discover new work?** Create linked issue:
   - `bd create "Found bug" --description="Details about what was found" -p 1 --deps discovered-from:<parent-id>`
5. **Complete**: `bd close <id> --reason "Done"`

### Auto-Sync

bd automatically syncs with git:

- Exports to `.beads/issues.jsonl` after changes (5s debounce)
- Imports from JSONL when newer (e.g., after `git pull`)
- No manual export/import needed!

### Important Rules

- ✅ Use bd for ALL task tracking
- ✅ Always use `--json` flag for programmatic use
- ✅ Link discovered work with `discovered-from` dependencies
- ✅ Check `bd ready` before asking "what should I work on?"
- ❌ Do NOT create markdown TODO lists
- ❌ Do NOT use external issue trackers
- ❌ Do NOT duplicate tracking systems

For more details, see README.md and docs/QUICKSTART.md.

## Landing the Plane (Session Completion)

**When ending a work session**, you MUST complete ALL steps below. Work is NOT complete until `git push` succeeds.

**MANDATORY WORKFLOW:**

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   bd sync
   git push
   git status  # MUST show "up to date with origin"
   ```
5. **Clean up** - Clear stashes, prune remote branches
6. **Verify** - All changes committed AND pushed
7. **Hand off** - Provide context for next session

**CRITICAL RULES:**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push
- If push fails, resolve and retry until it succeeds

<!-- END BEADS INTEGRATION -->

## SimCoupe ESP32-P4 Port — Project Context

**Project**: SimCoupe SAM Coupé emulator ported to Olimex ESP32-P4-PC  
**Credit**: RQ-Radionics (Ramon Martinez @ Jorge Fuertes)  
**Status**: 100% emulation speed achieved (50 fps stable)

### Hardware Target

- **Board**: Olimex ESP32-P4-PC (ESP32-P4, dual-core RISC-V 400 MHz, 32 MB PSRAM, 16 MB flash)
- **Display**: HDMI via LT8912B bridge (DPI RGB888 → MIPI DSI → HDMI), 640×480@60Hz VESA (SAM 512×384 centrado)
- **Audio**: ES8311 codec via I2S, 22050 Hz mono
- **Input**: USB HID keyboard via FE1.1s hub (PS/2 Set2 scancodes)
- **Storage**: microSD card mounted at `/sdcard`

### Build

```bash
# Full clean build
bash build_olimex_p4pc.sh clean

# Incremental build
idf.py build

# Flash
idf.py flash monitor
```

Requires ESP-IDF v5.3+ with ESP32-P4 support. Source the IDF environment first:
```bash
source /path/to/esp-idf/export.sh
```

### FreeRTOS Task Layout

| Priority | Task | Core |
|----------|------|------|
| 22+ | esp_timer, ipc0/1 | 0/1 |
| 15 | simcoupe_task (Z80 + video) | 1 |
| 14 | sound_task (Sound::FrameUpdate) | 0 |
| 10 | usb_lib | 0 |
| 5 | usb_client | 0 |
| 0 | IDLE0, IDLE1 | 0/1 |

**simcoupe_task stack must be DRAM** — `esp_cache_msync(DIR_C2M)` can invalidate PSRAM stack cache lines mid-function causing load access faults.

### Sound Pipeline (per frame)

```
Core 1: ExecuteChunk(N) → Frame::End(N) → IO::FrameUpdate(N) →
        [take s_sound_done] → [give s_sound_start] → ExecuteChunk(N+1)
Core 0: [take s_sound_start] → Sound::FrameUpdate(N) → [give s_sound_done]
```

Pre-give `s_sound_done` at startup so the first frame doesn't stall.

### Video Pipeline (dirty-line tracking)

```
Frame::End(N) → Video::Update(fb):
  for each of 192 rows:
    memcmp(row, m_prev[row], 512)   ← DRAM, skip if unchanged
    expand: palette[index] → RGB888 → m_row_buf (DRAM)
    memcpy(m_row_buf → PSRAM dy0)
    memcpy(m_row_buf → PSRAM dy1)
  flush_region(first_dirty..last_dirty only)
```

On static screens (boot, BASIC) dirty=0/192 → video blit drops from ~13 ms to ~2 ms.

### Key Performance Discoveries

1. **PSRAM bus contention**: Overlapping Z80 (reads SAM RAM from PSRAM) with async video blit (writes display buffer to PSRAM) on separate cores causes severe contention — Z80 slows from 12.5 ms to 21.5 ms/frame. Async video was reverted; video runs synchronously after Z80.

2. **Dirty-line tracking**: Compare each 512-byte SAM row with previous frame (DRAM→DRAM `memcmp`). Static screens skip nearly all blit work.

3. **SAASound `double` → `float`**: ESP32-P4 RISC-V has hardware FPU for `float` only; `double` is software-emulated (~10× slower). `scale_for_output()` called 441×/frame — switching to `float` gave a major speedup.

4. **SAASound DEFAULT_OVERSAMPLE 6 → 2**: Reduces chip ticks from 28,224 to 1,764 per frame (16× less work). Inaudible quality difference at 22050 Hz.

5. **FreqTable PSRAM → DRAM**: 8 KB frequency lookup table accessed every oscillator tick. DRAM latency ~5 ns vs PSRAM ~100 ns.

6. **512×480 display**: SAM active area is exactly 512 px wide. Eliminates horizontal padding, reduces flush region ~20% vs 640×480, makes rows contiguous in PSRAM.

### Key Source Files

| File | Purpose |
|------|---------|
| `main/main.cpp` | `app_main` + `simcoupe_task` (Core 1, prio 15, 32 KB DRAM stack) |
| `components/simcoupe_core/Base/CPU.cpp` | `sound_task` (Core 0, prio 14), semaphore pipeline |
| `ESP32/Video.cpp` | 512×480, dirty-line tracking, `m_prev[98304]` DRAM |
| `ESP32/Audio.cpp` | Real `sim_audio_write()`, no stubs |
| `components/simcoupe_core/SAASound/src/SAAImpl.cpp` | `float` arithmetic + filter state |
| `components/simcoupe_core/SAASound/src/defns.h` | `DEFAULT_OVERSAMPLE=2` |
| `components/simcoupe_core/SAASound/src/SAAFreq.cpp` | `FreqTable` in DRAM |
| `components/simcoupe_core/Base/Sound.h` | `SAMPLE_FREQ=22050` |
| `components/simcoupe_core/Base/SAMIO.cpp` | `Sound::FrameUpdate()` removed from `IO::FrameUpdate()` |
| `sdkconfig.defaults` | `HACT=512`, `HBP=248`, `SAMPLE_RATE=22050` |

### Open Issues

- **SamCoupe-zv2**: Replace kosarev Z80 with Z80_JLS from ESPectrum (deferred — previous attempt had emulation correctness issues)
- **SamCoupe-d9y**: Special keys (F11 reset, F12 menu), persistent config on SD card, auto-load disk

### Reference Repositories (do not modify)

- `/Volumes/FastDisk/Queru/Ports/esp32-mos/` — working ESP32-P4-PC reference implementation
- `/Volumes/FastDisk/Emu/simcoupe/` — original SimCoupe source
- `/Volumes/FastDisk/Emu/ESPectrum/` — ESPectrum (Z80_JLS source)
