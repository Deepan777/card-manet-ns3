#!/usr/bin/env python3
"""
SCR campaign runner - resumable, checkpointed, progress-reporting.

Design requirements (specification Stage 8):
  - explicit deduplicated run manifest, no blind Cartesian product
  - immutable attempt directories
  - resume by checksum: a run is redone only if outputs are missing or corrupt
  - never silently overwrite a completed attempt

Operational requirements:
  - safe to kill at any moment (Ctrl-C, power loss, laptop sleep/shutdown)
  - on restart, completed runs are skipped and the campaign continues
  - progress is written to disk continuously, readable without this script

Usage:
  python scripts/campaign_runner.py --manifest configs/<name>.json [--jobs N]
  python scripts/campaign_runner.py --manifest configs/<name>.json --status
"""

import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import pathlib
import shutil
import signal
import subprocess
import threading
import sys
import time
from datetime import datetime, timezone

ROOT = pathlib.Path(__file__).resolve().parent.parent
CHECKPOINTS = ROOT / "checkpoints"

_stop = False


def _handle_stop(signum, frame):
    global _stop
    _stop = True
    print("\n[runner] stop requested; finishing in-flight runs, then exiting cleanly.",
          flush=True)


signal.signal(signal.SIGINT, _handle_stop)
signal.signal(signal.SIGTERM, _handle_stop)


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run_dir(campaign, run_id):
    return CHECKPOINTS / campaign / run_id


def known_failures(campaign):
    """Runs recorded as permanently unrunnable, with the reason.

    R033: two DSDV runs abort inside vendored ns-3 (`interface >= 0` in
    Ipv4L3Protocol::SendRealOut) when DSDV forwards over an interface the C1
    outage model has taken down. It is deterministic, so every resume spent
    another ~16 minutes reproducing it before the next campaign phase could
    start.

    This list makes the exclusion EXPLICIT and auditable. It is not a DONE
    marker: these runs are skipped and reported as skipped, never counted as
    results, and the analysis still shows the affected cells at reduced n.
    """
    f = CHECKPOINTS / campaign / "KNOWN_FAILURES.json"
    if not f.exists():
        return {}
    try:
        return json.loads(f.read_text(encoding="utf-8"))
    except Exception:
        return {}


def is_complete(campaign, run_id):
    """Complete only if the DONE marker exists AND every recorded output still
    matches its recorded checksum. Truncated output from a power loss mid-write
    is therefore treated as incomplete and the run is redone."""
    d = run_dir(campaign, run_id)
    marker = d / "DONE.json"
    if not marker.exists():
        return False
    try:
        rec = json.loads(marker.read_text())
    except Exception:
        return False
    for name, digest in rec.get("outputs", {}).items():
        f = d / name
        if not f.exists() or sha256_file(f) != digest:
            return False
    return True


# --- R020: environment-fault classification and circuit breaker -------------
#
# Windows NTSTATUS codes surface through subprocess as large unsigned values.
# These are all "the process could not start or initialise", never "the
# simulation computed something and exited":
#
#   0xC0000142 STATUS_DLL_INIT_FAILED     -- observed 3,854 times in 3 minutes
#   0xC0000135 STATUS_DLL_NOT_FOUND       -- observed 1,756 times (R019)
#   0xC0000017 STATUS_NO_MEMORY
#   0xC000009A STATUS_INSUFFICIENT_RESOURCES
ENVIRONMENT_STATUS_CODES = {
    3221225794,  # 0xC0000142 DLL init failed
    3221225781,  # 0xC0000135 DLL not found
    3221225495,  # 0xC0000017 no memory
    3221225626,  # 0xC000009A insufficient resources
}
# A genuine simulation failure takes real time; a process that never started
# returns almost immediately. The time bound keeps a fast legitimate crash that
# happens to share a code from being retried forever.
ENVIRONMENT_MAX_ELAPSED_S = 5.0
# Overridable so the retry/breaker path can be exercised in a test without
# waiting minutes per run, and so an operator can tune backoff on a busy
# machine without editing this file.
MAX_ENV_RETRIES = int(os.environ.get("SCR_ENV_RETRIES", "4"))
ENV_BACKOFF_BASE_S = float(os.environ.get("SCR_ENV_BACKOFF_S", "15"))
ENV_BACKOFF_MAX_S = float(os.environ.get("SCR_ENV_BACKOFF_MAX_S", "120"))
# Consecutive environment faults tolerated before the campaign stops. The point
# is that a manifest must never be consumed by a transient machine condition.
CIRCUIT_BREAKER_CONSECUTIVE = int(os.environ.get("SCR_BREAKER_N", "10"))

ENV_RETRY_COUNT = [0]
IN_FLIGHT = [0]


def is_environment_fault(rc, elapsed):
    """True when the process died before it could run, not because it failed."""
    return rc in ENVIRONMENT_STATUS_CODES and elapsed < ENVIRONMENT_MAX_ELAPSED_S


def run_awake(argv, d, timeout_s, poll_s=5.0, sleep_gap_s=60.0):
    """Run one simulation; enforce the timeout on AWAKE time only.

    R056. The timeout exists to catch livelocks (R014), not to measure the
    calendar. When the laptop sleeps, the simulation is suspended with it, but
    a wall-clock timeout keeps counting, so after a long sleep every in-flight
    run would be killed and marked failed on wake. Here the elapsed time is
    accumulated from short polls, and a gap between polls much longer than the
    poll interval is treated as time asleep and not counted.

    Output goes straight to stdout.txt / stderr.txt, which is what the previous
    in-memory capture wrote after the process exited.
    """
    # R057: an executable from a second ns-3 tree (the baselines development
    # tree) must load THAT tree's libraries. Windows resolves the ns-3 DLLs
    # through PATH, so without this a dev-tree binary would silently load the
    # main tree's routing library. The library directory is derived from the
    # executable path (<tree>/build/scratch/<prog>/<exe> -> <tree>/build/lib);
    # for main-tree runs it is the directory already on PATH.
    env = dict(os.environ)
    exe = pathlib.Path(argv[0])
    parts = [p.lower() for p in exe.parts]
    if "build" in parts and "scratch" in parts:
        lib = pathlib.Path(*exe.parts[:parts.index("build") + 1]) / "lib"
        env["PATH"] = os.pathsep.join([str(lib), env.get("PATH", "")])
    with open(d / "stdout.txt", "w", encoding="utf-8") as fo, \
            open(d / "stderr.txt", "w", encoding="utf-8") as fe:
        proc = subprocess.Popen(argv, cwd=str(d), stdout=fo, stderr=fe, text=True, env=env)
        awake = 0.0
        last = time.monotonic()
        while True:
            try:
                rc = proc.wait(timeout=poll_s)
                return rc, False
            except subprocess.TimeoutExpired:
                pass
            now = time.monotonic()
            gap = now - last
            last = now
            if gap < sleep_gap_s:
                awake += gap
            if awake > timeout_s:
                proc.kill()
                proc.wait()
                return -9, True


def execute_run(campaign, binary, run):
    run_id = run["run_id"]
    d = run_dir(campaign, run_id)

    if is_complete(campaign, run_id):
        return (run_id, "skipped", 0.0)

    # R063 disk guard: a full disk would turn every remaining run into a recorded
    # failure. Below the floor the runner stops cleanly instead; it is resumable.
    global _stop
    free_gb = shutil.disk_usage(str(CHECKPOINTS)).free / 1e9
    if free_gb < float(os.environ.get("SCR_DISK_MIN_GB", "2.0")):
        if not _stop:
            print("[runner] DISK GUARD: %.2f GB free; stopping before %s" % (free_gb, run_id), flush=True)
        _stop = True
        return (run_id, "skipped_disk", 0.0)
    if _stop:
        return (run_id, "skipped_stop", 0.0)

    # Fresh attempt directory; partial output from a killed attempt is discarded.
    if d.exists():
        for f in d.iterdir():
            try:
                f.unlink()
            except OSError:
                pass
    d.mkdir(parents=True, exist_ok=True)

    # R053: a manifest spanning scenario families (the CARD campaign) names the
    # binary per run. Manifests without the field behave exactly as before.
    argv = [run.get("binary", binary)] + [str(a) for a in run["args"]]

    # R020: distinguish an ENVIRONMENT fault from a simulation failure and retry
    # it, instead of consuming a manifest entry.
    #
    # On 2026-09-09 the machine ran short of memory for about three minutes.
    # Every process launched in that window died during DLL initialisation in
    # under 0.06 s. Because a failure costs no time, three runners chewed
    # through 3,854 remaining runs in those three minutes and reported all of
    # them as failed. Runs already in flight kept completing normally for the
    # next twenty minutes, which is how we know the machine itself was fine.
    #
    # A run that dies before it can even load its libraries has told us nothing
    # about the simulation, so it must not be recorded as a result.
    attempt = 0
    IN_FLIGHT[0] += 1
    while True:
        t0 = time.time()
        rc, timed_out = run_awake(argv, d, run.get("timeout_s", 7200))
        if timed_out:
            (d / "stderr.txt").write_text("TIMEOUT", encoding="utf-8")
            rc = -9
        elapsed = time.time() - t0

        if not is_environment_fault(rc, elapsed) or attempt >= MAX_ENV_RETRIES:
            break
        attempt += 1
        # Back off so a machine under pressure is given room to recover rather
        # than being hammered by immediate retries.
        time.sleep(min(ENV_BACKOFF_BASE_S * (2 ** (attempt - 1)), ENV_BACKOFF_MAX_S))

    IN_FLIGHT[0] -= 1
    if attempt:
        ENV_RETRY_COUNT[0] += 1

    if rc != 0:
        env_fault = is_environment_fault(rc, elapsed)
        (d / "FAILED.json").write_text(json.dumps(
            {"run_id": run_id, "returncode": rc, "elapsed_s": elapsed,
             "argv": argv, "environment_fault": env_fault,
             "retries_attempted": attempt}, indent=2), encoding="utf-8")
        return (run_id, "env_fault" if env_fault else "failed", elapsed)

    outputs = {}
    for f in sorted(d.iterdir()):
        if f.is_file() and f.name not in ("DONE.json", "FAILED.json"):
            outputs[f.name] = sha256_file(f)

    # Atomic marker write: temp then rename, so a crash mid-write cannot leave a
    # DONE marker claiming a run finished when it did not.
    tmp = d / "DONE.json.tmp"
    tmp.write_text(json.dumps({
        "run_id": run_id,
        "campaign": campaign,
        "argv": argv,
        "returncode": rc,
        "elapsed_s": round(elapsed, 3),
        "completed_utc": datetime.now(timezone.utc).isoformat(),
        "outputs": outputs,
    }, indent=2), encoding="utf-8")
    os.replace(tmp, d / "DONE.json")
    return (run_id, "ok", elapsed)


def write_progress(campaign, total, done, failed, started, samples, jobs=1):
    pdir = CHECKPOINTS / campaign
    pdir.mkdir(parents=True, exist_ok=True)
    pct = (done / total * 100.0) if total else 0.0
    mean = sum(samples) / len(samples) if samples else None
    remaining = total - done
    # Wall-clock ETA must account for concurrency: with `jobs` runs in
    # flight the remaining work finishes about `jobs` times sooner than
    # the sequential sum of run times.
    eta_s = (mean * remaining / max(jobs, 1)) if mean else None

    payload = {
        "campaign": campaign,
        "updated_utc": datetime.now(timezone.utc).isoformat(),
        "total_runs": total,
        "completed": done,
        "failed": failed,
        "remaining": remaining,
        "percent_complete": round(pct, 2),
        "jobs": jobs,
        "mean_run_seconds": round(mean, 1) if mean else None,
        "eta_seconds": round(eta_s) if eta_s else None,
        "eta_hours": round(eta_s / 3600, 2) if eta_s else None,
        "wall_elapsed_hours": round((time.time() - started) / 3600, 3),
    }
    tmp = pdir / "progress.json.tmp"
    tmp.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    os.replace(tmp, pdir / "progress.json")

    bar_w = 40
    filled = int(bar_w * pct / 100)
    bar = "#" * filled + "-" * (bar_w - filled)
    eta_txt = (str(payload["eta_hours"]) + " h") if eta_s else "unknown"
    lines = [
        "campaign : " + campaign,
        "progress : [" + bar + "] " + format(pct, "5.1f") + "%",
        "runs     : " + str(done) + " done / " + str(total) + " total ("
        + str(failed) + " failed)",
        "mean run : " + str(payload["mean_run_seconds"]) + " s  (jobs="
        + str(jobs) + ")",
        "ETA      : " + eta_txt,
        "updated  : " + payload["updated_utc"],
        "",
    ]
    # A campaign whose runs are long produces no output for many minutes, which
    # is indistinguishable from a dead campaign -- this was mistaken for a stall
    # twice. Stamp the file on every write and record how many runs are in
    # flight, so "alive" is checkable without inspecting the process table.
    lines.append("in flight : " + str(IN_FLIGHT[0]))
    lines.append("heartbeat : " + datetime.now(timezone.utc).isoformat())
    (pdir / "PROGRESS.txt").write_text("\n".join(lines), encoding="utf-8")
    return pct, payload


def trip_breaker(n):
    print("", flush=True)
    print("[runner] CIRCUIT BREAKER: " + str(n) + " consecutive environment "
          "faults. The machine cannot start new processes right now (commonly "
          "memory pressure). Stopping so the manifest is not consumed by a "
          "condition that has nothing to do with the simulation.", flush=True)
    print("[runner] Nothing is lost: affected runs have no DONE marker and are "
          "retried on resume. Free memory, then rerun the same command.",
          flush=True)


def _python_alive(pid):
    """True if `pid` is a live python process (tasklist; guards against PID reuse)."""
    try:
        out = subprocess.run(["tasklist", "/FI", "PID eq %d" % pid, "/NH", "/FO", "CSV"],
                             capture_output=True, text=True, timeout=30).stdout
    except Exception:
        return False
    return ('"%d"' % pid) in out and "python" in out.lower()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--jobs", type=int, default=1,
                    help="concurrent runs; 1 = sequential (specification default)")
    ap.add_argument("--status", action="store_true",
                    help="print progress and exit without running anything")
    args = ap.parse_args()

    man = json.loads(pathlib.Path(args.manifest).read_text())
    campaign = man["campaign_id"]
    binary = man["binary"]
    runs = man["runs"]

    ids = [r["run_id"] for r in runs]
    if len(ids) != len(set(ids)):
        sys.exit("ERROR: duplicate run_id in manifest; manifest must be deduplicated.")

    if args.status:
        p = CHECKPOINTS / campaign / "PROGRESS.txt"
        print(p.read_text() if p.exists() else "no progress yet for " + campaign)
        return

    if not pathlib.Path(binary).exists():
        sys.exit("ERROR: binary not found: " + binary)

    # R058: one runner per campaign. Two runners on one manifest race on the
    # same run directories (R019). A campaign can now be started outside the
    # supervisor (card_taaodv began while the supervisor was already inside
    # phase 5), so the runner itself refuses to start while another live
    # runner holds the campaign.
    lock = CHECKPOINTS / campaign / "runner.lock"
    lock.parent.mkdir(parents=True, exist_ok=True)
    if lock.exists():
        try:
            old = int("".join(c for c in lock.read_text() if c.isdigit()) or 0)
        except Exception:
            old = 0
        if old and old != os.getpid() and _python_alive(old):
            print("[runner] runner %d already owns %s; exiting" % (old, campaign), flush=True)
            return
    lock.write_text(str(os.getpid()))

    started = time.time()
    skiplist = known_failures(campaign)
    pending = [r for r in runs
               if not is_complete(campaign, r["run_id"]) and r["run_id"] not in skiplist]
    if skiplist:
        print("[runner] skipping " + str(len(skiplist))
              + " run(s) recorded as permanently unrunnable:", flush=True)
        for rid, why in sorted(skiplist.items()):
            print("[runner]   " + rid + " -- " + str(why), flush=True)
    already = len(runs) - len(pending)
    print("[runner] campaign=" + campaign + " total=" + str(len(runs))
          + " already_complete=" + str(already) + " to_run=" + str(len(pending))
          + " jobs=" + str(args.jobs), flush=True)

    done = already
    failed = 0
    env_faults = 0
    consecutive_env = 0
    samples = []
    write_progress(campaign, len(runs), done, failed, started, samples, args.jobs)

    # Heartbeat. Long runs mean the progress file is otherwise untouched for ten
    # minutes or more, which reads as a dead campaign -- it was mistaken for one
    # twice. A daemon thread rewrites it every 30 s so the heartbeat stamp and
    # the in-flight count always show whether work is actually happening.
    _hb_stop = threading.Event()

    def _heartbeat():
        while not _hb_stop.wait(30.0):
            try:
                write_progress(campaign, len(runs), done, failed, started,
                               samples, args.jobs)
            except OSError:
                # Losing a heartbeat write must never disturb the campaign.
                pass

    _hb = threading.Thread(target=_heartbeat, daemon=True)
    _hb.start()

    if args.jobs == 1:
        for r in pending:
            if _stop:
                break
            rid, status, el = execute_run(campaign, binary, r)
            if status == "ok":
                done += 1
                samples.append(el)
                consecutive_env = 0
            elif status == "env_fault":
                env_faults += 1
                consecutive_env += 1
            elif status == "failed":
                failed += 1
                done += 1
                consecutive_env = 0
            pct, _ = write_progress(campaign, len(runs), done, failed, started, samples, args.jobs)
            print("[runner] " + rid + " " + status + " " + format(el, ".1f")
                  + "s -> " + format(pct, ".1f") + "%", flush=True)
            if consecutive_env >= CIRCUIT_BREAKER_CONSECUTIVE:
                trip_breaker(consecutive_env)
                break
    else:
        with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = {ex.submit(execute_run, campaign, binary, r): r for r in pending}
            for fut in cf.as_completed(futs):
                rid, status, el = fut.result()
                if status == "ok":
                    done += 1
                    samples.append(el)
                    consecutive_env = 0
                elif status == "env_fault":
                    env_faults += 1
                    consecutive_env += 1
                elif status == "failed":
                    failed += 1
                    done += 1
                    consecutive_env = 0
                pct, _ = write_progress(campaign, len(runs), done, failed, started,
                                        samples, args.jobs)
                print("[runner] " + rid + " " + status + " " + format(el, ".1f")
                      + "s -> " + format(pct, ".1f") + "%", flush=True)
                # R020 circuit breaker. Every pending run is queued up front, so
                # without this a transient machine condition drains the whole
                # manifest at ~0.03 s per run. Cancel what has not started and
                # stop; the runner is resumable, so nothing is lost by stopping.
                if consecutive_env >= CIRCUIT_BREAKER_CONSECUTIVE:
                    trip_breaker(consecutive_env)
                    for f2 in futs:
                        f2.cancel()
                    break
                if _stop:
                    for f2 in futs:
                        f2.cancel()
                    break

    _hb_stop.set()
    pct, payload = write_progress(campaign, len(runs), done, failed, started, samples, args.jobs)
    state = "STOPPED" if _stop else "FINISHED"
    print("\n[runner] " + state + ": " + str(done) + "/" + str(len(runs))
          + " (" + format(pct, ".1f") + "%), failed=" + str(failed), flush=True)
    if env_faults:
        print("[runner] environment faults (process never started): "
              + str(env_faults) + "; these consumed no manifest entry and will "
              "be retried on resume", flush=True)
    if ENV_RETRY_COUNT[0]:
        # Counts runs that needed at least one retry, whatever the outcome --
        # not runs that were rescued by one. Saying otherwise would overstate it.
        print("[runner] runs that required an environment retry: "
              + str(ENV_RETRY_COUNT[0]), flush=True)
    print("[runner] resume with the same command; completed runs are skipped.",
          flush=True)


if __name__ == "__main__":
    main()
