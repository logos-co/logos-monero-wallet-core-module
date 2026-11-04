#!/bin/sh
# One "Use Tor for" mode end to end: modes.sh identifying|all|none|stalled
# stalled is identifying with the direct node's block requests held for good.
# Needs LOGOS (a logosctl bundle), TREE (modules dir with tor, node and core). A mainnet node
# (UPSTREAM, or the first of a list that answers) stands behind both fronts: http = the direct
# node, socks = Tor to a fake onion.
# Mainnet, as its last checkpoint is close to the tip: a wallet restored just below it catches
# up in a minute, where a stagenet one pulls 1.7M block hashes first.
set -eu
MODE=$1
TOR_USE=$MODE; [ "$MODE" = stalled ] && TOR_USE=identifying
HERE=$(cd "$(dirname "$0")" && pwd)
UPSTREAMS=${UPSTREAM:-xmr-node.cakewallet.com:18081 node.sethforprivacy.com:18089 nodes.hashvault.pro:18081 node.monerodevs.org:18089}
ONION=abcdefghijklmnopqrstuvwxyz234567abcdefghijklmnopqrstuvwx.onion
# monero's functional-test seed: public, so the run builds a send and never broadcasts one.
SEED="velvet lymph giddy number token physics poetry unquoted nibs useful sabotage limits benches lifestyle eden nitrogen anvil fewest avoid batch vials washing fences goat unquoted"
W=$PWD/modes-$MODE
rm -rf "$W"; mkdir -p "$W/home"
export LOGOSCTL_CONFIG_DIR="$W/session"
ctl() { "$LOGOS/bin/logosctl" "$@"; }
field() { python3 -c 'import json,sys;print(json.loads(sys.stdin.read())["result"]["value"][sys.argv[1]])' "$1"; }
job() {
  j=$(ctl call monero_wallet_core_module startJob "$1" "json:$2")
  id=$(printf '%s' "$j" | field jobId); rc=$(printf '%s' "$j" | field receipt)
  for _ in $(seq 1 90); do
    s=$(ctl call monero_wallet_core_module jobStatus "$id" "$rc")
    case "$s" in *'"done"'*|*'"failed"'*) break;; esac
    sleep 1
  done
  printf '%s\n' "$s"
}
status_until() {
  for _ in $(seq 1 "$2"); do
    st=$(ctl call monero_wallet_core_module status) || st=""
    case "$st" in *"$1"*) printf '%s\n' "$st"; return 0;; esac
    sleep 2
  done
  printf '%s\n' "$st"; return 1
}
count() { grep -c "$1" "$2" || true; }
some() { sed 's/: [1-9][0-9]*$/: some/'; }

# The first public node that answers, asked directly so neither front logs it.
for UPSTREAM in $UPSTREAMS; do
  TIP=$(python3 -c 'import json,sys,urllib.request
r=urllib.request.urlopen(urllib.request.Request("http://"+sys.argv[1]+"/json_rpc",
  json.dumps({"jsonrpc":"2.0","id":"0","method":"get_info"}).encode(),{"Content-Type":"application/json"}),timeout=15)
print(json.load(r)["result"]["height"])' "$UPSTREAM" 2> /dev/null) && break
done
echo "upstream $UPSTREAM at $TIP"

python3 -I "$HERE/fronts.py" http 0 "$UPSTREAM" "$W/direct.log" "$W/hold" > "$W/http.port" &
HTTP_PID=$!
python3 -I "$HERE/fronts.py" socks 0 "$UPSTREAM" "$W/tor.log" > "$W/socks.port" &
SOCKS_PID=$!
trap 'ctl daemon stop > /dev/null 2>&1 || true; kill $HTTP_PID $SOCKS_PID 2> /dev/null || true' EXIT
trap 'exit 1' INT TERM
for _ in $(seq 1 50); do [ -s "$W/http.port" ] && [ -s "$W/socks.port" ] && break; sleep 0.1; done
HTTP_PORT=$(cat "$W/http.port"); SOCKS_PORT=$(cat "$W/socks.port")

mkdir -p "$LOGOSCTL_CONFIG_DIR"
printf 'modules_dirs: [%s]\n' "$TREE" | ctl daemon config set - > /dev/null
HOME="$W/home" ctl daemon start --detach > /dev/null
# Only the wallet backend may set "Use Tor for": admit logosctl, read when the node loads.
ctl module load monero_node_module > /dev/null; sleep 1
printf '%s\n' '{"allowHost":true}' > "$(ls -d "$LOGOSCTL_CONFIG_DIR"/data/monero_node_module/*/ | head -1)callers.json"
ctl module unload monero_node_module > /dev/null; sleep 1
ctl module load monero_wallet_core_module > /dev/null
ctl call monero_node_module set_tor_use "$TOR_USE"
ctl call monero_node_module set_node_config mainnet \
  "str:{\"url\":\"http://127.0.0.1:$HTTP_PORT\",\"onionUrl\":\"http://$ONION:18089\",\"proxy\":\"socks5h://127.0.0.1:$SOCKS_PORT\"}"
ctl call monero_node_module effective_node mainnet

[ "$TOR_USE" = identifying ] && touch "$W/hold"
job restore_from_seed "{\"name\":\"modes\",\"password\":\"pw\",\"network\":\"mainnet\",\"seed\":\"$SEED\",\"restoreHeight\":$((TIP - 20))}"
# To itself: the wallet holds nothing, so a send gets no further than its funds check.
send() {
  a=$(printf '%s' "$st" | python3 -c 'import json,sys;print(json.loads(sys.stdin.read())["result"]["address"])')
  job create_transaction "{\"address\":\"$a\",\"amount\":\"1000000\"}"
}

if [ "$MODE" = stalled ]; then
  for _ in $(seq 1 60); do [ "$(count '\.bin' "$W/direct.log")" -gt 0 ] && break; sleep 1; done
  # While wallet2 waits on the held request; logosctl's own deadline is 20 s.
  t0=$(date +%s); st=$(ctl call monero_wallet_core_module status) || st=""; t1=$(date +%s)
  [ $((t1 - t0)) -le 3 ] && echo "status while held: answered" || echo "status while held: $((t1 - t0)) s"
  printf '%s\n' "$st" | grep -o '"route":"[a-z_]*"' || true
  ctl call monero_wallet_core_module address 0 0 | grep -q '"result":"4' && echo "address while held: answered"
  # Never released: no new block for 90 s, and the engine drops the direct node to move.
  status_until '"route":"tor"' 240
  t2=$(date +%s); [ $((t2 - t0)) -le 150 ] && echo "moved to Tor: within 150 s" || echo "moved to Tor: after $((t2 - t0)) s"
  BEFORE=$(count . "$W/direct.log")
  status_until '"synchronized":true' 90
  # "connected" is the probe's reading, every 3 s, so it can trail the sync by a round.
  status_until '"connected":true' 10
  echo "direct node, requests after the switch: $(( $(count . "$W/direct.log") - BEFORE ))"
  rm -f "$W/hold"
elif [ "$MODE" = identifying ]; then
  for _ in $(seq 1 60); do [ "$(count '\.bin' "$W/direct.log")" -gt 0 ] && break; sleep 1; done
  echo "send while catching up:"
  job create_transaction '{"address":"","amount":"1"}'
  echo "SOCKS connections while catching up: $(count '^connect' "$W/tor.log")"
  rm -f "$W/hold"
  status_until '"route":"tor"' 90
  BEFORE=$(count . "$W/direct.log")
  TOR_BEFORE=$(count '\.bin' "$W/tor.log")
  status_until '"synchronized":true' 60
  echo "send over Tor:"
  send
  sleep 5
  echo "direct node, requests after the switch: $(( $(count . "$W/direct.log") - BEFORE ))"
  echo "over Tor, block requests after the switch: $(( $(count '\.bin' "$W/tor.log") - TOR_BEFORE ))" | some
else
  status_until '"synchronized":true' 90
  echo "send:"
  send
  sleep 5
fi
echo "direct node, decoy or broadcast requests: $(( $(count get_outs "$W/direct.log") + $(count send_raw "$W/direct.log") + $(count sendrawtransaction "$W/direct.log") ))"
echo "direct node, requests: $(count . "$W/direct.log")" | some
echo "SOCKS connections: $(count '^connect' "$W/tor.log")" | some
job close_wallet '{}' > /dev/null
ctl call monero_node_module tor_use
