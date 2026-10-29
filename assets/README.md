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
| `fonts/ssj_symbols.txt` | CJK character inventory locked to the actual UI strings (57 characters). Update it and regenerate whenever the copy changes. |
| `fonts/ssj_font_20.c` | 20 px body subset: ASCII 0x20-0x7E + U+00B7 middle dot + full-width comma and period + the 57 characters above, 4 bpp uncompressed, about 126 KB of Flash. Compiled into the firmware and used for all copy in `main/ssj_app.c`. |
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
