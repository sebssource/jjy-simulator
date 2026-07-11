# PlatformIO pre-build script: write git commit info into include/gitversion.h.
import subprocess
from SCons.Script import Import

Import("env")


def run_git(args):
    try:
        return subprocess.check_output(["git"] + args, stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return ""


commit = run_git(["rev-parse", "--short", "HEAD"])
dirty = run_git(["status", "--porcelain"]) != ""

lines = ["#pragma once", ""]
if commit:
    lines.append('#define GIT_COMMIT_SHORT "%s"' % commit)
else:
    lines.append("// GIT_COMMIT_SHORT not defined (not a git repo / git unavailable)")
lines.append("#define GIT_DIRTY %d" % (1 if dirty else 0))
lines.append("")

with open("include/gitversion.h", "w") as f:
    f.write("\n".join(lines) + "\n")

print("[gitversion] commit=%s dirty=%s" % (commit or "unknown", dirty))
