#!/usr/bin/env bash
# Protocol-layer regression tests.
#
# These do NOT test the engine's semantics (the stub has almost none); they pin
# the *framing* behaviour that the hidden driver will depend on, so that a later
# change to the protocol layer cannot silently break it.
#
# Every `check` runs the binary in a fresh data directory, so statements in one
# case cannot leak into another.
#
#   tests/protocol_test.sh [build/minivectordb]
set -uo pipefail

BIN="${1:-build/minivectordb}"
if [[ ! -x "$BIN" ]]; then
  echo "error: $BIN not executable" >&2
  exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0
seq=0

# check <name> <expected> <sql> [extra args...]
check() {
  local name="$1" expected="$2" input="$3"
  shift 3
  seq=$((seq + 1))
  local actual
  actual="$(printf '%s' "$input" | "$BIN" --data-dir "$TMP/d$seq" "$@" 2>/dev/null)"
  if [[ "$actual" == "$expected" ]]; then
    echo "PASS $name"
    pass=$((pass + 1))
  else
    echo "FAIL $name"
    echo "  expected: $(printf '%s' "$expected" | tr '\n' '|')"
    echo "  actual:   $(printf '%s' "$actual" | tr '\n' '|')"
    fail=$((fail + 1))
  fi
}

# --- framing ---------------------------------------------------------------
check "ddl responds OK" \
  "OK
END" "CREATE TABLE a (id BIGINT PRIMARY KEY);
"

check "dml responds affected count" \
  "OK
END
OK 2
END" "CREATE TABLE a (id BIGINT PRIMARY KEY);
INSERT INTO a VALUES (1),(2);
"

check "result set has header then rows" \
  "OK
END
OK 2
END
id
1
2
END" "CREATE TABLE a (id BIGINT PRIMARY KEY);
INSERT INTO a VALUES (2),(1);
SELECT id FROM a ORDER BY id;
"

check "empty result still terminates" \
  "OK
END
id
END" "CREATE TABLE a (id BIGINT PRIMARY KEY);
SELECT id FROM a;
"

check "several statements in one line are split" \
  "OK
END
OK 1
END
id
1
END" "CREATE TABLE a (id BIGINT PRIMARY KEY);
INSERT INTO a VALUES (1); SELECT id FROM a;
"

# --- statement splitting ---------------------------------------------------
check "semicolon inside string does not split" \
  "OK
END
OK 1
END" "CREATE TABLE a (id BIGINT PRIMARY KEY, c VARCHAR(8));
INSERT INTO a VALUES (1, 'x;y');
"

check "multi-line statement" \
  "OK
END
OK 1
END" "CREATE TABLE a (
  id BIGINT PRIMARY KEY
);
INSERT INTO a
  VALUES (1);
"

check "line comments and block comments ignored" \
  "OK
END
OK 1
END" "-- leading comment
CREATE TABLE a (id BIGINT PRIMARY KEY); # trailing
/* block ; comment */
INSERT INTO a VALUES (1);
"

check "trailing statement without semicolon at EOF" \
  "OK
END" "CREATE TABLE a (id BIGINT PRIMARY KEY)
"

check "blank input produces no output" \
  "" "
 
"

# --- errors ----------------------------------------------------------------
check "unsupported statement returns ERROR and keeps running" \
  "ERROR ERR_UNSUPPORTED: unsupported statement 'grant'
END
OK
END" "GRANT ALL ON *.* TO x;
CREATE TABLE a (id BIGINT PRIMARY KEY);
"

check "missing table is an error, not a crash" \
  "ERROR ERR_NO_TABLE: table 'nope' does not exist
END
OK
END" "SELECT id FROM nope;
CREATE TABLE a (id BIGINT PRIMARY KEY);
"

check "duplicate primary key rejected atomically" \
  "OK
END
OK 2
END
ERROR ERR_DUP_KEY: duplicate primary key 1
END
id
1
9
END" "CREATE TABLE a (id BIGINT PRIMARY KEY);
INSERT INTO a VALUES (9),(1);
INSERT INTO a VALUES (5),(1);
SELECT id FROM a ORDER BY id;
"

check "vector dimension mismatch is rejected" \
  "OK
END
ERROR ERR_TYPE: vector dimension mismatch: expected 3, got 2
END" "CREATE TABLE a (id BIGINT PRIMARY KEY, v VECTOR(3));
INSERT INTO a VALUES (1, '[1,2]');
"

# --- value domain ----------------------------------------------------------
# int64 boundaries: 9223372036854775807 is not representable as a double, so a
# double-based parse would mangle it.  Both extremes must round-trip exactly and
# the first value past each end must be rejected.
check "BIGINT boundaries are exact" \
  "OK
END
ERROR ERR_TYPE: value is not a valid BIGINT
END
OK 1
END
OK 1
END
ERROR ERR_TYPE: value is not a valid BIGINT
END
ERROR ERR_TYPE: value is not a valid BIGINT
END
id
-9223372036854775808
9223372036854775807
END" "CREATE TABLE a (id BIGINT PRIMARY KEY);
INSERT INTO a VALUES (9223372036854775808);
INSERT INTO a VALUES (9223372036854775807);
INSERT INTO a VALUES (-9223372036854775808);
INSERT INTO a VALUES (-9223372036854775809);
INSERT INTO a VALUES (1.5);
SELECT id FROM a ORDER BY id;
"

check "VARCHAR length counts code points, not bytes" \
  "OK
END
OK 1
END
ERROR ERR_TYPE: string too long for VARCHAR(3) column 'c'
END
id	c
1	中文词
END" "CREATE TABLE a (id BIGINT PRIMARY KEY, c VARCHAR(3));
INSERT INTO a VALUES (1, '中文词');
INSERT INTO a VALUES (2, '中文词语');
SELECT id, c FROM a;
"

check "VARCHAR comparison follows code point order" \
  "OK
END
OK 3
END
id
3
1
2
END" "CREATE TABLE a (id BIGINT PRIMARY KEY, c VARCHAR(9));
INSERT INTO a VALUES (1,'乙'),(2,'甲'),(3,'a');
SELECT id FROM a ORDER BY c;
"

check "NaN and Inf are rejected in vectors" \
  "OK
END
ERROR ERR_TYPE: vector component must be finite
END
ERROR ERR_TYPE: vector component must be finite
END" "CREATE TABLE a (id BIGINT PRIMARY KEY, v VECTOR(2));
INSERT INTO a VALUES (1,'[nan,1]');
INSERT INTO a VALUES (2,'[inf,1]');
"

# --- configurable profiles -------------------------------------------------
check "--ready-marker appends token line" \
  "OK
END
@@DONE@@" "CREATE TABLE a (id BIGINT PRIMARY KEY);
" --ready-marker '@@DONE@@'

check "--numeric-errors renders a numeric code" \
  "ERROR 1: unsupported statement 'grant'
END" "GRANT ALL ON *.* TO x;
" --numeric-errors

check "--output-format jsonl emits one object per statement" \
  '{}' "CREATE TABLE a (id BIGINT PRIMARY KEY);
" --output-format jsonl

check "--statement-mode line treats each line as a statement" \
  "OK
END
id
END" "CREATE TABLE a (id BIGINT PRIMARY KEY)
SELECT id FROM a
" --statement-mode line

# --- stream hygiene --------------------------------------------------------
stderr_out="$(printf 'CREATE TABLE a (id BIGINT PRIMARY KEY);\n' \
  | "$BIN" --data-dir "$TMP/verbose" --verbose 2>&1 >/dev/null)"
if [[ -n "$stderr_out" ]]; then
  echo "PASS --verbose logs to stderr only"
  pass=$((pass + 1))
else
  echo "FAIL --verbose produced no stderr output"
  fail=$((fail + 1))
fi

stdout_only="$(printf 'CREATE TABLE a (id BIGINT PRIMARY KEY);\n' \
  | "$BIN" --data-dir "$TMP/quiet" 2>/dev/null)"
if [[ "$stdout_only" != *"[sql]"* ]]; then
  echo "PASS stdout carries no log lines"
  pass=$((pass + 1))
else
  echo "FAIL log lines leaked into stdout"
  fail=$((fail + 1))
fi

echo
echo "protocol tests: $pass passed, $fail failed"
[[ "$fail" -eq 0 ]]
