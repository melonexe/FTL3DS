# FTL3DS test server

A small Docker app that tests every new commit automatically and shows the results in a web page:

1. Every few minutes it checks the GitHub branches it watches (`main` and `experimental/stereo-3d`
   by default) for new commits. A **Run now** button tests a branch on demand.
2. It builds the **headless game** ([`test/headless`](../../test/headless)) from that commit: the
   real game, test build included, compiled for Linux with nothing drawn or played and a simulated
   clock, so a test that takes a minute in Azahar takes a second or two. A branch made before the
   headless build existed shows **no-headless** until `main` is merged into it.
3. It runs every scenario in [`tools/test/scenarios`](../test/scenarios) as written (these must
   pass), plus a **seed sweep**: each scenario again with seeds 1..N. A scenario's checks are
   written for its own seed, so a failed check in the sweep is only a warning (amber); a crash or a
   run that never finishes is an error (red).
4. The page shows live progress, the latest result per branch, every run with its logs, and each
   scenario's history across runs (handy for spotting flaky tests).

It uses little: a full run with 10 seeds is a few hundred short game runs, a few minutes on two
CPUs. It needs your own `ftl.dat`, which stays in your data folder and never goes into the image.

What it can't check: anything you'd see or hear. Screenshots are skipped, there's no sound, and
timings are simulated. The Azahar runner (`tools/test/run_all.ps1`) and the console are still the
place for those.

## On TrueNAS SCALE (24.10 or later)

1. Make a dataset for it, e.g. `pool/apps/ftl3ds-tests`, and copy your `ftl.dat` into it (from your
   FTL folder: Steam > right-click FTL > Manage > Browse local files).
2. **Apps > Discover Apps > ⋮ > Install via YAML.** Name it `ftl3ds-tests` and paste
   [`docker-compose.yml`](docker-compose.yml), changing `/mnt/pool/apps/ftl3ds-tests` to your
   dataset's path.
3. Save. TrueNAS builds the image from GitHub (a minute or two the first time) and starts it.
4. Open `http://<your-nas>:8088`. The first check for commits runs straight away.

To update the server itself, redeploy the app (TrueNAS: Edit > Save, or `docker compose build --pull`
then `up -d`): it rebuilds from `main`. If the game starts using a 3DS function the headless
stand-ins (`test/headless/include/3ds.h`, `test/headless/stubs.c`) don't have, runs show
**build-failed** with the compiler's message in the build log: add the stand-in in the same commit. The game being tested is always the branch's latest commit.

## Settings

All in `environment:` in the compose file.

| Setting | Default | |
|---|---|---|
| `BRANCHES` | `main,experimental/stereo-3d` | Branches to watch, comma-separated |
| `POLL_MINUTES` | `10` | How often to check GitHub (0: only the Run buttons) |
| `SEEDS` | `10` | Extra runs per scenario with other seeds |
| `JOBS` | CPUs available | Tests run at the same time |
| `KEEP_RUNS` | `200` | Older results are deleted |
| `REPO_URL` | `https://github.com/melonexe/FTL3DS.git` | |

## The data folder

| Path | |
|---|---|
| `ftl.dat` | Your copy (you put it here) |
| `repo/` | The server's clone of the repository, and the headless build |
| `runs/<id>/` | One test run: `meta.json`, `results.json` (with every log), `build.log`, `runner.log` |

## Running it elsewhere

On any PC with Docker: from this folder, `docker compose up -d --build` after pointing the volume at
a folder holding `ftl.dat` (and `build: .` instead of the GitHub URL to use your local copy).

The headless tests also run without the server, on Linux or in MSYS2 with gcc, libpng and zlib:

```
make -C test/headless
python3 tools/test/run_headless.py --dat /path/to/ftl.dat --seeds 10
```
