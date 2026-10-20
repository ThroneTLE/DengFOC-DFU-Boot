"""Copy a Synex installation into a project, so each project has its own Synex
and its own config.ini.

Synex is portable: everything it saves goes to config.ini next to Synex.exe
(plus plot_data.* history and SynexLog.txt), it bundles the Qt and MSVC
runtimes, and needs nothing from the registry. This copies the program files
only; the update cache, backups, plot history and logs stay behind.

  python -X utf8 -I install_synex.py --from "E:/User/Desk/Synex_v3.0.5" [--to ../../Synex] [--config FILE]

--config seeds the project's config.ini (default: keep the project's existing
one, else take the source's). Refuses to overwrite while the target Synex runs.
"""
import argparse
import fnmatch
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_TO = os.path.normpath(os.path.join(HERE, "..", "..", "Synex"))

SKIP_DIRS = {"update"}                       # updater cache (~190 MB)
SKIP_FILES = ["config*.ini", "*.bak*", "*.bak", "plot_data*", "SynexLog.txt", "update.log"]


def running_from(folder):
    """True if a Synex.exe started from `folder` is running."""
    ps = "(Get-Process Synex -ErrorAction SilentlyContinue | ForEach-Object { $_.Path }) -join \"`n\""
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    want = os.path.normcase(os.path.abspath(os.path.join(folder, "Synex.exe")))
    return any(os.path.normcase(os.path.abspath(p.strip())) == want for p in out.splitlines() if p.strip())


def skipped(name):
    return any(fnmatch.fnmatch(name, pat) for pat in SKIP_FILES)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="src", required=True, help="existing Synex folder")
    ap.add_argument("--to", dest="dst", default=DEFAULT_TO)
    ap.add_argument("--config", help="config.ini to use for this project")
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.src, "Synex.exe")):
        sys.exit(f"no Synex.exe in {args.src}")
    if running_from(args.dst):
        sys.exit(f"Synex is running from {args.dst}: close it first")

    copied = 0
    size = 0
    for root, dirs, files in os.walk(args.src):
        rel = os.path.relpath(root, args.src)
        if rel == ".":
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        out = os.path.join(args.dst, rel) if rel != "." else args.dst
        os.makedirs(out, exist_ok=True)
        for f in files:
            if rel == "." and skipped(f):
                continue
            shutil.copy2(os.path.join(root, f), os.path.join(out, f))
            copied += 1
            size += os.path.getsize(os.path.join(root, f))

    cfg = os.path.join(args.dst, "config.ini")
    if args.config:
        shutil.copy2(args.config, cfg)
        note = f"config.ini from {args.config}"
    elif os.path.exists(cfg):
        note = "kept the project's config.ini"
    else:
        shutil.copy2(os.path.join(args.src, "config.ini"), cfg)
        note = "config.ini copied from the source"
    print(f"{copied} files, {size / 1e6:.0f} MB -> {args.dst}; {note}")


if __name__ == "__main__":
    main()
