#!/usr/bin/env python3
"""
Python prototype of the C case-insensitive path resolver in
runtime/win32/path_resolve.c. Used to validate the algorithm (the thing that
broke the original source port) without needing an iOS toolchain.

Run: python3 tools/path_resolve_proto.py
"""
import os, tempfile, shutil

class PathResolver:
    def __init__(self, root):
        self.root = root.replace("\\", "/")
        self.index = {}   # lowercased normalized -> real path
        self.cwd = self.root
        self._index(self.root)

    def _norm(self, p):
        p = p.replace("\\", "/")
        out = []
        for seg in p.split("/"):
            if seg in ("", "."):
                continue
            if seg == "..":
                if out:
                    out.pop()
            else:
                out.append(seg)
        return "/".join(out)

    def _index(self, d):
        for name in os.listdir(d):
            full = os.path.join(d, name)
            norm = self._norm(full)
            self.index[norm.lower()] = full
            if os.path.isdir(full):
                self._index(full)

    def resolve(self, inp):
        inp = inp.replace("\\", "/")
        if not (inp.startswith("/") or (len(inp) >= 2 and inp[1] == ":")):
            inp = self.cwd.rstrip("/") + "/" + inp
        norm = self._norm(inp)
        low = norm.lower()
        return self.index.get(low, norm)  # fallback to literal if missing

def main():
    tmp = tempfile.mkdtemp()
    try:
        base = os.path.join(tmp, "game")
        d = os.path.join(base, "assets", "songs", "Week1")
        os.makedirs(d)
        open(os.path.join(d, "Icon.PNG"), "w").write("x")
        open(os.path.join(base, "assets", "README.TXT"), "w").write("y")

        r = PathResolver(base)
        r1 = r.resolve("assets\\songs\\week1\\icon.png")  # backslash + wrong case
        r2 = r.resolve("ASSETS/readme.txt")
        print("index_size =", len(r.index))
        print("r1 =", r1)
        print("r2 =", r2)
        ok = (r1.endswith("Icon.PNG") and "Week1" in r1 and
              r2.endswith("README.TXT"))
        print("RESULT:", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

if __name__ == "__main__":
    raise SystemExit(main())
