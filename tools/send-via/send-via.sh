#!/bin/sh
# Sending through a public node over Tor, end to end on a private stagenet node: see driver.cpp.
# Needs MONERO_C and MONEROD_C (the prefixes of logos-monero-nix's monero-c and monerod-c) and
# JSON_INC (nlohmann_json's include dir); CXX defaults to c++. Works in ./send-via.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CXX=${CXX:-c++}
W=$PWD/send-via
rm -rf "$W"; mkdir -p "$W/wallets" "$W/chain"

"$CXX" -std=c++17 -O1 -DMONERO_C_VERSION=\"test\" -I"$ROOT/src" -I"$MONERO_C/include" -I"$JSON_INC" \
  "$ROOT/src/wallet_runtime.cpp" "$ROOT/src/address_check.cpp" "$HERE/driver.cpp" \
  -L"$MONERO_C/lib" -lmonero_wallet2_api_c -Wl,-rpath,"$MONERO_C/lib" -lpthread -o "$W/driver"
"$CXX" -std=c++17 -I"$MONEROD_C/include" "$HERE/chaind.cpp" \
  -L"$MONEROD_C/lib" -lmonerod_c -Wl,-rpath,"$MONEROD_C/lib" -lpthread -o "$W/chaind"

free_port() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
RPC=$(free_port); DEAD=$(free_port)

PIDS=""
trap 'for p in $PIDS; do kill "$p" 2> /dev/null || true; done; for p in $PIDS; do wait "$p" 2> /dev/null || true; done' EXIT
trap 'exit 1' INT TERM
"$W/chaind" "$W/chain" "$RPC" > "$W/chaind.log" 2>&1 &
PIDS="$!"
for _ in $(seq 1 120); do
  python3 -c 'import sys,urllib.request;urllib.request.urlopen("http://127.0.0.1:"+sys.argv[1]+"/get_height",timeout=2)' "$RPC" 2> /dev/null && break
  sleep 0.5
done
python3 -I "$ROOT/tools/tor-modes/fronts.py" http 0 "127.0.0.1:$RPC" "$W/own.log" > "$W/own.port" &
PIDS="$PIDS $!"
python3 -I "$ROOT/tools/tor-modes/fronts.py" socks 0 "127.0.0.1:$RPC" "$W/public.log" > "$W/socks.port" &
PIDS="$PIDS $!"
for _ in $(seq 1 50); do [ -s "$W/own.port" ] && [ -s "$W/socks.port" ] && break; sleep 0.1; done

"$W/driver" "$W/wallets" "$RPC" "$(cat "$W/own.port")" "$W/own.log" "$(cat "$W/socks.port")" "$W/public.log" "$DEAD"
