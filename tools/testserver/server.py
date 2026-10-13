"""FTL3DS test server: watches GitHub branches, builds the headless game (test/headless) for each new
commit, runs every scenario test plus seed sweeps (tools/test/run_headless.py), keeps the results and serves a web
page to follow them. Python standard library only. See tools/testserver/README.md.

Settings (environment):
    REPO_URL      https://github.com/melonexe/FTL3DS.git
    BRANCHES      main,experimental/stereo-3d   (comma-separated)
    POLL_MINUTES  10      how often to look for new commits (0: only the Run buttons)
    SEEDS         10      sweep runs per scenario, with seeds 1..N
    JOBS          CPUs    tests run in parallel
    KEEP_RUNS     200     older results are deleted
    PORT          8080
Data (/data, a volume): ftl.dat (yours, read-only to the game), repo/ (a clone), runs/ (results).
"""
import json
import os
import re
import shutil
import subprocess
import threading
import time
import traceback
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

REPO_URL = os.environ.get("REPO_URL", "https://github.com/melonexe/FTL3DS.git")
BRANCHES = [b.strip() for b in os.environ.get("BRANCHES", "main,experimental/stereo-3d").split(",") if b.strip()]
POLL_MINUTES = float(os.environ.get("POLL_MINUTES", "10"))
SEEDS = int(os.environ.get("SEEDS", "10"))
JOBS = int(os.environ.get("JOBS", "0")) or os.cpu_count() or 2
KEEP_RUNS = int(os.environ.get("KEEP_RUNS", "200"))
PORT = int(os.environ.get("PORT", "8080"))
DATA = os.environ.get("DATA_DIR", "/data")
DAT = os.path.join(DATA, "ftl.dat")
REPO = os.path.join(DATA, "repo")
RUNS = os.path.join(DATA, "runs")
HERE = os.path.dirname(os.path.abspath(__file__))
GIT_ENV = dict(os.environ, GIT_TERMINAL_PROMPT="0")

lock = threading.Lock()
queue = deque()       # {"branch", "reason"}
current = None        # the run in progress (its meta, plus "phase" and "progress")
wake = threading.Event()
last_poll = {"at": None, "error": None}


def git(*args, cwd=REPO, check=True):
    p = subprocess.run(["git", *args], cwd=cwd, env=GIT_ENV, capture_output=True, text=True, timeout=600)
    if check and p.returncode:
        raise RuntimeError(f"git {' '.join(args)}: {p.stderr.strip()}")
    return p.stdout.strip()


def remote_head(branch):
    out = git("ls-remote", REPO_URL, f"refs/heads/{branch}", cwd=DATA)
    return out.split()[0] if out else None


def slug(s):
    return re.sub(r"[^A-Za-z0-9._-]+", "-", s)


def write_json(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f)
    os.replace(tmp, path)


def read_json(path, default=None):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def all_runs():
    if not os.path.isdir(RUNS):
        return []
    metas = [read_json(os.path.join(RUNS, d, "meta.json")) for d in os.listdir(RUNS)]
    return sorted((m for m in metas if m), key=lambda m: m["started"], reverse=True)


def tested(branch, sha):
    return any(m["branch"] == branch and m["sha"] == sha for m in all_runs())


def enqueue(branch, reason):
    with lock:
        if any(q["branch"] == branch for q in queue):  # already waiting
            return False
        queue.append({"branch": branch, "reason": reason})
    wake.set()
    return True


def summarize(results):
    base = [r for r in results if r["kind"] == "base"]
    sweep = [r for r in results if r["kind"] == "sweep"]
    serious = [r for r in results if r["status"] in ("crash", "hung", "timeout")]
    return {
        "scenarios": len(base),
        "base_passed": sum(r["status"] == "pass" for r in base),
        "base_failed": sum(r["status"] != "pass" for r in base),
        "sweep_runs": len(sweep),
        "sweep_passed": sum(r["status"] == "pass" for r in sweep),
        "sweep_failed": sum(r["status"] == "fail" for r in sweep),
        "crashes": sum(r["status"] == "crash" for r in results),
        "hangs": sum(r["status"] in ("hung", "timeout") for r in results),
        "serious": len(serious),
    }


def do_run(job):
    """Builds and tests the branch's latest commit. Returns the finished meta."""
    global current
    branch = job["branch"]
    if not os.path.isdir(os.path.join(REPO, ".git")):
        git("clone", "--no-checkout", REPO_URL, REPO, cwd=DATA)
    git("fetch", "--prune", "origin", f"+refs/heads/{branch}:refs/remotes/origin/{branch}")
    sha = git("rev-parse", f"origin/{branch}")
    git("checkout", "-f", "--detach", sha)
    git("clean", "-fdx", "-e", "test/headless/build/")  # keep the build for incremental rebuilds
    info = git("log", "-1", "--format=%an%x00%aI%x00%s", sha).split("\x00")
    run_id = time.strftime("%Y%m%d-%H%M%S") + f"-{slug(branch)}-{sha[:7]}"
    run_dir = os.path.join(RUNS, run_id)
    os.makedirs(run_dir)
    meta = {"id": run_id, "branch": branch, "sha": sha, "author": info[0], "date": info[1], "message": info[2],
            "reason": job["reason"], "started": time.time(), "finished": None, "status": "running", "seeds": SEEDS}
    with lock:
        current = dict(meta, phase="building", progress=None)
    write_json(os.path.join(run_dir, "meta.json"), meta)

    def finish(status, **extra):
        meta.update(extra, status=status, finished=time.time())
        write_json(os.path.join(run_dir, "meta.json"), meta)
        return meta

    if not os.path.isfile(os.path.join(REPO, "test", "headless", "Makefile")):
        return finish("no-headless", note="This commit has no headless build (test/headless): merge main into the branch.")
    if not os.path.isfile(DAT):
        return finish("error", note=f"No ftl.dat: put your copy at {DAT} (the data folder).")

    with open(os.path.join(run_dir, "build.log"), "w") as log:
        p = subprocess.run(["make", "-C", "test/headless", f"-j{JOBS}"], cwd=REPO, stdout=log, stderr=subprocess.STDOUT)
    if p.returncode:
        return finish("build-failed")

    with lock:
        current["phase"] = "testing"
    progress = os.path.join(run_dir, "progress.json")
    results_path = os.path.join(run_dir, "results.json")
    cmd = ["python3", "tools/test/run_headless.py", "--dat", DAT, "--seeds", str(SEEDS), "--jobs", str(JOBS),
           "--json", results_path, "--progress", progress]
    with open(os.path.join(run_dir, "runner.log"), "w") as log:
        proc = subprocess.Popen(cmd, cwd=REPO, stdout=log, stderr=subprocess.STDOUT)
        while proc.poll() is None:
            with lock:
                current["progress"] = read_json(progress)
            time.sleep(1)
    results = read_json(results_path)
    if results is None:
        return finish("error", note="The test runner wrote no results: see runner.log.")
    summary = summarize(results)
    status = "passed" if not summary["base_failed"] and not summary["serious"] else "failed"
    return finish(status, summary=summary)


def worker():
    global current
    while True:
        wake.wait(5)
        wake.clear()
        while True:
            with lock:
                job = queue.popleft() if queue else None
            if not job:
                break
            try:
                do_run(job)
            except Exception:
                traceback.print_exc()
                err = traceback.format_exc(limit=3)
                if current and current.get("id"):
                    meta = read_json(os.path.join(RUNS, current["id"], "meta.json")) or dict(current)
                    meta.update(status="error", note=err, finished=time.time())
                    meta.pop("phase", None), meta.pop("progress", None)
                    write_json(os.path.join(RUNS, current["id"], "meta.json"), meta)
                else:
                    print(f"run of {job['branch']} failed before starting:\n{err}", flush=True)
            finally:
                with lock:
                    current = None
                prune()


def prune():
    runs = all_runs()
    for m in runs[KEEP_RUNS:]:
        shutil.rmtree(os.path.join(RUNS, m["id"]), ignore_errors=True)


def poller():
    while True:
        try:
            for b in BRANCHES:
                sha = remote_head(b)
                if sha and not tested(b, sha):
                    enqueue(b, "new commit")
            last_poll.update(at=time.time(), error=None)
        except Exception as e:
            last_poll.update(at=time.time(), error=str(e))
        if POLL_MINUTES <= 0:
            return
        time.sleep(POLL_MINUTES * 60)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else (json.dumps(body) if ctype == "application/json" else body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype + ("; charset=utf-8" if ctype.startswith("text") else ""))
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urlparse(self.path)
        parts = [p for p in url.path.split("/") if p]
        q = parse_qs(url.query)
        if not parts:
            with open(os.path.join(HERE, "index.html"), "rb") as f:
                return self.send(200, f.read(), "text/html")
        if parts == ["api", "state"]:
            runs = all_runs()
            with lock:
                cur, qd = (dict(current) if current else None), list(queue)
            latest = {b: next((m for m in runs if m["branch"] == b), None) for b in BRANCHES}
            return self.send(200, {
                "current": cur, "queue": qd, "latest": latest, "dat": os.path.isfile(DAT),
                "poll": last_poll, "config": {"repo": REPO_URL, "branches": BRANCHES, "poll_minutes": POLL_MINUTES,
                                              "seeds": SEEDS, "jobs": JOBS}})
        if parts == ["api", "runs"]:
            return self.send(200, all_runs()[: int(q.get("limit", ["100"])[0])])
        if len(parts) >= 3 and parts[:2] == ["api", "runs"]:
            run_dir = os.path.join(RUNS, slug(parts[2]))
            meta = read_json(os.path.join(run_dir, "meta.json"))
            if not meta:
                return self.send(404, {"error": "no such run"})
            if len(parts) == 3:
                results = read_json(os.path.join(run_dir, "results.json"), [])
                for r in results:
                    r.pop("log", None), r.pop("stderr", None)
                return self.send(200, {"meta": meta, "results": results})
            if parts[3] in ("build.log", "runner.log"):
                try:
                    with open(os.path.join(run_dir, parts[3]), encoding="utf-8", errors="replace") as f:
                        return self.send(200, f.read()[-200000:], "text/plain")
                except OSError:
                    return self.send(404, "none", "text/plain")
            if parts[3] == "log":  # ?scenario=&seed=
                name, seed = q.get("scenario", [""])[0], q.get("seed", [""])[0]
                for r in read_json(os.path.join(run_dir, "results.json"), []):
                    if r["scenario"] == name and str(r["seed"] if r["seed"] is not None else "") == seed:
                        return self.send(200, r["log"] + ("\n--- stderr ---\n" + r["stderr"] if r["stderr"].strip() else ""), "text/plain")
                return self.send(404, "no such result", "text/plain")
        if len(parts) == 3 and parts[:2] == ["api", "history"]:  # one scenario across recent runs
            out = []
            for m in all_runs()[:40]:
                res = read_json(os.path.join(RUNS, m["id"], "results.json"), [])
                mine = [r for r in res if r["scenario"] == parts[2]]
                if mine:
                    base = next((r["status"] for r in mine if r["kind"] == "base"), None)
                    out.append({"id": m["id"], "branch": m["branch"], "sha": m["sha"][:7], "base": base,
                                "sweep": [r["status"] for r in mine if r["kind"] == "sweep"]})
            return self.send(200, out)
        self.send(404, {"error": "not found"})

    def do_POST(self):
        parts = [p for p in urlparse(self.path).path.split("/") if p]
        if parts == ["api", "run"]:
            n = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(n) or b"{}")
            branch = body.get("branch")
            if branch not in BRANCHES:
                return self.send(400, {"error": f"unknown branch {branch}"})
            return self.send(200, {"queued": enqueue(branch, "run button")})
        if parts == ["api", "poll"]:
            threading.Thread(target=lambda: [enqueue(b, "new commit") for b in BRANCHES
                                             if (s := remote_head(b)) and not tested(b, s)], daemon=True).start()
            return self.send(200, {"ok": True})
        self.send(404, {"error": "not found"})


def main():
    os.makedirs(RUNS, exist_ok=True)
    # Runs cut off by a restart: mark them.
    for m in all_runs():
        if m["status"] == "running":
            m.update(status="error", note="The server stopped during this run.", finished=m.get("finished") or time.time())
            write_json(os.path.join(RUNS, m["id"], "meta.json"), m)
    threading.Thread(target=worker, daemon=True).start()
    threading.Thread(target=poller, daemon=True).start()
    print(f"FTL3DS test server on :{PORT}, watching {', '.join(BRANCHES)} of {REPO_URL}", flush=True)
    ThreadingHTTPServer(("", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
