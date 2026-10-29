#!/usr/bin/env python3
"""把「180 步频伴跑音乐」切成定长循环段并编码成固件用的 IMA ADPCM 素材。

设计约束(必须和固件常量一致,改一处要同时改 main/ssj_audio.c 与 CMakeLists):

- 采样率 22050 Hz / 单声道 / 16bit 中间格式。源素材能量几乎全在 8 kHz 以下,
  22.05 kHz 采样不损失可听内容,同时把素材压到能塞进 Flash 的大小。
- 编解码用 4bit IMA ADPCM,块固定 2450 采样(= 22050/9,111.1 ms)。
  块大小刻意取 2450:1 秒 = 3 拍(180 步/分),2450 = 7350/3,
  因此一拍正好 3 个块,节拍音可以精确落在块首,不需要运行期做样本级定位。
- 每块自带 4 字节头(predictor int16 + step index uint8 + 保留字节),
  固件从任意块开始解码都不需要前文状态,循环接缝处不掉音质。
- 段长必须是整秒(1 秒 = 3 拍),这样循环回绕后节拍相位仍然对齐。

用法:
    python tools/pack_music.py <源视频或音频> <输出 .adp> [--start 428] [--duration 400]
"""

from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

SAMPLE_RATE = 22050
BLOCK_SAMPLES = 2450          # 111.11 ms/块
BLOCK_BYTES = 4 + BLOCK_SAMPLES // 2   # 4 字节头 + 4bit 采样

STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]
INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def clamp16(v: int) -> int:
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def adpcm_encode_block(samples: list[int], predictor: int, index: int) -> tuple[bytes, int, int]:
    """编码一块;返回(字节流, 末态 predictor, 末态 index)。"""
    out = bytearray(struct.pack("<hBB", predictor, index, 0))
    nibbles = bytearray()
    cur = 0
    for i, sample in enumerate(samples):
        step = STEP_TABLE[index]
        diff = sample - predictor
        sign = 1 if diff < 0 else 0
        if sign:
            diff = -diff
        # 解码端重建的幅度是 (2*code+1)*step/8,所以量化档位取 code ≈ 4*diff/step
        # (写成 diff//step 会把每一档都算小一半,SNR 掉到 10 dB 以下)。
        code = (4 * diff) // step
        if code > 7:
            code = 7
        # 用被量化后的 code 反推回放值,保证编码/解码状态一致
        diffq = step >> 3
        if code & 4:
            diffq += step
        if code & 2:
            diffq += step >> 1
        if code & 1:
            diffq += step >> 2
        predictor = clamp16(predictor - diffq if sign else predictor + diffq)
        index += INDEX_TABLE[code]
        index = 0 if index < 0 else 88 if index > 88 else index

        nibble = code | (8 if sign else 0)
        if i % 2 == 0:
            cur = nibble
        else:
            nibbles.append(cur | (nibble << 4))
    out += nibbles
    return bytes(out), predictor, index


def adpcm_decode_block(block: bytes, samples: int) -> list[int]:
    predictor, index = struct.unpack_from("<hB", block, 0)
    out: list[int] = []
    for byte in block[4:]:
        for nibble in (byte & 0x0F, byte >> 4):
            step = STEP_TABLE[index]
            diffq = step >> 3
            if nibble & 4:
                diffq += step
            if nibble & 2:
                diffq += step >> 1
            if nibble & 1:
                diffq += step >> 2
            predictor = clamp16(predictor - diffq if nibble & 8 else predictor + diffq)
            index += INDEX_TABLE[nibble & 7]
            index = 0 if index < 0 else 88 if index > 88 else index
            out.append(predictor)
            if len(out) == samples:
                return out
    return out


def extract_pcm(source: Path, start_s: float, duration_s: float) -> list[int]:
    ffmpeg = shutil.which("ffmpeg") or r"C:\ffmpeg\bin\ffmpeg.exe"
    with tempfile.TemporaryDirectory() as tmp:
        wav_path = Path(tmp) / "loop.wav"
        cmd = [
            ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
            "-ss", f"{start_s:.3f}", "-t", f"{duration_s:.3f}", "-i", str(source),
            "-vn", "-ac", "1", "-ar", str(SAMPLE_RATE), "-c:a", "pcm_s16le", str(wav_path),
        ]
        subprocess.run(cmd, check=True)
        with wave.open(str(wav_path), "rb") as w:
            if w.getframerate() != SAMPLE_RATE or w.getnchannels() != 1:
                raise SystemExit("ffmpeg produced an unexpected format")
            raw = w.readframes(w.getnframes())
    return list(struct.unpack("<%dh" % (len(raw) // 2), raw))


def crossfade_loop(samples: list[int], fade_ms: float = 40.0) -> list[int]:
    """把尾部与头部做等功率交叉淡化,消除循环接缝的爆音。"""
    fade = int(SAMPLE_RATE * fade_ms / 1000.0)
    n = len(samples)
    if fade * 2 >= n:
        raise SystemExit("loop is too short for the requested crossfade")
    loop = list(samples)
    for i in range(fade):
        t = i / fade
        # 等功率:fade-in 用 sin,fade-out 用 cos
        import math
        a = math.sin(t * math.pi / 2)
        b = math.cos(t * math.pi / 2)
        tail = loop[n - fade + i]
        head = samples[i]
        loop[n - fade + i] = int(tail * b + head * a)
    return loop


def normalize(samples: list[int], peak: float = 0.92) -> list[int]:
    top = max(abs(v) for v in samples)
    if top == 0:
        raise SystemExit("silent audio")
    gain = (peak * 32767.0) / top
    return [clamp16(int(round(v * gain))) for v in samples]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("output")
    ap.add_argument("--start", type=float, default=428.0, help="循环段起点(秒)")
    ap.add_argument("--duration", type=float, default=400.0, help="循环段时长(秒,必须整秒)")
    ap.add_argument("--peak", type=float, default=0.92, help="峰值归一化目标(0..1)")
    ap.add_argument("--decode-check", action="store_true", help="解码回 WAV 做自检")
    args = ap.parse_args()

    if abs(args.duration - round(args.duration)) > 1e-6:
        raise SystemExit("--duration must be a whole number of seconds (1 s = 3 beats at 180 BPM)")

    samples = extract_pcm(Path(args.source), args.start, args.duration)
    expected = int(round(args.duration * SAMPLE_RATE))
    if len(samples) < expected:
        raise SystemExit(f"decoded {len(samples)} samples, expected {expected}")
    samples = samples[:expected]
    samples = crossfade_loop(samples)
    samples = normalize(samples, args.peak)

    if len(samples) % BLOCK_SAMPLES:
        raise SystemExit("sample count is not a whole number of ADPCM blocks")

    encoded = bytearray()
    decoded: list[int] = []
    predictor, index = 0, 0
    for offset in range(0, len(samples), BLOCK_SAMPLES):
        block, predictor, index = adpcm_encode_block(
            samples[offset : offset + BLOCK_SAMPLES], predictor, index
        )
        if len(block) != BLOCK_BYTES:
            raise SystemExit(f"block is {len(block)} bytes, expected {BLOCK_BYTES}")
        encoded += block
        if args.decode_check:
            decoded.extend(adpcm_decode_block(block, BLOCK_SAMPLES))

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(encoded)
    blocks = len(encoded) // BLOCK_BYTES
    print(f"wrote {out_path} : {len(encoded)} bytes, {blocks} blocks, "
          f"{blocks * BLOCK_SAMPLES / SAMPLE_RATE:.1f} s @ {SAMPLE_RATE} Hz IMA ADPCM mono")

    if args.decode_check:
        err = sum((a - b) ** 2 for a, b in zip(samples, decoded))
        sig = sum(a * a for a in samples)
        snr = 10 * 0.4342944819032518 * (sig / err).__log__() if False else 10 * __import__("math").log10(sig / err)
        print(f"decode self-check: SNR {snr:.1f} dB over {len(decoded)} samples")
        wav_path = out_path.with_suffix(".check.wav")
        with wave.open(str(wav_path), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SAMPLE_RATE)
            w.writeframes(struct.pack("<%dh" % len(decoded), *decoded))
        print(f"wrote {wav_path} for listening / spectrogram inspection")
    return 0


if __name__ == "__main__":
    sys.exit(main())
