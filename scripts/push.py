#!/usr/bin/env python3
"""
Push the project files to GitHub via the REST Contents API (no git, works
around the blocked github.com HTTPS on this machine). Reads the token from
/tmp/ghtoken (NOT embedded, NOT committed). Idempotent: updates if a file
already exists.
"""
import os, base64, json, urllib.request, urllib.error, sys

TOKEN_FILE = "/tmp/ghtoken"
REPO = "Aiden6652/corruption-ios-runtime"
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
API = f"https://api.github.com/repos/{REPO}/contents/"

SKIP_DIRS = {".git", "build", "unicorn"}

def token():
    import os
    t = os.environ.get("GITHUB_TOKEN")
    if t:
        return t.strip()
    if os.path.exists(TOKEN_FILE):
        with open(TOKEN_FILE) as f:
            return f.read().strip()
    raise SystemExit("no GITHUB_TOKEN env / " + TOKEN_FILE)

def api_request(method, url, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", "Bearer " + token())
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode())

def walk():
    for dirpath, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for fn in files:
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, ROOT).replace("\\", "/")
            yield rel, full

def main():
    count, ok, fail = 0, 0, 0
    for rel, full in walk():
        count += 1
        with open(full, "rb") as f:
            content = base64.b64encode(f.read()).decode()
        url = API + rel
        status, _ = api_request("GET", url)
        body = {"message": f"add {rel}", "content": content}
        if status == 200:
            sha = api_request("GET", url)[1]["sha"]
            body["sha"] = sha
            body["message"] = f"update {rel}"
        st, resp = api_request("PUT", url, body)
        if st in (200, 201):
            ok += 1
            print(f"[ok] {rel}")
        else:
            fail += 1
            print(f"[FAIL {st}] {rel}: {resp.get('message') if isinstance(resp, dict) else resp}")
    print(f"\n=== pushed {ok}/{count} (failed {fail}) ===")
    sys.exit(1 if fail else 0)

if __name__ == "__main__":
    main()
