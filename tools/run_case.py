#!/usr/bin/env python3
"""Local case runner for MiniVectorDB.

Replays the public cases from ``mysqltest/cases`` against a built binary and
compares the result with the assertion objects in ``.result``.

This is *our* driver, not the organiser's.  The official ``run_case.py`` lives in
their internal repository and is not published, so the point of this script is
to lock down the assertion semantics the public README specifies (``{}``,
``error``, ``affected_rows``, ``columns``, ``rows``, ``unordered``) and to make
protocol drift visible: if the binary changes its framing, this script fails
loudly.

Directives (``--restart``, ``--mode ann``) are not SQL and do not consume an
assertion, matching the README: "同名 .result：JSON 数组，只与 SQL 按顺序一一
对应，驱动指令不占断言位置".

Usage:
    tools/run_case.py --binary build/minivectordb --case sql_crud
    tools/run_case.py --binary build/minivectordb --all
    tools/run_case.py --binary build/minivectordb --all --keep-going
"""
from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CASES = REPO / "mysqltest" / "cases"

TERMINATOR = "END"
FLOAT_ABS_TOL = 1e-5
FLOAT_REL_TOL = 1e-5

# The README freezes the *semantic* error names but says the code mapping is not
# frozen yet: "当前未冻结错误码映射的配置至少要求非空错误码".  We therefore accept
# a semantic name if our stable code is in its alias set, and fall back to
# "any non-empty code" (with a note) for names we have no mapping for.
ERROR_ALIASES: dict[str, tuple[str, ...]] = {
    "duplicate_key": ("ERR_DUP_KEY",),
    "unsupported_sql": ("ERR_UNSUPPORTED",),
    "dimension_mismatch": ("ERR_TYPE",),
}


class DriverError(RuntimeError):
    pass


@dataclass
class Response:
    status: str            # "OK", "AFFECTED", "ROWS", "ERROR"
    affected: int = 0
    columns: list[str] | None = None
    rows: list[list[str]] | None = None
    error: str = ""


class Session:
    """One database process against one data directory."""

    def __init__(self, binary: str, data_dir: str, extra_args: list[str]):
        self.binary = binary
        self.data_dir = data_dir
        self.extra_args = extra_args
        self.proc: subprocess.Popen | None = None
        self._start()

    def _start(self) -> None:
        # stderr goes to a file, never a pipe: the database logs freely there and
        # an undrained pipe would fill up and deadlock the session.
        fd, self.stderr_path = tempfile.mkstemp(prefix="mvdb-stderr-", suffix=".log")
        self.stderr_file = os.fdopen(fd, "wb")
        self.proc = subprocess.Popen(
            [self.binary, "--data-dir", self.data_dir, *self.extra_args],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.stderr_file,
            text=True,
            bufsize=1,
        )
        if self.proc.stdin is None or self.proc.stdout is None:
            raise DriverError("failed to open pipes")

    def execute(self, sql: str) -> Response:
        assert self.proc and self.proc.stdin and self.proc.stdout
        self.proc.stdin.write(sql.rstrip("\n") + "\n")
        self.proc.stdin.flush()

        lines: list[str] = []
        while True:
            line = self.proc.stdout.readline()
            if line == "":
                raise DriverError(f"database exited while handling: {sql}")
            line = line.rstrip("\n")
            if line == TERMINATOR:
                break
            lines.append(line)

        if not lines:
            raise DriverError(f"empty response for: {sql}")

        head = lines[0]
        if head.startswith("ERROR"):
            return Response(status="ERROR", error=head)
        if head.startswith("OK "):
            return Response(status="AFFECTED", affected=int(head[3:].strip()))
        if head == "OK":
            return Response(status="OK")

        columns = head.split("\t")
        rows = [ln.split("\t") for ln in lines[1:]]
        return Response(status="ROWS", columns=columns, rows=rows)

    def restart(self) -> None:
        """Normal shutdown, then start again on the *same* data directory."""
        assert self.proc
        if self.proc.stdin:
            try:
                self.proc.stdin.close()
            except OSError:
                pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            raise DriverError("database did not exit on EOF during --restart")
        self._start()

    def set_mode(self, mode: str) -> Response:
        # Our inferred way of switching modes; the official driver's mechanism is
        # still unpublished (see docs/PROTOCOL.md, item U6).  The trailing
        # semicolon is mandatory: under semicolon framing a statement without a
        # terminator is buffered forever, which shows up as a silent hang.
        return self.execute(f"SET vector_mode = {mode};")

    def close(self) -> None:
        if not self.proc:
            return
        if self.proc.stdin:
            try:
                self.proc.stdin.close()
            except OSError:
                pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        try:
            self.stderr_file.close()
        except OSError:
            pass
        try:
            os.unlink(self.stderr_path)
        except OSError:
            pass

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def parse_scalar(text):
    """Turn a TSV cell into int/float/str the way the .result file encodes it.

    The .result side is already typed JSON, so values pass through unchanged;
    only the TSV side (always text) is coerced.
    """
    if not isinstance(text, str):
        return text
    try:
        if text.strip() == "":
            return text
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        pass
    return text


def scalar_equal(expected, actual) -> bool:
    if isinstance(expected, bool) or isinstance(actual, bool):
        return expected == actual
    if isinstance(expected, (int, float)) and isinstance(actual, (int, float)):
        if isinstance(expected, int) and isinstance(actual, int):
            return expected == actual
        return math.isclose(
            float(expected), float(actual),
            rel_tol=FLOAT_REL_TOL, abs_tol=FLOAT_ABS_TOL,
        )
    return expected == actual


def error_code_of(resp: Response) -> str:
    """Pull the stable code out of 'ERROR <CODE>: <message>'."""
    body = resp.error[len("ERROR"):].strip()
    return body.split(":", 1)[0].strip() if body else ""


def compare(assertion: dict, resp: Response, sql: str,
            unmapped: list[str]) -> list[str]:
    problems: list[str] = []

    if "error" in assertion:
        want = assertion["error"]
        if resp.status != "ERROR":
            return [f"expected error '{want}', but statement succeeded"]
        got = error_code_of(resp)
        if not got:
            return ["expected a non-empty error code, got none"]
        aliases = ERROR_ALIASES.get(want)
        if aliases is None:
            unmapped.append(f"{want} (got {got})")
            return []
        if got not in aliases:
            problems.append(
                f"expected error '{want}' (code {aliases[0]}), got '{got}'"
            )
        return problems

    if resp.status == "ERROR":
        return [f"statement failed but was expected to succeed: {resp.error}"]

    if "affected_rows" in assertion:
        if resp.status != "AFFECTED":
            problems.append(
                f"expected affected_rows={assertion['affected_rows']}, got {resp.status}"
            )
        elif resp.affected != assertion["affected_rows"]:
            problems.append(
                f"expected affected_rows={assertion['affected_rows']}, got {resp.affected}"
            )

    if "columns" in assertion:
        if resp.status != "ROWS":
            problems.append(f"expected a result set, got {resp.status}")
        elif resp.columns != assertion["columns"]:
            problems.append(
                f"expected columns={assertion['columns']}, got {resp.columns}"
            )

    if "rows" in assertion:
        if resp.status != "ROWS":
            problems.append(f"expected {len(assertion['rows'])} row(s), got {resp.status}")
            return problems
        expected_rows = [[parse_scalar(c) for c in row] for row in assertion["rows"]]
        actual_rows = [[parse_scalar(c) for c in row] for row in (resp.rows or [])]
        if assertion.get("unordered"):
            problems.extend(_compare_unordered(expected_rows, actual_rows))
        else:
            problems.extend(_compare_ordered(expected_rows, actual_rows))
    return problems


def _rows_equal(a, b) -> bool:
    return len(a) == len(b) and all(scalar_equal(x, y) for x, y in zip(a, b))


def _compare_ordered(expected, actual) -> list[str]:
    if len(expected) != len(actual):
        return [f"expected {len(expected)} row(s), got {len(actual)}: {actual}"]
    for i, (e, a) in enumerate(zip(expected, actual)):
        if not _rows_equal(e, a):
            return [f"row {i}: expected {e}, got {a}"]
    return []


def _compare_unordered(expected, actual) -> list[str]:
    if len(expected) != len(actual):
        return [f"expected {len(expected)} row(s), got {len(actual)}: {actual}"]
    remaining = list(actual)
    for i, e in enumerate(expected):
        for j, a in enumerate(remaining):
            if _rows_equal(e, a):
                remaining.pop(j)
                break
        else:
            return [f"expected row {e} is missing from {actual}"]
    return []


def load_case(name: str) -> tuple[list[tuple[str, str]], list[dict]]:
    """Return (ops, assertions) where ops are ('sql', text) / ('restart', '') /
    ('mode', 'ann').  Only 'sql' ops line up with assertions."""
    test = CASES / f"{name}.test"
    result = CASES / f"{name}.result"
    if not test.exists() or not result.exists():
        raise DriverError(f"case '{name}' not found under {CASES}")

    ops: list[tuple[str, str]] = []
    for line in test.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith("--"):
            word = line[2:].strip()
            if word == "restart":
                ops.append(("restart", ""))
            elif word.startswith("mode "):
                ops.append(("mode", word[5:].strip()))
            else:
                raise DriverError(f"{name}: unknown directive '{line}'")
        else:
            ops.append(("sql", line))

    assertions = json.loads(result.read_text(encoding="utf-8"))
    sql_count = sum(1 for kind, _ in ops if kind == "sql")
    if sql_count != len(assertions):
        raise DriverError(
            f"{name}: {sql_count} statements but {len(assertions)} assertions"
        )
    return ops, assertions


def run_case(binary: str, name: str, extra_args: list[str], verbose: bool) -> bool:
    ops, assertions = load_case(name)
    tmpdir = tempfile.mkdtemp(prefix=f"mvdb-{name}-")
    failures: list[str] = []
    unmapped: list[str] = []
    sql_index = 0
    try:
        with Session(binary, tmpdir, extra_args) as sess:
            for kind, arg in ops:
                if kind == "restart":
                    if verbose:
                        print("  --restart")
                    try:
                        sess.restart()
                    except DriverError as exc:
                        failures.append(f"  [restart] {exc}")
                        break
                    continue
                if kind == "mode":
                    if verbose:
                        print(f"  --mode {arg}")
                    resp = sess.set_mode(arg)
                    if resp.status == "ERROR":
                        failures.append(f"  [mode {arg}] {resp.error}")
                    continue

                assertion = assertions[sql_index]
                sql_index += 1
                resp = sess.execute(arg)
                problems = compare(assertion, resp, arg, unmapped)
                if problems:
                    for p in problems:
                        failures.append(f"  [{sql_index}] {arg}\n      {p}")
                elif verbose:
                    print(f"  [{sql_index}] ok   {arg}")
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    if failures:
        print(f"FAIL {name}")
        for f in failures:
            print(f)
        return False
    print(f"PASS {name} ({sql_index} statements)")
    if unmapped:
        print(f"  note: unmapped error name(s) accepted on non-empty-code rule: "
              f"{', '.join(unmapped)}")
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=str(REPO / "build" / "minivectordb"))
    ap.add_argument("--case", action="append", default=[])
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--keep-going", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--extra-arg", action="append", default=[],
                    help="extra flag passed to the binary, e.g. --extra-arg=--verbose")
    args = ap.parse_args()

    if not os.access(args.binary, os.X_OK):
        print(f"error: {args.binary} is not executable; build it first", file=sys.stderr)
        return 2

    manifest = json.loads((REPO / "mysqltest" / "manifest.json").read_text(encoding="utf-8"))
    known = [c["id"] for c in manifest["cases"]]
    selected = known if args.all else args.case
    if not selected:
        print("error: pass --all or --case <name>", file=sys.stderr)
        return 2

    passed, failed = [], []
    for name in selected:
        if name not in known:
            print(f"error: '{name}' is not in manifest.json", file=sys.stderr)
            return 2
        ok = run_case(args.binary, name, args.extra_arg, args.verbose)
        (passed if ok else failed).append(name)
        if not ok and not args.keep_going:
            break

    print(f"RESULT: {len(passed)} passed, {len(failed)} failed"
          f" (of {len(selected)})")
    if failed:
        print("failed:", ", ".join(failed))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
