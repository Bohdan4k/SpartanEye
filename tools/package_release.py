"""Build the release zip from a Release build: deterministic (fixed timestamps, sorted entries) + SHA-256 files.

Usage: python -I tools/package_release.py [--build build] [--out dist]
Zip layout (extract anywhere, run the exe):
  SpartanEye-<version>/SpartanEye.exe, metro_npc_overlay.dll, Create desktop shortcut.cmd,
  README.md, README.ru.md, CHANGELOG.md, LICENSE, THIRD_PARTY_LICENSES.md
Writes <zip>.sha256 ("<hash> *<name>", the format of sha256sum / Get-FileHash checks) and prints the hashes of the zip
and of both binaries. Run tools/privacy_scan.py on the zip before publishing it.
"""
import argparse
import hashlib
import os
import re
import sys
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARIES = ["SpartanEye.exe", "metro_npc_overlay.dll"]
DOCS = ["README.md", "README.ru.md", "CHANGELOG.md", "LICENSE", "THIRD_PARTY_LICENSES.md",
        os.path.join("dist", "Create desktop shortcut.cmd")]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def version():
    with open(os.path.join(REPO, "src", "version.h"), encoding="utf-8") as f:
        m = re.search(r'#define MOD_VERSION_STR "([^"]+)"', f.read())
    if not m:
        sys.exit("version not found in src/version.h")
    return m.group(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.join(REPO, "build"))
    ap.add_argument("--out", default=os.path.join(REPO, "dist"))
    a = ap.parse_args()
    ver = version()
    base = f"SpartanEye-{ver}"
    rel_dir = os.path.join(a.build, "Release")
    files = [(os.path.join(rel_dir, b), b) for b in BINARIES] + [(os.path.join(REPO, d), os.path.basename(d))
                                                                 for d in DOCS]
    for src, _ in files:
        if not os.path.isfile(src):
            sys.exit(f"missing: {src} (build Release first)")
    os.makedirs(a.out, exist_ok=True)
    zpath = os.path.join(a.out, base + ".zip")
    stamp = (2026, 1, 1, 0, 0, 0)  # fixed: the same inputs always give the same zip
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for src, name in sorted(files, key=lambda t: t[1].lower()):
            info = zipfile.ZipInfo(f"{base}/{name}", date_time=stamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(src, "rb") as f:
                z.writestr(info, f.read())
    digest = sha256(zpath)
    with open(zpath + ".sha256", "w", encoding="ascii", newline="\n") as f:
        f.write(f"{digest} *{os.path.basename(zpath)}\n")
    print(f"{os.path.basename(zpath)}  {os.path.getsize(zpath)} bytes")
    print(f"  SHA-256 {digest}")
    for b in BINARIES:
        print(f"  {b}: SHA-256 {sha256(os.path.join(rel_dir, b))}")


if __name__ == "__main__":
    main()
