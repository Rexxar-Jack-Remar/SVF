#!/usr/bin/env python3
"""Run complete UAF and taint clients over a directory of LLVM bitcode files.

Every invocation writes its configuration, commands, logs, reports, snapshots,
optional baseline indexes, and results.csv below the repository work/ directory.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import pathlib
import resource
import signal
import shlex
import subprocess
import sys
import time
from datetime import datetime, timezone
from typing import Any


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
BACKENDS = ("svf", "cpu", "cuda", "bitmap", "pestrie")
CLIENTS = ("uaf", "taint")
CSV_FIELDS = [
    "record_type",
    "bitcode",
    "client",
    "backend",
    "repeat",
    "pta",
    "status",
    "exit_code",
    "timed_out",
    "wall_s",
    "client_s",
    "total_s",
    "ir_s",
    "pta_s",
    "load_s",
    "save_s",
    "graph_candidates_s",
    "index_s",
    "query_transfer_s",
    "gpu_kernel_s",
    "gpu_copy_s",
    "consume_s",
    "solve_s",
    "report_s",
    "candidates",
    "pairs",
    "hits",
    "tiles",
    "reports",
    "frees",
    "reads",
    "writes",
    "sources",
    "sinks",
    "wpa_peak_rss_bytes",
    "cuda_buffer_bytes",
    "baseline_index",
    "report_sha256",
    "report_bytes",
    "reference_sha256",
    "report_match",
    "timeout_s",
    "memory_limit_bytes",
    "command",
    "stdout_path",
    "stderr_path",
    "report_path",
    "snapshot_path",
    "index_cache_path",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run end-to-end UAF and taint clients over .bc files"
    )
    parser.add_argument("input_dir", type=pathlib.Path)
    parser.add_argument("--config", type=pathlib.Path)
    parser.add_argument("--wpa", type=pathlib.Path)
    parser.add_argument("--pta", choices=("ander", "vfspta"))
    parser.add_argument("--clients", nargs="+", choices=CLIENTS)
    parser.add_argument("--backends", nargs="+", choices=BACKENDS)
    parser.add_argument("--repeats", type=int)
    parser.add_argument("--timeout-s", type=float)
    parser.add_argument("--memory-limit-gb", type=float)
    parser.add_argument("--tile", type=int)
    parser.add_argument("--cuda-device", type=int)
    parser.add_argument("--run-name")
    parser.add_argument(
        "--prepare",
        action=argparse.BooleanOptionalAction,
        default=None,
        help="run the per-bitcode snapshot preparation pass",
    )
    parser.add_argument(
        "--reuse-index-cache",
        action=argparse.BooleanOptionalAction,
        default=None,
        help="reuse Bitmap/Pestrie indexes under the run directory",
    )
    return parser.parse_args()


def resolve_path(path: pathlib.Path, base: pathlib.Path) -> pathlib.Path:
    if path.is_absolute():
        return path.expanduser().resolve()
    return (base / path).expanduser().resolve()


def load_config(path: pathlib.Path | None) -> dict[str, Any]:
    if path is None:
        return {}
    with path.open() as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError("configuration must be a JSON object")
    # A generated work/<run>/config.json contains reproducibility metadata and
    # stores the actual options under ``settings``.  Accepting that format
    # makes it possible to reuse a previous run after overriding machine-local
    # paths such as --wpa and the input directory.
    if "settings" in value:
        settings = value["settings"]
        if not isinstance(settings, dict):
            raise ValueError("configuration settings must be a JSON object")
        value = settings
    return value


def effective_config(args: argparse.Namespace, config: dict[str, Any]) -> dict[str, Any]:
    def choose(name: str, default: Any) -> Any:
        value = getattr(args, name)
        return value if value is not None else config.get(name, default)

    backends = list(choose("backends", list(BACKENDS)))
    clients = list(choose("clients", list(CLIENTS)))
    invalid_backends = sorted(set(backends) - set(BACKENDS))
    invalid_clients = sorted(set(clients) - set(CLIENTS))
    if invalid_backends:
        raise ValueError(f"unsupported backends: {', '.join(invalid_backends)}")
    if invalid_clients:
        raise ValueError(f"unsupported clients: {', '.join(invalid_clients)}")

    repeats = int(choose("repeats", 1))
    timeout_s = float(choose("timeout_s", 300))
    memory_limit_gb = float(choose("memory_limit_gb", 0))
    tile = int(choose("tile", 1048576))
    cuda_device = int(choose("cuda_device", 0))
    if repeats < 1 or timeout_s <= 0 or memory_limit_gb < 0 or tile < 1:
        raise ValueError("repeats, timeout, tile, and memory limit must be valid")

    wpa = choose("wpa", str(REPO_ROOT / "Release-build/bin/wpa"))
    return {
        "wpa": str(resolve_path(pathlib.Path(wpa), pathlib.Path.cwd())),
        "pta": choose("pta", "ander"),
        "clients": clients,
        "backends": backends,
        "repeats": repeats,
        "timeout_s": timeout_s,
        "memory_limit_gb": memory_limit_gb,
        "memory_limit_bytes": int(memory_limit_gb * (1024**3)),
        "tile": tile,
        "cuda_device": cuda_device,
        "prepare": bool(choose("prepare", True)),
        "reuse_index_cache": bool(choose("reuse_index_cache", False)),
        "run_name": choose(
            "run_name", datetime.now(timezone.utc).strftime("e2e-%Y%m%d-%H%M%S")
        ),
    }


def safe_name(value: str) -> str:
    return "".join(c if c.isalnum() or c in "._-" else "_" for c in value)


def digest(path: pathlib.Path) -> tuple[str, int]:
    h = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            h.update(chunk)
            size += len(chunk)
    return h.hexdigest(), size


def preexec(memory_limit_bytes: int):
    if memory_limit_bytes:
        resource.setrlimit(resource.RLIMIT_AS, (memory_limit_bytes, memory_limit_bytes))


def execute(
    command: list[str],
    stdout_path: pathlib.Path,
    stderr_path: pathlib.Path,
    timeout_s: float,
    memory_limit_bytes: int,
) -> dict[str, Any]:
    started = time.perf_counter()
    status = "ok"
    exit_code: int | None = None
    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    stderr_path.parent.mkdir(parents=True, exist_ok=True)
    with stdout_path.open("w") as stdout, stderr_path.open("w") as stderr:
        process = subprocess.Popen(
            command,
            stdout=stdout,
            stderr=stderr,
            start_new_session=True,
            preexec_fn=(
                (lambda: preexec(memory_limit_bytes))
                if memory_limit_bytes
                else None
            ),
        )
        try:
            exit_code = process.wait(timeout=timeout_s)
            if exit_code != 0:
                status = f"exit_{exit_code}"
        except subprocess.TimeoutExpired:
            status = "timeout"
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
    return {
        "status": status,
        "exit_code": exit_code,
        "timed_out": status == "timeout",
        "wall_s": time.perf_counter() - started,
    }


def read_metric(path: pathlib.Path, prefix: str) -> dict[str, Any] | None:
    values = []
    for line in path.read_text(errors="replace").splitlines():
        if line.startswith(prefix):
            values.append(json.loads(line[len(prefix) :]))
    if len(values) != 1:
        return None
    return values[0]


def merge_metric(row: dict[str, Any], metric: dict[str, Any]) -> None:
    for key, value in metric.items():
        target = "graph_candidates_s" if key == "graph_s" else key
        if target in row:
            row[target] = value


def blank_row() -> dict[str, Any]:
    return {field: "" for field in CSV_FIELDS}


def command_text(command: list[str]) -> str:
    return shlex.join(command)


def announce_command(command: list[str]) -> None:
    separator = "=" * 100
    print(separator, flush=True)
    print(f"command: {command_text(command)}", flush=True)
    print(separator, flush=True)


def main() -> int:
    args = parse_args()
    try:
        config = load_config(resolve_path(args.config, pathlib.Path.cwd()) if args.config else None)
        settings = effective_config(args, config)
        input_dir = resolve_path(args.input_dir, pathlib.Path.cwd())
        wpa = pathlib.Path(settings["wpa"])
        if not input_dir.is_dir():
            raise ValueError(f"input directory does not exist: {input_dir}")
        if not wpa.is_file():
            raise ValueError(f"wpa executable does not exist: {wpa}")
        bitcodes = sorted(input_dir.glob("*.bc"))
        if not bitcodes:
            raise ValueError(f"no .bc files found in {input_dir}")
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    work_root = REPO_ROOT / "work"
    run_dir = work_root / safe_name(settings["run_name"])
    if run_dir.exists():
        print(f"error: run directory already exists: {run_dir}", file=sys.stderr)
        return 2
    for name in ("logs", "reports", "snapshots", "index-cache"):
        (run_dir / name).mkdir(parents=True, exist_ok=True)

    metadata = {
        "repository": str(REPO_ROOT),
        "input_dir": str(input_dir),
        "bitcodes": [str(path) for path in bitcodes],
        "settings": settings,
    }
    (run_dir / "config.json").write_text(json.dumps(metadata, indent=2) + "\n")

    rows: list[dict[str, Any]] = []
    references: dict[tuple[str, str, int], str] = {}
    results = run_dir / "results.csv"
    commands = run_dir / "commands.jsonl"
    results_stream = results.open("w", newline="")
    commands_stream = commands.open("w")
    writer = csv.DictWriter(results_stream, fieldnames=CSV_FIELDS)
    writer.writeheader()
    results_stream.flush()

    def add_row(row: dict[str, Any]) -> None:
        rows.append(row)
        writer.writerow(row)
        results_stream.flush()
        commands_stream.write(json.dumps({"command": row["command"]}) + "\n")
        commands_stream.flush()

    for bitcode in bitcodes:
        stem = safe_name(bitcode.stem)
        snapshot = run_dir / "snapshots" / f"{stem}.{settings['pta']}.snapshot"
        if settings["prepare"]:
            prepare_stdout = run_dir / "logs" / f"{stem}.prepare.stdout.log"
            prepare_stderr = run_dir / "logs" / f"{stem}.prepare.stderr.log"
            prepare_command = [
                str(wpa),
                f"-{settings['pta']}",
                f"-client-save={snapshot}",
                str(bitcode),
            ]
            announce_command(prepare_command)
            execution = execute(
                prepare_command,
                prepare_stdout,
                prepare_stderr,
                settings["timeout_s"],
                settings["memory_limit_bytes"],
            )
            metric = read_metric(prepare_stderr, "CLIENT_PREPARE ")
            row = blank_row()
            row.update(
                {
                    "record_type": "prepare",
                    "bitcode": str(bitcode),
                    "pta": settings["pta"],
                    "command": command_text(prepare_command),
                    "stdout_path": str(prepare_stdout),
                    "stderr_path": str(prepare_stderr),
                    "snapshot_path": str(snapshot),
                    "timeout_s": settings["timeout_s"],
                    "memory_limit_bytes": settings["memory_limit_bytes"],
                    **execution,
                }
            )
            if metric:
                merge_metric(row, metric)
            add_row(row)

        for client in settings["clients"]:
            for repeat in range(1, settings["repeats"] + 1):
                key = (stem, client, repeat)
                for backend in settings["backends"]:
                    run_stem = f"{stem}.{client}.{backend}.r{repeat}"
                    report = run_dir / "reports" / f"{run_stem}.jsonl"
                    stdout_path = run_dir / "logs" / f"{run_stem}.stdout.log"
                    stderr_path = run_dir / "logs" / f"{run_stem}.stderr.log"
                    command = [
                        str(wpa),
                        f"-{settings['pta']}",
                        f"-alias-client={client}",
                        f"-client-backend={backend}",
                        f"-client-device={settings['cuda_device']}",
                        f"-client-tile={settings['tile']}",
                        f"-client-report={report}",
                        str(bitcode),
                    ]
                    index_cache = ""
                    if settings["reuse_index_cache"] and backend in ("bitmap", "pestrie"):
                        index_cache = str(run_dir / "index-cache")
                        command.insert(-1, f"-client-index-cache={index_cache}")
                    announce_command(command)
                    execution = execute(
                        command,
                        stdout_path,
                        stderr_path,
                        settings["timeout_s"],
                        settings["memory_limit_bytes"],
                    )
                    metric = read_metric(stderr_path, "CLIENT_METRICS ") or {}
                    row = blank_row()
                    row.update(
                        {
                            "record_type": "client",
                            "bitcode": str(bitcode),
                            "client": client,
                            "backend": backend,
                            "repeat": repeat,
                            "pta": settings["pta"],
                            "command": command_text(command),
                            "stdout_path": str(stdout_path),
                            "stderr_path": str(stderr_path),
                            "report_path": str(report),
                            "snapshot_path": str(snapshot) if settings["prepare"] else "",
                            "index_cache_path": index_cache,
                            "timeout_s": settings["timeout_s"],
                            "memory_limit_bytes": settings["memory_limit_bytes"],
                            **execution,
                        }
                    )
                    merge_metric(row, metric)
                    if report.is_file():
                        row["report_sha256"], row["report_bytes"] = digest(report)
                        reference = references.setdefault(key, row["report_sha256"])
                        row["reference_sha256"] = reference
                        row["report_match"] = row["report_sha256"] == reference
                        if row["status"] == "ok" and not row["report_match"]:
                            row["status"] = "report_mismatch"
                    add_row(row)

    results_stream.close()
    commands_stream.close()

    failed = [row for row in rows if row["status"] != "ok"]
    print(f"run directory: {run_dir}")
    print(f"results: {results}")
    print(f"records: {len(rows)}, failures: {len(failed)}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
