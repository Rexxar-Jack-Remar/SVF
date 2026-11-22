#!/usr/bin/env python3
"""Measure complete snapshot-to-report clients; compare exact report files after timing."""
import argparse
import hashlib
import json
import os
import pathlib
import signal
import subprocess
import sys
import time

def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=pathlib.Path, help="complete snapshot, or bitcode with --live-pta")
    ap.add_argument("--live-pta", choices=["ander","vfspta"], help="extract events and index after a fresh PTA")
    ap.add_argument("--wpa", required=True)
    ap.add_argument("--output", type=pathlib.Path, required=True)
    ap.add_argument("--backends", nargs="+", default=["cpu","cuda","bitmap","pestrie"])
    ap.add_argument("--clients", nargs="+", choices=["uaf","taint"], default=["uaf","taint"])
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--device", type=int, default=0)
    ap.add_argument("--index-cache", type=pathlib.Path)
    ap.add_argument("--tile", type=int, default=1048576)
    args = ap.parse_args()
    if min(args.repeats,args.timeout,args.tile) < 1:
        ap.error("repeats, timeout and tile must be positive")
    args.output.mkdir(parents=True,exist_ok=False)
    failed = False
    with (args.output/"runs.jsonl").open("w") as runs:
        for client in args.clients:
            reference = None
            for repeat in range(args.repeats):
                # Rotate the order across repetitions.
                shift = repeat % len(args.backends)
                for backend in args.backends[shift:] + args.backends[:shift]:
                    stem = f"{client}-{repeat+1}-{backend}"
                    log = args.output/(stem+".log")
                    report = args.output/(stem+".jsonl")
                    inputs = ["-"+args.live_pta,str(args.input)] if args.live_pta else [f"-client-load={args.input}"]
                    cmd = [args.wpa,*inputs,f"-alias-client={client}",
                           f"-client-device={args.device}",f"-client-tile={args.tile}",
                           f"-client-backend={backend}",f"-client-report={report}"]
                    if args.index_cache and backend in ("bitmap","pestrie"):
                        cmd.append(f"-client-index-cache={args.index_cache}")
                    row = dict(client=client,backend=backend,repeat=repeat+1,command=cmd)
                    start = time.perf_counter()
                    with log.open("w") as stream:
                        proc = subprocess.Popen(cmd,stdout=stream,stderr=subprocess.STDOUT,start_new_session=True)
                        try:
                            status = proc.wait(timeout=args.timeout)
                            row["status"] = "ok" if status == 0 else f"exit_{status}"
                        except subprocess.TimeoutExpired:
                            os.killpg(proc.pid,signal.SIGKILL)
                            proc.wait()
                            row["status"] = "timeout"
                    row["wall_s"] = time.perf_counter() - start
                    if row["status"] == "ok":
                        with log.open() as stream:
                            metrics = [json.loads(line.removeprefix("CLIENT_METRICS "))
                                       for line in stream if line.startswith("CLIENT_METRICS ")]
                        if len(metrics) != 1:
                            raise ValueError(f"missing client metrics in {log}")
                        row.update(metrics[0])
                        row["sha256"] = digest(report)
                        if reference is None:
                            reference = row["sha256"]
                        elif reference == row["sha256"]:
                            report.unlink()
                        else:
                            row["status"] = "report_mismatch"
                    failed |= row["status"] != "ok"
                    print(json.dumps(row, sort_keys=True), file=runs, flush=True)
                    print(stem,row["status"],round(row["wall_s"],3),flush=True)
    return int(failed)

if __name__ == "__main__":
    sys.exit(main())
