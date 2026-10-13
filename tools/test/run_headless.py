#!/usr/bin/env python3
"""Runs scenario tests on the headless PC build (test/headless) and reports the results as JSON.

    make -C test/headless
    python3 tools/test/run_headless.py --dat /path/to/ftl.dat [--filter 'boss*'] [--seeds 20] [--jobs 4]
                               [--json results.json] [--progress progress.json]

Each scenario (tools/test/scenarios/*.txt) runs once as written: that's the "base" run, and its
checks must pass, as in Azahar. --seeds N adds N "sweep" runs with the seed replaced (1..N). A
scenario's checks are written for its own seed, so a sweep's failed check is only a warning; a
sweep that crashes or never finishes is an error.

Every run gets its own SD card folder; ftl.dat is shared read-only (it's read as romfs:/ftl.dat).
Time inside the game is simulated, so runs are repeatable and take seconds. Screenshots ("shot")
are skipped. Exit code: 1 if any base run failed or any run crashed or hung.
"""
import argparse
import concurrent.futures
import fnmatch
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LINE = re.compile(r"\[\s*([\d.]+)\]\s+(PASS|FAIL|ERROR|DONE|SHOT|STATE)\b(.*)")


def scenario_end(text):
    """The latest 'at' time in a scenario (seconds), to bound the simulated run."""
    times = [float(m.group(1)) for m in re.finditer(r"^\s*at\s+([\d.]+)", text, re.M)]
    return max(times, default=0.0)


def with_seed(text, seed):
    if seed is None:
        return text
    lines = [ln for ln in text.splitlines() if not re.match(r"\s*seed\s+", ln)]
    return f"seed {seed}\n" + "\n".join(lines) + "\n"


def run_one(game, romfs, name, text, seed, wall_timeout):
    """One run: returns a result dict."""
    work = tempfile.mkdtemp(prefix="ftl3ds_run_")
    sd = os.path.join(work, "3ds", "ftl3ds")
    os.makedirs(sd)
    with open(os.path.join(sd, "test.txt"), "w") as f:
        f.write(with_seed(text, seed))
    env = dict(os.environ, FTL3DS_SDMC=work, FTL3DS_ROMFS=romfs,
               FTL3DS_MAX_SECONDS=str(scenario_end(text) + 60))
    t0 = time.time()
    status, stderr = None, ""
    try:
        p = subprocess.run([game], env=env, cwd=work, capture_output=True, text=True, timeout=wall_timeout)
        stderr = p.stderr
        if p.returncode < 0 or p.returncode > 1:
            status = "crash"
            stderr += f"\n(exit code {p.returncode})"
    except subprocess.TimeoutExpired as e:
        status = "timeout"
        stderr = (e.stderr or b"").decode("utf-8", "replace") if isinstance(e.stderr, bytes) else (e.stderr or "")
    wall = time.time() - t0
    log = ""
    try:
        with open(os.path.join(sd, "test.log"), encoding="utf-8", errors="replace") as f:
            log = f.read()
    except OSError:
        pass
    shutil.rmtree(work, ignore_errors=True)

    counts = {"PASS": 0, "FAIL": 0, "ERROR": 0}
    done, sim = False, 0.0
    for ln in log.splitlines():
        m = LINE.search(ln)
        if not m:
            continue
        sim = max(sim, float(m.group(1)))
        kind = m.group(2)
        if kind in counts:
            counts[kind] += 1
        if kind == "DONE":
            done = True
    if status is None:
        if not done:
            status = "hung"  # ran out of simulated time without reaching "done"
        elif counts["FAIL"] or counts["ERROR"]:
            status = "fail"
        else:
            status = "pass"
    return {
        "scenario": name, "seed": seed, "kind": "base" if seed is None else "sweep", "status": status,
        "passed": counts["PASS"], "failed": counts["FAIL"], "errors": counts["ERROR"],
        "sim_seconds": round(sim, 2), "wall_seconds": round(wall, 2), "log": log, "stderr": stderr[-4000:],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dat", required=True, help="ftl.dat (or the folder holding it)")
    ap.add_argument("--game", default=os.path.join(ROOT, "test", "headless", "build", "ftl3ds_headless"))
    ap.add_argument("--scenarios", default=os.path.join(HERE, "scenarios"))
    ap.add_argument("--filter", default="*", help="scenario name pattern, e.g. 'boss*'")
    ap.add_argument("--seeds", type=int, default=0, help="extra runs per scenario with seeds 1..N")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--timeout", type=float, default=300, help="wall-clock seconds per run")
    ap.add_argument("--json", help="write all results here")
    ap.add_argument("--progress", help="keep this file updated with {done, total} while running")
    args = ap.parse_args()

    dat = os.path.abspath(args.dat)
    romfs = dat if os.path.isdir(dat) else os.path.dirname(dat)
    if not os.path.isfile(os.path.join(romfs, "ftl.dat")):
        sys.exit(f"no ftl.dat in {romfs}")
    if not os.path.isfile(args.game):
        sys.exit(f"no {args.game}: build it with make -C test/headless")

    jobs = []
    for fn in sorted(os.listdir(args.scenarios)):
        name = fn[:-4]
        if not fn.endswith(".txt") or not fnmatch.fnmatch(name, args.filter):
            continue
        with open(os.path.join(args.scenarios, fn), encoding="utf-8") as f:
            text = f.read()
        jobs.append((name, text, None))
        jobs += [(name, text, s) for s in range(1, args.seeds + 1)]

    results, lock, t0 = [], threading.Lock(), time.time()

    def progress():
        if args.progress:
            tmp = args.progress + ".tmp"
            with open(tmp, "w") as f:
                json.dump({"done": len(results), "total": len(jobs)}, f)
            os.replace(tmp, args.progress)

    progress()
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        futs = [ex.submit(run_one, args.game, romfs, n, t, s, args.timeout) for n, t, s in jobs]
        for fut in concurrent.futures.as_completed(futs):
            r = fut.result()
            with lock:
                results.append(r)
                progress()
            if r["status"] != "pass":
                tag = r["status"].upper() + ("" if r["kind"] == "base" else f" (seed {r['seed']})")
                print(f"{r['scenario']}: {tag}", flush=True)

    results.sort(key=lambda r: (r["scenario"], r["seed"] is not None, r["seed"] or 0))
    bad = [r for r in results if (r["kind"] == "base" and r["status"] != "pass") or r["status"] in ("crash", "hung", "timeout")]
    base = [r for r in results if r["kind"] == "base"]
    print(f"{sum(r['status'] == 'pass' for r in base)}/{len(base)} scenarios passed, "
          f"{len(results) - len(base)} sweep runs, {len(bad)} problems, {time.time() - t0:.1f} s")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
