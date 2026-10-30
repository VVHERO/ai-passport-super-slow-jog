<p align="right">
  <a href="README.md">简体中文</a> · <strong>English</strong>
</p>

# Super Slow Jog

Turns [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) into a pocket pacer.

180 steps per minute is the pace that makes super slow jogging feel easy — short strides, a low heart rate, still able to talk while you move. This little device does one job: keep a steady beat right next to your ear. A green dot pulses with every tick, so you can lock onto the rhythm by ear or by eye.

Turn the music on and it also plays a 180 BPM running track, with the downbeat, the metronome click and the screen brightness all locked to the same clock: one beat of music, one dip of the screen. You can run without headphones and follow the light instead.

<img src="docs/images/cover.jpg" width="360" alt="Super Slow Jog cover">

## Two ways to run

| Mode | Settings | Behaviour |
|---|---|---|
| Free run | Duration 1–120 min | Stops on its own, so you never have to check a watch |
| Intervals | Rounds 1–20 / Run 1–30 min / Rest 1–10 min | Run and rest use different pitches; a three-note cue marks every switch |

Cadence is adjustable from **120 to 200 steps per minute**, +2 per press, wrapping back to the bottom at the top of the range. Every setting lives in NVS, so it survives a power-off or a battery swap.

## Running music

- A 400-second 180 BPM track taken from one continuous stretch of the 33-minute original, looped without a seam
- With music on, the cadence is locked to 180 steps per minute — the music *is* the cadence — so the cadence row disappears from the home screen
- Pause stops the music too; resume continues from the same spot instead of restarting
- The metronome click is mixed quietly into the downbeat; turn the volume down if you would rather hear only the music
- Music and click share one audio path, so they can never drift apart by half a beat

## Beat-synced screen flash

The settings screen offers `off / soft / strong`: on every beat the whole display dims briefly (45% brightness on soft, 8% on strong, restored after about 110 ms), so you can follow the rhythm out of the corner of your eye. Soft is the default; strong is the full light/dark blink.

## Heart rate (optional, off by default)

- Reads the standard BLE Heart Rate Service directly — Amazfit Balance's "Heart Rate Push" or any common chest strap, no phone app involved
- Turn "Heart rate" on in settings; when a session starts the device scans and connects by itself and shows `心率 82` in the top-left corner of the run screen
- The number is coloured by percentage of maximum heart rate: **blue** (< 60%, too easy), **green** (60–70%, the super-slow-jogging zone), **orange** (70–80%, drifting high), **red** (≥ 80%, high)
- Maximum heart rate defaults to 190 (≈ 220 − 30 years); the settings screen has "Max HR" (150–210, step 5) if yours differs
- On the watch side, heart rate push must be switched on first; some models only actually broadcast during a workout
- Reconnects automatically (rescan after 3 s); when nothing is found it scans for 8 s and then idles for 25 s so it does not drain the battery, and it switches off when the session ends

## Screens

| Run screen | Home screen |
|---|---|
| <img src="docs/images/run.jpg" width="300"> | <img src="docs/images/home.jpg" width="300"> |

Top to bottom on the run screen: the current phase (running / resting / paused), how much of this phase is left (intervals only), total session time as `mm:ss`, steps taken, a dot that pulses with the beat (green running, blue resting, orange frozen while paused), and which keys are available right now. The battery percentage sits in the top-right corner; if the gauge misses a read, that corner stays empty and nothing else is affected.

## Keys

**Home screen**

- Up / Down: move the highlight between rows; the selected row turns darker with green text
- OK on a setting row: bump the current value by one step, wrapping at the ends
- OK on the "Start" row: begin the session
- Hold OK (about half a second): open the settings screen

**Settings screen** (music / screen flash / volume / heart rate / max HR / back)

- Up / Down to move, OK to change the selected item: music on or off, screen flash `off → soft → strong`, volume 0–100% in steps of 10, heart rate on or off, max heart rate 150–210 in steps of 5
- OK on "Back", or hold OK anywhere, saves and returns to the home screen

**While running**

- Tap OK: pause / resume — time does not count while paused and the music stops with it
- Hold OK (about half a second): finish this session and open the result screen
- Up / Down: volume ±10%, with a percentage bubble that fades after about a second

**Result screen**: the title shows whether you finished the planned duration or rounds, or ended early; below it sit the total time and step count for the session. Press OK to return to the home screen with your settings intact.

## Sound

- 22.05 kHz / 16 bit / mono PCM output; every beat is synthesised from a lookup table
- The beat sits higher while running and lower while resting
- In interval mode a three-note cue plays before each phase change, then the new pitch takes over
- At the end of the session an ascending two-note phrase plays and the result screen opens automatically
- Every beat has a 4 ms fade in and out, so there is no click
- Beats are scheduled against absolute time, so nothing drifts over a long run
- The bundled track is 4-bit IMA ADPCM stored in flash: decoding is table lookups and adds, it costs no CPU, and the click is mixed into the same PCM block, so the music downbeat, the click and the screen flash share one clock

## Building

Requires ESP-IDF **v5.5.3**, target `esp32c3`.

```bash
. $IDF_PATH/export.sh          # Windows: %IDF_PATH%\export.bat
idf.py set-target esp32c3
idf.py build
```

`LVGL 9.5` and the other dependencies are pinned in `dependencies.lock` and fetched on the first build.

The running-music asset is not tracked in the repository (the audio belongs to the creator). Generate it once after cloning, then build:

```bash
python tools/pack_music.py "<180 BPM source video>" assets/music/ssj_music.adp \
  --start 428 --duration 400 --peak 0.72 --decode-check
```

Without that file, `main/CMakeLists.txt` stops during configuration and prints the command above.

### Merged firmware (flash from 0x0)

```bash
idf.py merge-bin -o FoloToy-AI-Passport-full.bin
python tools/verify_firmware.py build
```

`verify_firmware.py` checks the image layout and size against what the device expects. The merged image flashes from **0x0** and covers bootloader, partition table and application.

```bash
esptool.py --chip esp32c3 -p <PORT> write_flash 0x0 FoloToy-AI-Passport-full.bin
```

## Validation

```bash
./tools/validate.sh            # static checks + firmware build
./tools/validate.sh --static   # static checks and host tests only
```

Static checks include:

- `tools/check_repo.py` — repository structure rules
- Host tests for the pure C state machine (`tests/test_ssj_logic.c`): 7 cases covering config clamping, session lifecycle, phase transitions and step math
- CJK glyph coverage (`tests/test_ssj_font_coverage.py`), which proves every Chinese character used by the UI exists in the subset font

## Layout

```
main/
  ssj_logic.{c,h}    pure C state machine, no hardware dependency, host-testable
  ssj_app.{c,h}      LVGL UI (home / run / result / settings) and the beat pulse animation
  ssj_audio.{c,h}    metronome synthesis, music streaming and beat-synced screen flash
  ssj_hr.{c,h}       BLE heart rate: CENTRAL scan / connect / subscribe to HRS, auto-reconnect
  ssj_store.{c,h}    NVS persistence of settings
  main.c             boot, key queue, LVGL locking
assets/fonts/        Source Han Sans subset (SIL OFL 1.1) and its licence
assets/music/        running-music IMA ADPCM asset, produced by tools/pack_music.py
tests/               host tests
tools/               build, validation, archiving, music packing
```

The state machine is fully decoupled from the UI, audio and storage layers: `ssj_logic.c` uses only standard C and runs on the host, so timing regressions do not need a device.

## Hardware

ESP32-C3 (8 MB flash, no PSRAM), 240×320 display, 3 ADC buttons, ES8311 codec, CW2017 fuel gauge. The console runs over USB-Serial-JTAG; UART0 TX shares GPIO21 with the backlight, so the firmware leaves UART0 unused.

BLE runs on NimBLE; heart rate needs the CENTRAL and OBSERVER roles (see `sdkconfig.defaults`). The stack is only initialised the first time heart rate is switched on, so it costs no RAM while the feature is off.

## Licence and credits

Built on [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) under the **MIT License**; the original copyright belongs to FoloToy, see [LICENSE](LICENSE).

The bundled subset font is Source Han Sans SC, licensed under the SIL Open Font License 1.1, see [assets/fonts/LICENSE.txt](assets/fonts/LICENSE.txt).

The bundled running music was supplied by the creator of this play. It remains the creator's property, ships only with this play's firmware and is not separately licensed for reuse; see [assets/README.md](assets/README.md) for how it is regenerated.

The beat pulse, the metronome synthesis and the UI layout are implemented from scratch in this project; no demo shell code was reused.
