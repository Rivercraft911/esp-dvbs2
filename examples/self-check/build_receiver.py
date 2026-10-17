#!/usr/bin/env python3
"""Build the separately licensed LeanDVB under ignored notes/. No device access."""
import argparse
from pathlib import Path
import subprocess
import sys

REVISION = "84c59e1c7a1a79338d5722d63f28640cc9d350f3"
ROOT = Path(__file__).resolve().parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("directory", nargs="?", type=Path, default=ROOT/"notes/leansdr")
    a = p.parse_args()
    path = a.directory.resolve()
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "--no-checkout", "https://github.com/pabr/leansdr.git", str(path)], check=True)
        subprocess.run(["git", "-C", str(path), "checkout", REVISION], check=True)
    actual = subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
    if actual != REVISION:
        p.error(f"receiver must be at {REVISION}; use a new directory to preserve existing work")
    # Independent portability edits to the external receiver only. No receiver
    # source is linked, imported, or distributed as part of esp-dvbs2.
    changes = {
        "src/leansdr/sdr.h": [("exp10f(-mer/20)", "powf(10.0f, -mer/20)")],
        "src/leansdr/bch.h": [("TGF C[NN]={1,}, B[NN]={1,};",
                               "TGF C[NN], B[NN];\n      memset(C, 0, sizeof(C));\n      memset(B, 0, sizeof(B));\n      C[0] = B[0] = 1;")],
        "src/leansdr/dvbs2.h": [
            ("const int search_range = modcod_info::MIN_SYMBOLS_PER_FRAME;",
             "const int search_range = modcod_info::MAX_SYMBOLS_PER_FRAME;"),
            ("int child = vfork();", "int child = fork();")],
    }
    if sys.platform == "darwin":
        changes["src/apps/leandvb.cc"] = [
            ("if ( cfg.input_pipe ) {", "if ( false ) { // pipe resizing is Linux-only"),
            ("F_SETPIPE_SZ", "0")]
        changes["src/leansdr/dvbs2.h"].append(("F_SETPIPE_SZ", "0"))
        # The only pipe-resizing block in this header is unused by our internal decoder.
    for name, edits in changes.items():
        file = path/name
        text = file.read_text()
        for before, after in edits:
            if before not in text:
                if after not in text:
                    p.error(f"unrecognized receiver source: {name}: {before}")
                continue
            text = text.replace(before, after)
        file.write_text(text)
    flags = '-O3 -I.. -DLEANSDR_EXTENSIONS -DVERSION="local" -Wno-deprecated-declarations'
    subprocess.run(["make", "-C", str(path/"src/apps"), "VERSION=local",
                    "CXXFLAGS="+flags.replace('"', '\\"'), "leandvb"], check=True)
    print(path/"src/apps/leandvb")


if __name__ == "__main__":
    main()
