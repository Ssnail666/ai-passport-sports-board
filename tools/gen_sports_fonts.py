#!/usr/bin/env python3
"""Generate the fixed Chinese font subsets used by the 「运动榜」 application.

The baseline enables Montserrat only, which has no Chinese glyphs. This script
collects every character that the application actually puts into a C string
literal, adds printable ASCII, and asks the pinned `lv_font_conv` release to
render one subset per UI size.

Comments are stripped first and `ESP_LOG*()` calls are removed before the scan:
their text goes to the USB serial console, which does not use LVGL fonts, and
keeping those characters would embed unused glyphs in every size.

The generated `.c` files under `assets/fonts/` are committed, so a normal build
never needs Node.js or network access. Re-run this script after changing any
displayed text, then run `tests/test_sports_font_coverage.py` and the full gate.

Usage:
    python3 tools/gen_sports_fonts.py --font .env/fonts/NotoSansSC-Regular.ttf
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUTPUT_DIR = ROOT / "assets" / "fonts"
INVENTORY_FILE = OUTPUT_DIR / "sports_font_chars.txt"

# Sources whose string literals are rendered on the device.
SOURCES = ("main/sports_app.c", "main/sports_model.c", "main/main.c")

# Generated symbol name -> pixel size. 16px = hints/units, 20px = body,
# 28px = screen titles. The 40px large numbers use Montserrat from sdkconfig.
FONTS = (
    ("sports_font_16", 16),
    ("sports_font_20", 20),
    ("sports_font_28", 28),
)

ASCII_RANGE = "0x20-0x7E"
# Pinned converter: output format must match the LVGL version in dependencies.lock.
LV_FONT_CONV = "lv_font_conv@1.5.3"

STRING_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
LOG_CALL = re.compile(r"\bESP_LOG[A-Z]?\s*\(")


def strip_comments(text):
    """Remove // and /* */ comments while leaving string literals untouched."""
    out = []
    index = 0
    length = len(text)
    in_string = False
    while index < length:
        char = text[index]
        if in_string:
            out.append(char)
            if char == "\\" and index + 1 < length:
                out.append(text[index + 1])
                index += 2
                continue
            if char == '"':
                in_string = False
            index += 1
            continue
        if char == '"':
            in_string = True
            out.append(char)
            index += 1
            continue
        if text.startswith("//", index):
            newline = text.find("\n", index)
            index = length if newline < 0 else newline
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            index = length if end < 0 else end + 2
            continue
        out.append(char)
        index += 1
    return "".join(out)


def strip_log_calls(text):
    """Remove whole ESP_LOG*() calls, including their multi-line arguments."""
    while True:
        match = LOG_CALL.search(text)
        if not match:
            return text
        depth = 0
        index = match.end() - 1
        in_string = False
        while index < len(text):
            char = text[index]
            if in_string:
                if char == "\\":
                    index += 2
                    continue
                if char == '"':
                    in_string = False
            elif char == '"':
                in_string = True
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    break
            index += 1
        text = text[: match.start()] + text[index + 1 :]


def source_strings(relative_path):
    """Return the concatenated display strings declared in one source file."""
    text = (ROOT / relative_path).read_text(encoding="utf-8")
    text = strip_log_calls(strip_comments(text))
    return "".join(match.group(1) for match in STRING_LITERAL.finditer(text))


def collect_chars():
    """Return the sorted non-ASCII characters rendered on the display."""
    chars = set()
    for relative in SOURCES:
        for char in source_strings(relative):
            if ord(char) > 0x7F:
                chars.add(char)
    if not chars:
        raise SystemExit("no non-ASCII display characters found")
    return sorted(chars)


def collect_display_symbols():
    """Return every character (ASCII included) rendered on the display."""
    chars = {chr(code) for code in range(0x20, 0x7F)}
    for relative in SOURCES:
        chars.update(source_strings(relative))
    chars.discard("\n")
    return chars


def covered_codepoints(font_source):
    """Parse a generated font source for the glyphs it actually contains."""
    text = Path(font_source).read_text(encoding="utf-8")
    return {int(match.group(1), 16) for match in re.finditer(r"/\* U\+([0-9A-Fa-f]{4,6}) ", text)}


def run_converter(font, name, size, symbols):
    output = OUTPUT_DIR / (name + ".c")
    try:
        font_arg = str(font.relative_to(ROOT))
        output_arg = str(output.relative_to(ROOT))
    except ValueError:
        font_arg, output_arg = str(font), str(output)

    command = [
        "npx", "--yes", LV_FONT_CONV,
        "--font", font_arg,
        "--size", str(size),
        "--bpp", "4",
        "--format", "lvgl",
        "--no-compress",
        "--range", ASCII_RANGE,
        "--symbols", symbols,
        "--lv-font-name", name,
        "--lv-include", "lvgl.h",
        "-o", output_arg,
    ]
    print("[font] %s (%dpx)" % (name, size))
    result = subprocess.run(command, cwd=str(ROOT), capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit("lv_font_conv failed for " + name)
    if not output.is_file() or output.stat().st_size == 0:
        raise SystemExit("lv_font_conv produced no output for " + name)
    return output


def write_inventory(chars, font):
    lines = [
        "# 「运动榜」中文字体子集字符清单",
        "#",
        "# 由 tools/gen_sports_fonts.py 自动收集 main/ 下 C 字符串字面量里会被绘制到",
        "# 屏幕上的字符(注释与 ESP_LOG 日志文本不计入),再加上可打印 ASCII",
        "# (0x20-0x7E)后生成。大号数字使用 sdkconfig 启用的 Montserrat 40,不在此",
        "# 子集内。",
        "#",
        "# 源字体: Noto Sans SC Regular (SIL Open Font License 1.1)",
        "# 源文件: 自行下载的 NotoSansSC-Regular.ttf/.otf(不随仓库提交)",
        "# 转换器: " + LV_FONT_CONV + " --bpp 4 --no-compress --format lvgl",
        "#",
        "# 修改任何界面文字后请重新运行生成脚本,并执行 ./tools/validate.sh --static。",
        "",
        "ascii: 0x20-0x7E",
        "non_ascii_count: " + str(len(chars)),
        "non_ascii: " + "".join(chars),
        "codepoints: " + " ".join("U+%04X" % ord(char) for char in chars),
        "",
    ]
    INVENTORY_FILE.write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--font",
        default=os.environ.get("SPORTS_SOURCE_FONT", ""),
        help="path to NotoSansSC-Regular.ttf/.otf (SIL OFL 1.1)",
    )
    args = parser.parse_args()

    if not args.font:
        parser.error("--font is required (or set SPORTS_SOURCE_FONT)")
    font = Path(args.font).expanduser().resolve()
    if not font.is_file():
        parser.error("source font not found: %s" % font)

    chars = collect_chars()
    symbols = "".join(chars)
    print("[font] %d non-ASCII characters: %s" % (len(chars), symbols))

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    for name, size in FONTS:
        run_converter(font, name, size, symbols)
    write_inventory(chars, font)

    print("[font] wrote %s and %d font sources under %s/"
          % (INVENTORY_FILE.relative_to(ROOT), len(FONTS), OUTPUT_DIR.relative_to(ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
