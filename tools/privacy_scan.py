"""Privacy scan: fails (exit 1) if any file, archive or git history contains personal data. Stdlib only.

Usage:
  python -I tools/privacy_scan.py PATH [PATH ...] [--patterns FILE] [--allow FILE] [--git REPO_DIR]

PATH: files, folders (recursive) and .zip archives (every member is scanned decompressed).
Checks, each as text and inside binaries (ASCII and UTF-16 strings), case-insensitive:
  1. personal strings: one per line in --patterns FILE (keep that file OUTSIDE the repository) plus this machine's
     user name, computer name and profile folder, taken from the environment automatically;
  2. absolute local paths: drive-letter paths (C:\\..., D:/...), \\Users\\, /c/Users/, /home/<name>, AppData;
  3. Steam IDs (17-digit SteamID64) and Steam "userdata" folders;
  4. e-mail addresses (only those listed with --allow pass, e.g. the author's GitHub noreply address);
  5. Windows executables: a debug directory (CodeView record = PDB file name and path) or any ".pdb" string;
  6. --git REPO_DIR: every commit's author / committer / message (all refs) and the repository's .git/config.
Allowed literals (--allow FILE, one per line) are removed from a match before it counts (e-mails, example paths).
"""
import argparse
import io
import os
import re
import struct
import subprocess
import sys
import zipfile
import zlib

TEXT_EXT = {".md", ".txt", ".cpp", ".h", ".c", ".rc", ".py", ".yml", ".yaml", ".json", ".cmd", ".bat", ".ini",
            ".bbcode", ".cmake", ".gitignore", ".gitattributes", ".html", ".csv", ""}
GENERIC = [
    ("local path", rb"(?<![A-Za-z0-9])[A-Za-z]:(?:\\\\?|/)[A-Za-z0-9_$%(.~ -]{3}"),
    ("user folder", rb"[\\/](?:Users|Documents and Settings)[\\/]"),
    ("user folder", rb"(?-i:/home/[a-z0-9_.-]+)"),
    ("AppData", rb"AppData"),
    ("Steam ID", rb"(?<!\d)7656119\d{10}(?!\d)"),
    ("Steam userdata", rb"userdata[\\/]\d+"),
    ("e-mail", rb"[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}"),
    ("PDB", rb"\.pdb\b"),
]


def strings_of(data):
    """Printable ASCII runs and UTF-16LE runs (>= 4 chars), one per line."""
    out = [m.group() for m in re.finditer(rb"[\x20-\x7E]{4,}", data)]
    for m in re.finditer(rb"(?:[\x20-\x7E]\x00){4,}", data):
        out.append(m.group().decode("utf-16le").encode("ascii", "replace"))
    return b"\n".join(out)


# Debug-directory entry types that carry no path or name: POGO (13, linker section names), REPRO (16, build hash),
# VC_FEATURE (12), EX_DLLCHARACTERISTICS (20). Anything else, above all CodeView (2 = PDB path), fails.
HARMLESS_DEBUG_TYPES = {12, 13, 16, 20}


def pe_debug_entries(data):
    """(debug-entry types, CodeView PDB path) of a PE image; ([], b'') when it is not a PE or has no debug directory."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        return [], b""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 0x18 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        return [], b""
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)
    rva, size = struct.unpack_from("<II", data, dd + 6 * 8)  # IMAGE_DIRECTORY_ENTRY_DEBUG = 6
    if not rva or not size:
        return [], b""
    secs = []
    for i in range(nsec):
        o = opt + opt_size + 40 * i
        vsize, va, rsize, rptr = struct.unpack_from("<IIII", data, o + 8)
        secs.append((va, max(vsize, rsize), rptr))
    o = next((rp + rva - va for va, sz, rp in secs if va <= rva < va + sz), None)
    types, pdb = [], b""
    if o is not None:
        for i in range(size // 28):
            typ, dsize, draw, dptr = struct.unpack_from("<IIII", data, o + 28 * i + 12)
            types.append(typ)
            if typ == 2 and data[dptr:dptr + 4] == b"RSDS":
                pdb = data[dptr + 24:dptr + dsize].split(b"\0")[0]
    return types, pdb


def png_text(data):
    """Text and metadata chunks of a PNG (tEXt / iTXt / zTXt / eXIf), the only places a PNG can carry personal data;
    the compressed pixels are noise to a string search."""
    out, pos = [], 8
    while pos + 8 <= len(data):
        n, typ = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + n]
        if typ in (b"tEXt", b"iTXt", b"eXIf"):
            out.append(body)
        elif typ == b"zTXt":
            k = body.find(b"\0")
            try:
                out.append(body[:k] + b" " + zlib.decompress(body[k + 2:]))
            except zlib.error:
                out.append(body)
        pos += 12 + n
    return b"\n".join(out)


class Scanner:
    def __init__(self, personal, allow):
        self.personal = [p for p in personal if len(p) >= 3]
        self.allow = [a.lower() for a in allow]
        self.hits = []
        self.files = 0

    def check(self, where, data, binary, generic=True):
        self.files += 1
        if binary and data[:8] == b"\x89PNG\r\n\x1a\n":
            text = png_text(data)
        elif binary:
            types, pdb = pe_debug_entries(data)
            bad = [t for t in types if t not in HARMLESS_DEBUG_TYPES]
            if pdb or bad:
                self.hits.append((where, "PE debug directory", pdb.decode("latin-1") or f"entry types {bad}"))
            text = strings_of(data)
        else:
            text = data
            if text[:2] in (b"\xff\xfe", b"\xfe\xff"):
                text = text.decode("utf-16").encode("utf-8", "replace")
        low = text.lower()
        for p in self.personal:
            if len(p) < 5:  # short names (a computer or user name that is also a word): whole word, exact case
                m = re.search(rb"(?<![A-Za-z0-9_])" + re.escape(p.encode("utf-8")) + rb"(?![A-Za-z0-9_])", text)
                i = m.start() if m else -1
            else:
                i = low.find(p.lower().encode("utf-8"))
            if i >= 0:
                self.hits.append((where, "personal string", self.context(text, i, len(p))))
        for label, rx in GENERIC if generic else []:
            for m in re.finditer(rx, text, re.I):
                ctx = self.context(text, m.start(), m.end() - m.start())
                if any(a in ctx.lower() for a in self.allow):
                    continue
                self.hits.append((where, label, ctx))

    @staticmethod
    def context(text, i, n):
        s = text[max(0, i - 30):i + n + 30]
        return s.decode("utf-8", "replace").replace("\n", " ").replace("\r", " ").strip()

    def scan_bytes(self, where, data):
        ext = os.path.splitext(where)[1].lower()
        if ext == ".zip" or data[:4] == b"PK\x03\x04":
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                for info in z.infolist():
                    self.check(f"{where}!{info.filename}", info.filename.encode("utf-8"), False)
                    if not info.is_dir():
                        self.scan_bytes(f"{where}!{info.filename}", z.read(info))
            return
        binary = ext not in TEXT_EXT or b"\0" in data[:4096] and data[:2] not in (b"\xff\xfe", b"\xfe\xff")
        # This script describes the generic patterns in its own text: check it for personal strings only.
        self.check(where, data, binary, generic=os.path.basename(where) != "privacy_scan.py")

    def scan_path(self, path):
        if os.path.isdir(path):
            for dirpath, dirnames, filenames in os.walk(path):
                dirnames[:] = [d for d in dirnames if d not in (".git", "build", "__pycache__", ".vs")]
                for fn in filenames:
                    full = os.path.join(dirpath, fn)
                    rel = os.path.relpath(full, path)
                    self.check(rel + " (file name)", rel.encode("utf-8"), False)
                    with open(full, "rb") as f:
                        self.scan_bytes(rel, f.read())
        else:
            with open(path, "rb") as f:
                self.scan_bytes(os.path.basename(path), f.read())

    def scan_git(self, repo):
        log = subprocess.run(["git", "-C", repo, "log", "--all", "--format=%H%n%an <%ae>%n%cn <%ce>%n%B"],
                             capture_output=True)
        if log.returncode == 0:
            self.check("git history", log.stdout, False)
        cfg = os.path.join(repo, ".git", "config")
        if os.path.isfile(cfg):
            with open(cfg, "rb") as f:
                self.check(".git/config", f.read(), False)


def machine_strings():
    out = []
    for var in ("USERNAME", "COMPUTERNAME", "USERDOMAIN", "LOGNAME", "USER"):
        v = os.environ.get(var, "")
        if v and v.upper() not in ("SYSTEM", "WORKGROUP"):
            out.append(v)
    home = os.environ.get("USERPROFILE") or os.environ.get("HOME") or ""
    if home:
        out.append(os.path.basename(home.rstrip("\\/")))
    return sorted(set(out))


def read_list(path):
    if not path:
        return []
    with open(path, encoding="utf-8") as f:
        return [ln.strip() for ln in f if ln.strip() and not ln.lstrip().startswith("#")]


def main():
    ap = argparse.ArgumentParser(description="Fail on personal data in files, archives and git history.")
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--patterns", help="file with personal strings, one per line (keep it outside the repo)")
    ap.add_argument("--allow", help="file with allowed literals (e.g. the author's noreply e-mail)")
    ap.add_argument("--git", help="also scan this repository's history and .git/config")
    a = ap.parse_args()
    sys.stdout.reconfigure(errors="replace")  # consoles with a legacy code page (cp1251, cp437) must not crash us
    personal = read_list(a.patterns) + machine_strings()
    s = Scanner(personal, read_list(a.allow))
    for p in a.paths:
        s.scan_path(p)
    if a.git:
        s.scan_git(a.git)
    print(f"privacy scan: {s.files} items, {len(personal)} personal strings "
          f"({len(read_list(a.patterns))} from the pattern file, {len(personal) - len(read_list(a.patterns))} "
          f"from this machine), {len(GENERIC)} generic checks")
    for where, label, ctx in s.hits:
        print(f"  HIT [{label}] {where}: ...{ctx}...")
    print("RESULT: FAIL" if s.hits else "RESULT: PASS (no personal data found)")
    return 1 if s.hits else 0


if __name__ == "__main__":
    sys.exit(main())
