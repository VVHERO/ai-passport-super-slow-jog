#!/usr/bin/env python3
"""Syntax-check the app sources with the exact flags IDF uses, without a full build.

Usage: python tools/syntax_check.py [file-name ...]
Example: python tools/syntax_check.py ssj_app.c ssj_audio.c

It reuses <build>/compile_commands.json and replaces "-o <out>" with "-fsyntax-only".
Arguments are passed as a list (no shell) so the long include list stays legal on Windows.
"""

import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

BUILD = Path(os.environ.get("TEMP", "/tmp")) / "ssj-firmware"
DB = BUILD / "compile_commands.json"
DEFAULT = ["ssj_app.c", "ssj_audio.c", "ssj_logic.c", "ssj_store.c", "main.c"]


def main():
    keys = sys.argv[1:] or DEFAULT
    entries = json.loads(DB.read_text(encoding="utf-8"))
    rc = 0
    for key in keys:
        match = [e for e in entries if e["file"].replace("\\", "/").endswith("/main/" + key)]
        if not match:
            print("SKIP %s: not in compile_commands.json" % key)
            continue
        e = match[0]
        cmd = re.sub(r" -o \S+", " -fsyntax-only", e["command"])
        argv = [a.strip('"') for a in shlex.split(cmd, posix=False)]
        print("CHECK %s" % key)
        r = subprocess.run(argv, cwd=e["directory"], capture_output=True, text=True)
        if r.stdout.strip():
            print(r.stdout)
        if r.stderr.strip():
            print(r.stderr)
        if r.returncode != 0:
            rc = 1
    print("SYNTAX CHECK: %s" % ("PASS" if rc == 0 else "FAIL"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
