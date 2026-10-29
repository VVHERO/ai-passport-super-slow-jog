<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, sound effects, and battery profiles, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

The Super Slow Jog app (`main/ssj_app.c`) uses the following assets:

| File | Description |
| --- | --- |
| `fonts/SourceHanSansCN-Regular.otf` | Source Han Sans CN Regular source font, used only to generate the subsets locally and not compiled into the firmware. Source: [adobe-fonts/source-han-sans](https://github.com/adobe-fonts/source-han-sans) (release branch, SubsetOTF/CN), SIL Open Font License 1.1, redistribution permitted. |
| `fonts/LICENSE.txt` | Full text of the SIL Open Font License 1.1 covering that source font (Adobe copyright notice), distributed alongside it. |
| `fonts/ssj_symbols.txt` | CJK character inventory locked to the actual UI strings (65 characters). Update it and regenerate whenever the copy changes. |
| `fonts/ssj_font_20.c` | 20 px body subset: ASCII 0x20-0x7E + U+00B7 middle dot + full-width comma and period + the 65 characters above, 4 bpp uncompressed, about 135 KB of Flash. Compiled into the firmware and used for all copy in `main/ssj_app.c`. |
| `fonts/ssj_font_48.c` | 48 px large subset: ASCII 0x20-0x7E (digits, colon and percent for the timer), 4 bpp uncompressed, about 222 KB of Flash. |

Generation commands (lv_font_conv, run from the repository root; pin the converter version with `npx lv_font_conv@<version>`):

```bash
npx lv_font_conv \
  --font assets/fonts/SourceHanSansCN-Regular.otf \
  --range 0x20-0x7E,0xB7,0xFF0C,0x3002 \
  --symbols "$(cat assets/fonts/ssj_symbols.txt)" \
  --size 20 --bpp 4 --format lvgl --no-compress \
  --lv-font-name ssj_font_20 --lv-include lvgl.h \
  --output assets/fonts/ssj_font_20.c

npx lv_font_conv \
  --font assets/fonts/SourceHanSansCN-Regular.otf \
  --range 0x20-0x7E \
  --size 48 --bpp 4 --format lvgl --no-compress \
  --lv-font-name ssj_font_48 --lv-include lvgl.h \
  --output assets/fonts/ssj_font_48.c
```

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.

Bundled running music:

| File | Description |
| --- | --- |
| `music/ssj_music.adp` | 400-second loop of the 180 BPM running track: 22,050 Hz mono, encoded as 4-bit IMA ADPCM in fixed 2,450-sample blocks (4-byte header + 1,225 bytes, 1,229 bytes per block), 3,600 blocks, 4,424,400 bytes. |

- Source: a 33:28 "180 BPM running music" video supplied by the play's creator (720x1280, AAC 44.1 kHz stereo). The audio remains the creator's property, ships only with this play's firmware, and is not separately licensed for reuse.
- **The file is not tracked by git** (`/assets/music/*.adp` is in `.gitignore`); only the packing script lives in the repository. After a fresh clone you must generate it with the command below, otherwise `main/CMakeLists.txt` stops with an explicit error during configuration.
- The loop is taken from 428 s to 828 s: that span is an exact number of 180 BPM beats (one second equals three beats) and sits between two mix transitions. The last 40 ms are equal-power crossfaded with the opening 40 ms, so the loop point has no click.
- Peak is normalised to 0.72 FS to leave headroom for the metronome click mixed on top.
- ADPCM instead of MP3: decoding is table lookups and adds, so it costs no CPU and needs no decode buffer, and the metronome click can be mixed into the same I2S stream with its phase locked to the music.
- Regenerate (needs the source video and ffmpeg):

```bash
python tools/pack_music.py "<source video>" assets/music/ssj_music.adp \
  --start 428 --duration 400 --peak 0.72 --decode-check
```

`--duration` must be a whole number of seconds (one second equals three beats), otherwise
the beat phase drifts at the loop point. `--decode-check` decodes the result back to WAV
and prints the SNR (currently 25.9 dB).

## Battery profiles

[`battery/wanli-500mah-profile2-xil40x-20261020.txt`](battery/wanli-500mah-profile2-xil40x-20261020.txt)
contains the supplied CW2017 profile for the Wanli New Energy 500 mAh cell
(profile2, XIL40X, 2026-10-20). It is an unchanged copy of the project-supplied
file, retained at the maintainer's request for this firmware update. Its
original filename is recorded in the paired Chinese document. No separate
license was included in the supplied file.

The format is 80 lines of two hexadecimal digits, one byte per line in register
order. The BSP embeds those bytes in `components/bsp/src/bsp_battery.c`; the
host gate checks the source file and initializer for an exact match. Text-file
SHA-256: `7ea96eba4634f4495a37c25ffacd54c1deaa0693b162c892d671a2ba68460919`.
Decoded 80-byte SHA-256:
`fa7365671093ef449c5bdcfb4adb279e3d275f888109f50b6b2f21e9b96220d5`.
This is a fuel-gauge profile, not a charging configuration or a generic profile
for every 500 mAh cell; see the [hardware guide](../docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md#9-cw2017-fuel-gauge).
