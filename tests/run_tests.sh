#!/usr/bin/env bash
# All interop checks: official Python SDK client -> our server, our C++ client -> our server
# and -> the Python reference server, over stdio and Streamable HTTP.
#   PY=path/to/python-with-mcp bash tests/run_tests.sh [bin_dir]
set -u
PY=${PY:-python}
BIN=${1:-build/Release}
EXT=""; [ -f "$BIN/demo_server.exe" ] && EXT=".exe"
SERVER="$BIN/demo_server$EXT"; CHECK="$BIN/client_check$EXT"
export PYTHONIOENCODING=utf-8 PYTHONWARNINGS=ignore
fail=0
t() { local name=$1; shift; if timeout 180 "$@" > /tmp/mcp_t.log 2>&1; then echo "PASS $name"; else echo "FAIL $name"; tail -5 /tmp/mcp_t.log; fail=1; fi; }
SERVER_NATIVE=$(cygpath -w "$SERVER" 2>/dev/null || echo "$SERVER")
PY_NATIVE=$(cygpath -w "$(command -v "$PY")" 2>/dev/null || echo "$PY")

t "python client -> C++ server (stdio)" "$PY" tests/py_client_test.py stdio "$SERVER_NATIVE"
t "C++ client -> C++ server (stdio)" "$CHECK" --stdio "$SERVER"
t "C++ client -> Python server (stdio)" "$CHECK" --stdio "$PY_NATIVE" tests/py_ref_server.py

"$SERVER" --http 8000 > /dev/null 2>&1 & S1=$!
"$PY" tests/py_ref_server.py --http 8001 > /dev/null 2>&1 & S2=$!
for _ in $(seq 1 50); do curl -s -o /dev/null http://127.0.0.1:8001/mcp && curl -s -o /dev/null http://127.0.0.1:8000/mcp && break; sleep 0.2; done
t "python client -> C++ server (http)" "$PY" tests/py_client_test.py http http://127.0.0.1:8000/mcp
t "C++ client -> C++ server (http)" "$CHECK" --http http://127.0.0.1:8000/mcp
t "C++ client -> Python server (http)" "$CHECK" --http http://127.0.0.1:8001/mcp
kill $S1 $S2 2>/dev/null; wait $S1 $S2 2>/dev/null
exit $fail
