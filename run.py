#!/usr/bin/env python3
"""Run one synthesis engine consistently across AIG benchmarks."""

import argparse
import csv
import hashlib
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP, getcontext
from pathlib import Path


getcontext().prec = 50

DEFAULT_ENGINE_PATH = "build/target-reduction"
DEFAULT_CEC_PATH = "../abc/abc"
DECIMAL_PLACES = 2

BENCHMARK_SETS = {
    # "benchmark-suite-name": [
    #     "case-name-00", "case-name-01", ... "case-name-n"
    # ],
}

ANSI_ESCAPE_RE = re.compile(r"\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])")


@dataclass(frozen=True)
class BenchmarkCase:
    path: Path
    name: str
    output_stem: str


@dataclass
class RunResult:
    row: dict
    numeric: dict
    include_in_average: bool


def parse_search_dirs(raw_dirs):
    """Expand repeated/comma-separated search directories while preserving order."""
    if not raw_dirs:
        raw_dirs = [["benchmarks"]]

    result = []
    seen = set()
    for group in raw_dirs:
        values = group if isinstance(group, (list, tuple)) else [group]
        for raw in values:
            for item in raw.split(","):
                item = item.strip()
                if not item:
                    continue
                path = Path(item).expanduser()
                key = str(path)
                if key not in seen:
                    result.append(path)
                    seen.add(key)
    return result


def find_named_aig(name, search_dirs):
    """Find one unambiguous AIG basename in the configured search roots."""
    filename = name if name.lower().endswith(".aig") else f"{name}.aig"
    matches = []
    for directory in search_dirs:
        if directory.is_dir():
            matches.extend(path.resolve() for path in directory.rglob(filename) if path.is_file())

    unique_matches = sorted(set(matches))
    if not unique_matches:
        return None
    if len(unique_matches) > 1:
        listed = "\n  ".join(str(path) for path in unique_matches)
        raise ValueError(
            f"Benchmark name '{filename}' is ambiguous. Narrow --dir or pass a path:\n  {listed}"
        )
    return unique_matches[0]


def safe_output_stem(name, source_path, used_stems):
    """Create a filesystem-safe, collision-free flat output stem."""
    stem = str(Path(name).with_suffix("")).replace("/", "__").replace(os.sep, "__")
    stem = re.sub(r"[^A-Za-z0-9_.-]+", "_", stem).strip("._") or "benchmark"
    if stem in used_stems:
        digest = hashlib.sha1(str(source_path).encode("utf-8")).hexdigest()[:8]
        stem = f"{stem}__{digest}"
    used_stems.add(stem)
    return stem


def make_cases(path_and_names):
    """Normalize benchmark paths and assign stable output names."""
    cases = []
    used_paths = set()
    used_stems = set()
    for path, name in path_and_names:
        resolved = path.resolve()
        if resolved in used_paths:
            continue
        used_paths.add(resolved)
        cases.append(
            BenchmarkCase(
                path=resolved,
                name=resolved.name,
                output_stem=safe_output_stem(name, resolved, used_stems),
            )
        )
    return cases


def collect_cases(input_spec, search_dirs):
    """Resolve a path, benchmark-set name, basename, or all AIGs in --dir."""
    if input_spec is None:
        found = []
        for directory in search_dirs:
            if not directory.is_dir():
                continue
            for path in sorted(directory.rglob("*.aig")):
                if path.is_file():
                    name = f"{directory.name}/{path.relative_to(directory).as_posix()}"
                    found.append((path, name))
        if not found:
            raise ValueError(f"No .aig files found in: {', '.join(map(str, search_dirs))}")
        return make_cases(found)

    explicit_path = Path(input_spec).expanduser()
    if explicit_path.exists():
        if explicit_path.is_file():
            if explicit_path.suffix.lower() != ".aig":
                raise ValueError(f"Input file is not an .aig file: {explicit_path}")
            return make_cases([(explicit_path, explicit_path.name)])

        found = [
            (path, path.relative_to(explicit_path).as_posix())
            for path in sorted(explicit_path.rglob("*.aig"))
            if path.is_file()
        ]
        if not found:
            raise ValueError(f"No .aig files found in directory: {explicit_path}")
        return make_cases(found)

    if input_spec in BENCHMARK_SETS:
        found = []
        missing = []
        for name in BENCHMARK_SETS[input_spec]:
            path = find_named_aig(name, search_dirs)
            if path is None:
                missing.append(name)
            else:
                found.append((path, path.name))
        if missing:
            print(f"Warning: {len(missing)} benchmark(s) not found: {', '.join(missing)}")
        if not found:
            raise ValueError(f"No benchmarks from set '{input_spec}' were found")
        return make_cases(found)

    path = find_named_aig(input_spec, search_dirs)
    if path is None:
        raise ValueError(
            f"Input '{input_spec}' is neither an existing path nor an AIG found in: "
            f"{', '.join(map(str, search_dirs))}"
        )
    return make_cases([(path, path.name)])


def decode_aiger_delta(data, offset):
    """Decode one unsigned AIGER binary delta."""
    value = 0
    shift = 0
    while True:
        if offset >= len(data):
            raise ValueError("Unexpected EOF while decoding an AIGER delta")
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            return value, offset
        shift += 7
        if shift > 63:
            raise ValueError("Invalid AIGER delta")


def read_binary_aig_stats(data, header, line_end):
    """Read combinational binary AIGER size and output depth."""
    if len(header) != 6:
        raise ValueError("Extended binary AIGER headers are not supported")
    _, maximum, n_inputs, n_latches, n_outputs, n_ands = header
    maximum = int(maximum)
    n_inputs = int(n_inputs)
    n_latches = int(n_latches)
    n_outputs = int(n_outputs)
    n_ands = int(n_ands)
    if n_latches != 0:
        raise ValueError("Sequential AIGER files are not supported")
    if maximum != n_inputs + n_ands:
        raise ValueError("AIGER object count does not match inputs + ANDs")

    offset = line_end + 1
    output_literals = []
    for _ in range(n_outputs):
        next_line = data.index(b"\n", offset)
        output_literals.append(int(data[offset:next_line]))
        offset = next_line + 1

    levels = [0] * (maximum + 1)
    for and_index in range(n_ands):
        node_id = n_inputs + 1 + and_index
        lhs_literal = node_id << 1
        delta0, offset = decode_aiger_delta(data, offset)
        rhs1_literal = lhs_literal - delta0
        delta1, offset = decode_aiger_delta(data, offset)
        rhs0_literal = rhs1_literal - delta1
        fanin0 = rhs0_literal >> 1
        fanin1 = rhs1_literal >> 1
        if fanin0 >= node_id or fanin1 >= node_id:
            raise ValueError("AIGER AND fanins are not topologically ordered")
        levels[node_id] = max(levels[fanin0], levels[fanin1]) + 1

    if any((literal >> 1) > maximum for literal in output_literals):
        raise ValueError("AIGER output references an invalid object")
    level = max((levels[literal >> 1] for literal in output_literals), default=0)
    return {"nPIs": n_inputs, "nPOs": n_outputs, "size": n_ands, "level": level}


def read_ascii_aig_stats(data, header):
    """Read combinational ASCII AIGER size and output depth."""
    if len(header) != 6:
        raise ValueError("Extended ASCII AIGER headers are not supported")
    _, maximum, n_inputs, n_latches, n_outputs, n_ands = header
    maximum = int(maximum)
    n_inputs = int(n_inputs)
    n_latches = int(n_latches)
    n_outputs = int(n_outputs)
    n_ands = int(n_ands)
    if n_latches != 0:
        raise ValueError("Sequential AIGER files are not supported")

    lines = data.decode("ascii", errors="strict").splitlines()
    cursor = 1
    cursor += n_inputs
    output_start = cursor
    cursor += n_outputs
    output_literals = [int(lines[index]) for index in range(output_start, cursor)]

    levels = [0] * (maximum + 1)
    for _ in range(n_ands):
        lhs_literal, rhs0_literal, rhs1_literal = map(int, lines[cursor].split())
        cursor += 1
        node_id = lhs_literal >> 1
        fanin0 = rhs0_literal >> 1
        fanin1 = rhs1_literal >> 1
        if node_id > maximum or fanin0 >= node_id or fanin1 >= node_id:
            raise ValueError("AIGER AND fanins are not topologically ordered")
        levels[node_id] = max(levels[fanin0], levels[fanin1]) + 1

    level = max((levels[literal >> 1] for literal in output_literals), default=0)
    return {"nPIs": n_inputs, "nPOs": n_outputs, "size": n_ands, "level": level}


def read_aig_stats(path):
    """Read combinational binary or ASCII AIGER statistics without invoking an engine."""
    data = Path(path).read_bytes()
    line_end = data.index(b"\n")
    header = data[:line_end].decode("ascii", errors="strict").split()
    if not header:
        raise ValueError("Empty AIGER header")
    if header[0] == "aig":
        return read_binary_aig_stats(data, header, line_end)
    if header[0] == "aag":
        return read_ascii_aig_stats(data, header)
    raise ValueError("Input is not an AIGER aig/aag file")


def quote_engine_argument(value):
    """Quote a filename for the ABC-style command language."""
    text = str(value)
    if any(char in text for char in ('"', "\n", "\r", ";")):
        raise ValueError(f"Unsupported character in command path: {text}")
    return f'"{text}"'


def normalize_user_command(command):
    """Remove only redundant trailing command separators."""
    return command.strip().rstrip(";").strip()


def build_engine_script(input_path, user_command, output_path):
    """Build the common read/optimize/write script accepted by Target-Reduction and ABC."""
    return (
        f"read {quote_engine_argument(input_path)}; "
        f"{user_command}; "
        f"write {quote_engine_argument(output_path)};"
    )


def run_engine(engine_path, script, log_path):
    """Run the engine and time only that subprocess, excluding later CEC."""
    command = [str(engine_path), "-c", script]
    with log_path.open("w") as log_handle:
        log_handle.write(f"Executing engine: {command!r}\n{'=' * 72}\n")
        log_handle.flush()
        start = time.perf_counter()
        process = subprocess.run(
            command,
            stdout=log_handle,
            stderr=subprocess.STDOUT,
            check=False,
        )
        elapsed = time.perf_counter() - start
        log_handle.write(f"\nEngine return code: {process.returncode}\n")
    return process.returncode, Decimal(str(elapsed))


def parse_cec_status(output):
    """Classify the standard ABC CEC result text."""
    clean = ANSI_ESCAPE_RE.sub("", output).lower()
    if "not equivalent" in clean:
        return "inequivalent"
    if "are equivalent" in clean:
        return "equivalent"
    return "error"


def run_cec(cec_path, original_path, optimized_path, log_path):
    """Run ABC CEC after engine timing and report its independent wall time."""
    script = (
        f"cec -n {quote_engine_argument(optimized_path)} "
        f"{quote_engine_argument(original_path)};"
    )
    command = [str(cec_path), "-c", script]
    start = time.perf_counter()
    process = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        text=True,
    )
    elapsed = Decimal(str(time.perf_counter() - start))
    output = process.stdout or ""
    with log_path.open("a") as log_handle:
        log_handle.write(f"\n{'=' * 72}\nExecuting CEC: {command!r}\n")
        log_handle.write(output)
        log_handle.write(f"\nCEC return code: {process.returncode}\n")
    if process.returncode != 0:
        return "error", elapsed
    return parse_cec_status(output), elapsed


def reduction_percentage(before, after):
    """Return (1 - after / before) * 100 with its sign preserved."""
    if before is None or after is None or before == 0:
        return None
    return (Decimal(1) - Decimal(after) / Decimal(before)) * Decimal(100)


def format_decimal(value):
    """Round decimal output with the conventional >= 5 rounds up rule."""
    if value is None:
        return ""
    if not isinstance(value, Decimal):
        try:
            value = Decimal(value)
        except (InvalidOperation, TypeError):
            return ""
    quantum = Decimal(1).scaleb(-DECIMAL_PLACES)
    rounded = value.quantize(quantum, rounding=ROUND_HALF_UP)
    return format(rounded, f".{DECIMAL_PLACES}f")


def process_case(case, engine_path, user_command, output_dir, log_dir, cec_path):
    """Run one benchmark and retain unrounded numeric values for aggregation."""
    output_path = (output_dir / f"{case.output_stem}.aig").resolve()
    log_path = log_dir / f"{case.output_stem}.log"
    if output_path == case.path:
        raise ValueError(f"Refusing to overwrite input AIG: {case.path}")
    output_path.unlink(missing_ok=True)

    before_stats = None
    before_error = ""
    try:
        before_stats = read_aig_stats(case.path)
    except Exception as error:
        before_error = str(error)

    script = build_engine_script(case.path, user_command, output_path)
    engine_returncode, walltime = run_engine(engine_path, script, log_path)

    after_stats = None
    after_error = ""
    if output_path.is_file():
        try:
            after_stats = read_aig_stats(output_path)
        except Exception as error:
            after_error = str(error)
    else:
        after_error = "engine did not create the output AIG"

    cec_status = "not_checked"
    cec_walltime = None
    if engine_returncode == 0 and after_stats is not None and cec_path is not None:
        cec_status, cec_walltime = run_cec(cec_path, case.path, output_path, log_path)

    if engine_returncode != 0:
        status = "engine_error"
    elif before_stats is None or after_stats is None:
        status = "stats_error"
    elif cec_status == "inequivalent":
        status = "inequivalent"
    elif cec_status == "error":
        status = "cec_error"
    elif cec_status == "equivalent":
        status = "equivalent"
    else:
        status = "success"

    with log_path.open("a") as log_handle:
        log_handle.write(f"\n{'=' * 72}\nFinal status: {status}\n")
        if before_error:
            log_handle.write(f"Input statistics error: {before_error}\n")
        if after_error:
            log_handle.write(f"Output statistics error: {after_error}\n")

    size_reduction = reduction_percentage(
        before_stats["size"] if before_stats else None,
        after_stats["size"] if after_stats else None,
    )
    level_reduction = reduction_percentage(
        before_stats["level"] if before_stats else None,
        after_stats["level"] if after_stats else None,
    )

    numeric = {
        "nPIs": Decimal(before_stats["nPIs"]) if before_stats else None,
        "nPOs": Decimal(before_stats["nPOs"]) if before_stats else None,
        "size_before": Decimal(before_stats["size"]) if before_stats else None,
        "level_before": Decimal(before_stats["level"]) if before_stats else None,
        "size_after": Decimal(after_stats["size"]) if after_stats else None,
        "level_after": Decimal(after_stats["level"]) if after_stats else None,
        "size_reduction_percentage": size_reduction,
        "level_reduction_percentage": level_reduction,
        "walltime_seconds": walltime,
        "cec_walltime_seconds": cec_walltime,
    }
    row = {
        "benchmark_name": case.name,
        "nPIs": before_stats["nPIs"] if before_stats else "",
        "nPOs": before_stats["nPOs"] if before_stats else "",
        "size_before": before_stats["size"] if before_stats else "",
        "level_before": before_stats["level"] if before_stats else "",
        "size_after": after_stats["size"] if after_stats else "",
        "level_after": after_stats["level"] if after_stats else "",
        "size_reduction_percentage": format_decimal(size_reduction),
        "level_reduction_percentage": format_decimal(level_reduction),
        "walltime_seconds": format_decimal(walltime),
        "cec_walltime_seconds": format_decimal(cec_walltime),
        "status": status,
    }
    return RunResult(
        row=row,
        numeric=numeric,
        include_in_average=status in {"success", "equivalent"},
    )


def write_summary(results, csv_path):
    """Write rows and averages computed from unrounded in-memory Decimal values."""
    fieldnames = [
        "benchmark_name",
        "nPIs",
        "nPOs",
        "size_before",
        "level_before",
        "size_after",
        "level_after",
        "size_reduction_percentage",
        "level_reduction_percentage",
        "walltime_seconds",
        "cec_walltime_seconds",
        "status",
    ]
    numeric_fields = [
        "nPIs",
        "nPOs",
        "size_before",
        "level_before",
        "size_after",
        "level_after",
        "size_reduction_percentage",
        "level_reduction_percentage",
        "walltime_seconds",
        "cec_walltime_seconds",
    ]
    accepted = [result for result in results if result.include_in_average]

    average_row = {field: "" for field in fieldnames}
    average_row["benchmark_name"] = "average"
    average_row["status"] = f"{len(accepted)}/{len(results)} included"
    for field in numeric_fields:
        values = [result.numeric[field] for result in accepted]
        values = [value for value in values if value is not None]
        if values:
            average_row[field] = format_decimal(sum(values) / Decimal(len(values)))

    with csv_path.open("w", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        writer.writeheader()
        for result in results:
            writer.writerow(result.row)
        writer.writerow(average_row)


def validate_executable(path, label):
    """Resolve and validate one executable path."""
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        raise ValueError(f"{label} executable not found: {resolved}")
    if not os.access(resolved, os.X_OK):
        raise ValueError(f"{label} path is not executable: {resolved}")
    return resolved


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Run an ABC-style -c engine on AIG benchmarks, record engine-only wall time, "
            "optionally run ABC CEC, and generate one consistent CSV summary."
        )
    )
    parser.add_argument(
        "input",
        nargs="?",
        help=(
            "A .aig path, directory, benchmark basename, or built-in benchmark-set name. "
            "Omit it to recursively process all AIGs under --dir."
        ),
    )
    parser.add_argument(
        "--engine-path",
        default=DEFAULT_ENGINE_PATH,
        help=f"Synthesis executable supporting -c. Default: {DEFAULT_ENGINE_PATH}",
    )
    parser.add_argument(
        "--cmd",
        default=None,
        help="Commands placed between read and write in the selected engine.",
    )
    parser.add_argument(
        "--dir",
        nargs="+",
        action="append",
        default=None,
        help=(
            "Search directories for benchmark names/sets, or all-input mode. Accepts multiple, "
            "repeated, or comma-separated paths. Default: benchmarks"
        ),
    )
    parser.add_argument(
        "--output-dir",
        "-o",
        default="outputs",
        help="Directory for optimized AIG files. Default: outputs",
    )
    parser.add_argument(
        "--log-dir",
        "--log-root",
        dest="log_dir",
        default="logs",
        help="Root directory under which a timestamped run directory is created. Default: logs",
    )
    parser.add_argument(
        "--cec",
        action="store_true",
        help="Run a separate ABC CEC after engine timing. Disabled by default.",
    )
    parser.add_argument(
        "--cec-path",
        default=DEFAULT_CEC_PATH,
        help=f"ABC executable used only with --cec. Default: {DEFAULT_CEC_PATH}",
    )
    parser.add_argument(
        "--precision",
        type=int,
        default=DECIMAL_PLACES,
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--list-sets",
        action="store_true",
        help="List built-in benchmark-set names and exit.",
    )
    args = parser.parse_args()

    if args.list_sets:
        for name, benchmarks in BENCHMARK_SETS.items():
            print(f"{name}: {len(benchmarks)}")
        return
    if args.cmd is None:
        parser.error("--cmd is required unless --list-sets is used")

    user_command = normalize_user_command(args.cmd)
    if not user_command:
        parser.error("--cmd must not be empty")

    try:
        engine_path = validate_executable(Path(args.engine_path), "Engine")
        cec_path = validate_executable(Path(args.cec_path), "CEC") if args.cec else None
        search_dirs = parse_search_dirs(args.dir)
        cases = collect_cases(args.input, search_dirs)
    except ValueError as error:
        parser.error(str(error))

    output_dir = Path(args.output_dir).expanduser()
    output_dir.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    log_dir = Path(args.log_dir).expanduser() / f"run_{timestamp}"
    log_dir.mkdir(parents=True, exist_ok=False)

    print(f"Engine: {engine_path}")
    print(f"Command: {user_command}")
    print(f"CEC: {cec_path if cec_path is not None else 'disabled'}")
    print(f"Benchmarks: {len(cases)}")
    print(f"Outputs: {output_dir.resolve()}")
    print(f"Logs: {log_dir.resolve()}")

    results = []
    for index, case in enumerate(cases, start=1):
        print(f"[{index}/{len(cases)}] {case.name} ...", end="", flush=True)
        try:
            result = process_case(
                case,
                engine_path,
                user_command,
                output_dir,
                log_dir,
                cec_path,
            )
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            print(f" fatal error: {error}")
            continue
        results.append(result)
        print(
            f" {result.row['status']} | size {result.row['size_before']} -> "
            f"{result.row['size_after']} | wall {result.row['walltime_seconds']}s"
        )

    if not results:
        print("Error: No benchmark produced a result.")
        sys.exit(1)

    csv_path = log_dir / "summary.csv"
    write_summary(results, csv_path)
    print(f"Summary: {csv_path.resolve()}")


if __name__ == "__main__":
    main()
