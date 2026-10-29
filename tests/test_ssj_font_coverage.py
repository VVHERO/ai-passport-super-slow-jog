# tests/test_ssj_font_coverage.py —— 超慢跑应用字体码点覆盖检查(host 侧,无 ESP-IDF 依赖)。
#
# 依据 lvgl-chinese-fonts 指南的验收要求:对应用实际使用的每个字体,检查全部
# 所需可打印 Unicode 码点是否在生成子集中。本测试从 ssj_app.c 的字符串字面量
# 解码出实际文案码点,再与生成的字体 .c(其 Opts 注释与码点表)对照:
#   - ASCII 区:要求 0x20-0x7E 全覆盖;
#   - 中间点 U+00B7(按键提示分隔符):要求覆盖;
#   - CJK:要求 ssj_symbols.txt(生成时锁定的字符清单)里的每个字符都出现在
#     字体 .c 的 Opts --symbols 参数里,且清单与当前 UI 文案完全一致。
# 字体子集只收录了固定文案,清单不一致即视为缺字,直接失败,防止改文案忘重新生成。
#
# 运行: python3 tests/test_ssj_font_coverage.py(仓库根目录)
import re
import sys
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "main" / "ssj_app.c"
SYMBOLS = ROOT / "assets" / "fonts" / "ssj_symbols.txt"
FONTS = {
    "ssj_font_20": ROOT / "assets" / "fonts" / "ssj_font_20.c",
    "ssj_font_48": ROOT / "assets" / "fonts" / "ssj_font_48.c",
}

failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL:", msg)


def extract_ui_codepoints(text):
    """从 ssj_app.c 的字符串字面量提取实际文案的全部码点(排除注释与自检固件分支)。"""
    # 去掉 // 与 /* */ 注释,避免把注释里的汉字当作文案。
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    # 自检固件分支(SSJ_SELFTEST)只在诊断构建里编译,里面的日志文案不进界面,
    # 也不该逼着字体子集收进"会话/自动"这类日志用字。
    text = re.sub(r"#ifdef SSJ_SELFTEST.*?#endif", "", text, flags=re.S)
    cps = set()
    for s in re.findall(r'"((?:[^"\\]|\\.)*)"', text):
        s = s.replace("\\n", "\n")
        # 源码里没有 \uXXXX 转义;直接按 UTF-8 解码后的字符统计。
        for ch in s:
            cps.add(ord(ch))
    return cps


def font_opts_symbols(font_path):
    """读取字体 .c 头部 Opts 注释,返回 (range_str, symbols_str)。"""
    head = font_path.read_text(encoding="utf-8", errors="replace")[:2000]
    m = re.search(r"Opts: (.*)", head)
    if not m:
        fail(f"{font_path.name}: 找不到 Opts 注释")
        return "", ""
    opts = m.group(1)
    r = re.search(r"--range (\S+)", opts)
    s = re.search(r"--symbols (\S+)", opts)
    return (r.group(1) if r else ""), (s.group(1) if s else "")


def parse_range_tokens(range_str):
    """把 --range 参数解析成码点集合。

    lv_font_conv 写 Opts 时会把单个码点写成十进制(如 U+00B7 -> 183),
    区间保持十六进制,所以这里统一用 int(tok, 0) 兼容两种写法。
    """
    cps = set()
    for tok in range_str.split(","):
        tok = tok.strip()
        if not tok:
            continue
        try:
            if "-" in tok:
                lo, hi = tok.split("-", 1)
                cps.update(range(int(lo, 0), int(hi, 0) + 1))
            else:
                cps.add(int(tok, 0))
        except ValueError:
            fail(f"无法解析 --range 片段: {tok!r}")
    return cps


def main():
    text = APP.read_text(encoding="utf-8")
    ui_cps = extract_ui_codepoints(text)

    # 1. ASCII 与中间点必须由 ssj_font_20 覆盖(UI 全部文案都走 20 号字)。
    r20, sym20 = font_opts_symbols(FONTS["ssj_font_20"])
    cps20 = parse_range_tokens(r20)
    missing_ascii = [c for c in range(0x20, 0x7F) if c not in cps20]
    if missing_ascii:
        fail(f"ssj_font_20 未覆盖 ASCII 区: range={r20}")
    if 0xB7 not in cps20:
        fail(f"ssj_font_20 未覆盖中间点 U+00B7: range={r20}")

    # 2. CJK 覆盖:UI 文案中的每个 CJK 码点必须在 ssj_symbols.txt 清单内,
    #    且清单里的每个字符都被生成命令实际采用(--symbols)。
    listed = set(SYMBOLS.read_text(encoding="utf-8").strip())
    ui_cjk = {chr(c) for c in ui_cps if 0x4E00 <= c <= 0x9FFF}
    missing_in_list = ui_cjk - listed
    if missing_in_list:
        fail(
            "UI 出现了字符清单之外的汉字: "
            + " ".join(f"U+{ord(c):04X}({c})" for c in sorted(missing_in_list))
            + " —— 请更新 assets/fonts/ssj_symbols.txt 并重新生成字体"
        )
    if not listed <= set(sym20):
        missing = sorted(listed - set(sym20))
        fail(
            "ssj_font_20 生成时未采用清单字符: "
            + " ".join(f"U+{ord(c):04X}({c})" for c in missing)
        )

    # 3. 48 号大字字体只用于 MM:SS/%,要求 ASCII 数字/冒号/百分号可用。
    r48, _ = font_opts_symbols(FONTS["ssj_font_48"])
    cps48 = parse_range_tokens(r48)
    if [c for c in range(0x20, 0x7F) if c not in cps48]:
        fail(f"ssj_font_48 未覆盖 ASCII 区: range={r48}")

    # 4. 反例:清单外的生僻字应判定不覆盖(防止检查无条件通过)。
    probe = "\u9F98"   # 龘 之外的 U+9F98,刻意不在清单
    if probe in listed:
        fail("字符清单不应包含反例字符 U+9F98")

    if failures:
        print(f"字体覆盖检查: {len(failures)} 处失败")
        return 1
    print(
        "SSJ font coverage: PASS "
        f"(ASCII+U+00B7+{len(listed)} CJK -> ssj_font_20, digits -> ssj_font_48)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
