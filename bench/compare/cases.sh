#!/usr/bin/env bash
# The run.sh --cases loop (plan 2026-11-12 Task 5): a differential gate, TestKvrocksCases against both bench
# containers, then the production-style datasets on each server in turn and timed runs of the cases that passed.
# Sourced by run.sh after lib.sh and workloads_cases.sh; uses run.sh's settle, stop_sampler, _du, _server_failed,
# remove_data, write_meta and _load_cb.
# shellcheck disable=SC2034  # SERVER_CONTAINER/SERVER_PORT locals are read by cli; META_EXTRA/W6_* by write_meta
GO_IMAGE=golang:1.22
# the longest wait for a Kvrocks DBSIZE SCAN to land, in seconds
CASE_SCAN_MAX=900
CASE_EXP_END=0

# ---- the differential gate ----

_diff_name() { echo "tideskv-bench-diff-$1-$TS"; }
_diff_port() { if [[ $1 == tideskv ]]; then echo 6379; else echo 6666; fi; }

# Starts one diff server (relaxed config, the same limits as start_server) and waits for its PING.
_diff_start() {
  local s=$1 c data="$OUT/data/diff-$1" conf i
  c=$(_diff_name "$s")
  mkdir -p "$data"; chmod 0777 "$data"
  _track "$c"
  conf=$(write_server_conf "$s" relaxed) || return 1
  local common=(--detach --name "$c" --network "$NET" --cpuset-cpus "$SERVER_CPUS"
                --memory "$SERVER_MEM" --memory-swap "$SERVER_MEM" --ulimit "nofile=$SERVER_NOFILE:$SERVER_NOFILE")
  if [[ $s == tideskv ]]; then
    docker run "${common[@]}" -v "$data:/data" -v "$conf:/etc/tideskv.conf:ro" "$TIDESKV_IMAGE" /etc/tideskv.conf \
      >/dev/null || return 1
  else
    docker run "${common[@]}" -v "$data:/var/lib/kvrocks/data" -v "$conf:/etc/kvrocks.conf:ro" \
      --entrypoint kvrocks "$KVROCKS_IMAGE" -c /etc/kvrocks.conf >/dev/null || return 1
  fi
  local SERVER_CONTAINER=$c SERVER_PORT
  SERVER_PORT=$(_diff_port "$s")
  for ((i = 0; i < 60; i++)); do
    if [[ $(cli PING 2>/dev/null) == PONG ]]; then log "diff $s is up"; return 0; fi
    sleep 1
  done
  log "diff $s did not answer PING; last log lines:"
  docker logs --tail 30 "$c" >&2 || true
  return 1
}

_diff_stop() {
  local c; c=$(_diff_name "$1")
  docker logs "$c" >>"$OUT/$1/diff-server.log" 2>&1 || true
  docker rm -f "$c" >/dev/null 2>&1 || true
}

# Runs TestKvrocksCases from a Go container on $NET; prints go test's -json stream (and any error text).
# The build cache is a writable copy of the host's; the host's module cache is mounted read-only when it exists.
_diff_go_test() {
  local cache="$OUT/go-cache" hc hm extra=()
  hc=$(go env GOCACHE 2>/dev/null) || hc=""
  hm=$(go env GOMODCACHE 2>/dev/null) || hm=""
  mkdir -p "$cache"
  if [[ -n $hc && -d $hc ]]; then cp -a "$hc/." "$cache/" 2>/dev/null || log "could not copy the Go build cache $hc"; fi
  if [[ -n $hm && -d $hm ]]; then extra+=(-v "$hm:/gomod:ro" -e GOMODCACHE=/gomod -e GOFLAGS=-mod=mod -e GOPROXY=off); fi
  if ! docker image inspect "$GO_IMAGE" >/dev/null 2>&1; then docker pull -q "$GO_IMAGE" >/dev/null 2>&1 || true; fi
  local _rc=0 gc="tideskv-bench-diff-go-$TS"
  _track "$gc"
  docker run --rm --name "$gc" --network "$NET" --cpuset-cpus "$CLIENT_CPUS" \
    --user "$(id -u):$(id -g)" -e HOME=/tmp -e GOCACHE=/gocache -v "$cache:/gocache" \
    -v "$ROOT/tests/compat:/src:ro" -w /src "${extra[@]}" \
    -e "TIDESKV_OURS_ADDR=$(_diff_name tideskv):6379" -e "TIDESKV_KVROCKS_ADDR=$(_diff_name kvrocks):6666" \
    -e TIDESKV_DIFF_REQUIRED=1 "$GO_IMAGE" go test ./diff -run TestKvrocksCases -json -count=1 2>&1 || _rc=$?
  rm -rf "$cache" 2>/dev/null || true
  return $_rc
}

# Turns go test's -json stream ($1) into cases-diff.json and cases-diff.log. A case is "diverged" when a seed failed,
# "pass" when its subtest passed, and "diff did not run" otherwise (no parseable results, or a setup failure).
_diff_parse() {
  # shellcheck disable=SC2086  # one argument per case
  python3 - "$1" "$2" "$OUT/cases-diff.json" "$OUT/cases-diff.log" $CASES <<'EOF'
import json, re, sys
raw, rc, out_json, out_log, cases = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:]
case_re, seed_re = re.compile(r"^TestKvrocksCases/(C\d+)$"), re.compile(r"^TestKvrocksCases/(C\d+)/seed-\d+$")
case_act, seed_fail, text = {}, set(), []
with open(raw, errors="replace") as f:
    for line in f:
        try:
            ev = json.loads(line)
        except ValueError:
            ev = None
        if not isinstance(ev, dict):
            text.append(line)
            continue
        text.append(ev.get("Output") or "")
        name, act = ev.get("Test") or "", ev.get("Action")
        m = case_re.match(name)
        if m and act in ("pass", "fail"):
            case_act[m.group(1)] = act
        m = seed_re.match(name)
        if m and act == "fail":
            seed_fail.add(m.group(1))
status = {}
for c in cases:
    if c in seed_fail:
        status[c] = "diverged"
    elif case_act.get(c) == "pass":
        status[c] = "pass"
    else:
        status[c] = "diff did not run"
with open(out_json, "w") as f:
    json.dump(status, f, indent=2)
with open(out_log, "w") as f:
    f.write("go test exited %s\n" % rc)
    f.writelines(text)
EOF
}

# Starts both servers, runs TestKvrocksCases against them and writes cases-diff.{json,log}.
run_case_diff() {
  local raw="$OUT/cases-diff.raw" rc=0 s
  : >"$raw"
  log "diff: TestKvrocksCases against both servers"
  _diff_start tideskv && _diff_start kvrocks || rc=$?
  if ((rc)); then echo "a diff server did not start; see */diff-server.log" >"$raw"
  else _diff_go_test >"$raw" || rc=$?; fi
  for s in tideskv kvrocks; do _diff_stop "$s"; done
  _diff_parse "$raw" "$rc" || { log "could not parse the diff output; see $raw"; return 1; }
  rm -f "$raw"
  remove_data
  log "diff: $(tr -d '\n ' <"$OUT/cases-diff.json")"
}

# The cases cases-diff.json marks "pass", in CASES order.
cases_passed() {
  [[ -f $OUT/cases-diff.json ]] || return 0
  # shellcheck disable=SC2086  # one argument per case
  python3 -c 'import json, sys
d = json.load(open(sys.argv[1]))
print(" ".join(c for c in sys.argv[2:] if d.get(c) == "pass"))' "$OUT/cases-diff.json" $CASES
}

# ---- loads and the expiry measurement ----

# cache_exp first, so its keys expire during the other loads; CASE_EXP_END is when that load ended on this server.
cases_load_all() {
  local ds start order=cache_exp
  start=$(_q_now)
  for ds in $CASE_DATASETS; do [[ $ds == cache_exp ]] || order+=" $ds"; done
  for ds in $order; do
    LOAD_NAME=$ds LOAD_N=1
    log "loading $ds"
    wc_load "$ds" _load_cb || { log "load of $ds failed; see $CUR/load/"; return 1; }
    if [[ $ds == cache_exp ]]; then CASE_EXP_END=$(_q_now); fi
  done
  load_check wc_verify_cmds || return 1
  LOAD_SECONDS=$(( $(_q_now) - start ))
}

_kv_scan_ts() { cli INFO keyspace 2>/dev/null | tr -d '\r' | sed -n 's/^last_dbsize_scan_timestamp://p'; }

# The server's key count now. On Kvrocks: a DBSIZE SCAN, then DBSIZE polled until its count changes or the scan's
# timestamp moves (the scan landed with the same count), for at most CASE_SCAN_MAX seconds.
_cases_dbsize() {
  if [[ $SERVER_NAME != kvrocks* ]]; then cli DBSIZE; return; fi
  local n0 ts0 n ts t0
  n0=$(cli DBSIZE); ts0=$(_kv_scan_ts)
  cli DBSIZE SCAN >/dev/null
  t0=$(_q_now)
  while :; do
    ts=$(_kv_scan_ts)  # before the count: a moved timestamp means the count read next is the new scan's
    n=$(cli DBSIZE)
    if [[ $n != "$n0" || ( -n $ts && $ts != "$ts0" ) ]]; then break; fi
    if (( $(_q_now) - t0 >= CASE_SCAN_MAX )); then log "DBSIZE SCAN did not land within ${CASE_SCAN_MAX}s"; break; fi
    _q_sleep
  done
  echo "$n"
}

# Waits until C1_EXPIRE_S seconds have passed since this server's cache_exp load ended, settles, then records the key
# count and data dir size in <server>/expiry.json. keys_loaded is the C1 keyspace; keys_other is what the other
# datasets planned, so DBSIZE can be read against keys_live_expected + keys_other.
cases_expiry_check() {
  local data=$1 t0 b0 waited n
  b0=$(_du "$data")
  t0=$(_q_now)
  while (( $(_q_now) - CASE_EXP_END < C1_EXPIRE_S )); do _q_sleep; done
  waited=$(( $(_q_now) - t0 ))
  log "$CUR: waited ${waited}s for the cache_exp keys to expire"
  settle "$OUT/$CUR/settle-expiry" || return 1
  n=$(_cases_dbsize)
  [[ $n =~ ^[0-9]+$ ]] || { log "$CUR: DBSIZE after expiry is [$n]"; return 1; }
  printf '{"keys_loaded": %s, "keys_live_expected": %s, "keys_other": %s, "dbsize_after": %s, "bytes_before": %s, "bytes_after": %s, "waited_s": %s}\n' \
    "$C1_KEYS" $((C1_KEYS * 8 / 10)) $((C2_KEYS + C3_KEYS + C4_SETS + C5_BOARDS + C6_LISTS + C7_KEYS)) \
    "$n" "$b0" "$(_du "$data")" "$waited" >"$OUT/$CUR/expiry.json"
}

# ---- one case ----

# C8 has no probe of its own: it runs C1, C2 and C5
_case_probe_lines() {
  if [[ $1 == C8 ]]; then wc_probe C1; wc_probe C2; wc_probe C5; else wc_probe "$1"; fi
}

# Runs the case's probe lines; prints "[line]: reply" for the first ERR or WRONGTYPE reply and fails.
_case_probe() {
  local line reply
  while read -r line; do
    # shellcheck disable=SC2086
    reply=$(cli $line 2>&1)
    if [[ $reply == ERR* || $reply == WRONGTYPE* ]]; then echo "[${line:0:60}]: $reply"; return 1; fi
  done < <(_case_probe_lines "$1")
}

# The C8 parts in the background, each with MEMTIER_RUN_FLAGS and its own threads/clients: _c8_run <base> <seconds>
# writes <base>.<part>.{json,log} and fails when any part fails.
_c8_cb() {
  local part=$1; shift
  memtier "$C8_BASE.$part" --test-time="$C8_TIME" "$@" "${MEMTIER_RUN_FLAGS[@]}" & C8_PIDS+=($!)
}
_c8_run() {
  local p rc=0
  C8_BASE=$1 C8_TIME=$2 C8_PIDS=()
  wc_c8_parts _c8_cb || rc=1
  for p in "${C8_PIDS[@]}"; do wait "$p" || rc=1; done
  return $rc
}

# The timed window around "$@": docker stats, the cgroup CPU and write deltas, the status, and a liveness check.
_case_measure() {
  local base=$1 spid cpu0 cpu1 rc=0; shift
  stats_sampler "$SERVER_CONTAINER" >"$OUT/$base.stats.jsonl" & spid=$!
  cpu0=$(cg_sample) || cpu0=""
  "$@" || rc=$?
  cpu1=$(cg_sample) || cpu1=""
  stop_sampler "$spid"
  if ((rc)); then echo "memtier exited $rc; see $base*.log" >"$OUT/$base.status"; else echo ok >"$OUT/$base.status"; fi
  if [[ -n $cpu0 && -n $cpu1 ]]; then
    echo $(( ${cpu1%% *} - ${cpu0%% *} )) >"$OUT/$base.cpu"
    echo $(( ${cpu1##* } - ${cpu0##* } )) >"$OUT/$base.io"
  fi
  if [[ $(docker inspect -f '{{.State.Running}}' "$SERVER_CONTAINER" 2>/dev/null) != true ]]; then
    echo "server stopped during the run (OOM-killed: $(docker inspect -f '{{.State.OOMKilled}}' "$SERVER_CONTAINER"))" >"$OUT/$base.status"
    return 1
  fi
}

# Probe, settle, warm-up and the timed run of case $2 into $1.*; further arguments are extra memtier flags (the rate).
_case_run() {
  local base=$1 c=$2 err; shift 2
  mkdir -p "$(dirname "$OUT/$base")"
  if ! err=$(_case_probe "$c"); then echo "probe: $err" >"$OUT/$base.status"; return 0; fi
  settle "$OUT/$base.settle" || { echo "could not read the server's cgroup to wait for it to settle" >"$OUT/$base.status"; return 0; }
  if [[ $c == C8 ]]; then
    _c8_run "$base.warmup" "$WARMUP" || true
    _case_measure "$base" _c8_run "$base" "$DURATION"
    return
  fi
  wc_run "$c" _run_cb || { echo "wc_run $c failed" >"$OUT/$base.status"; return 0; }
  local common=(--threads="$THREADS" --clients="$CLIENTS" --pipeline=1 "${MEMTIER_RUN_FLAGS[@]}" "$@")
  memtier "$base.warmup" "${common[@]}" --test-time="$WARMUP" "${RUN_ARGS[@]}" || true
  _case_measure "$base" memtier "$base" "${common[@]}" --test-time="$DURATION" "${RUN_ARGS[@]}"
}

# run_case <level> <C> <k>: repeat k of case C. Fails only when the server died.
run_case() { _case_run "$CUR/$1/$2.r$3" "$2"; }

# run_case_rate <level> <C>: case C at CASE_RATE ops/s in total, spread over THREADS x CLIENTS connections.
run_case_rate() { _case_run "$CUR/$1/$2.rate" "$2" "--rate-limiting=$(wc_rate_per_conn "$THREADS" "$CLIENTS")"; }

# ---- one server, the whole mode ----

# Every case in $2... at level $1: the repeats, the rate pass (not C8), then the data dir size after a settle.
_cases_level() {
  local level=$1 c k; shift
  for c in "$@"; do
    for ((k = 1; k <= CASE_REPEATS; k++)); do
      log "$CUR $level $c r$k"
      run_case "$level" "$c" "$k" || return 1
    done
    if [[ $c != C8 ]]; then log "$CUR $level $c rate"; run_case_rate "$level" "$c" || return 1; fi
    settle "$OUT/$CUR/$level/$c.du-settle" || true
    _du "$OUT/data/$CUR" >"$OUT/$CUR/$level/$c.du"
  done
}

run_cases_server() {
  local server=$1 level after data="$OUT/data/$1" passed
  CUR=$server
  passed=$(cases_passed)
  if [[ -z $passed ]]; then log "$server: no case passed the diff; nothing to time"; return 0; fi
  start_server "$server" relaxed "$data" || { _server_failed "server did not start (relaxed)"; return 1; }
  parity_check "$server" relaxed || { _server_failed "parity check failed (relaxed); see $server/config.txt"; return 1; }
  cases_load_all || { _server_failed "load failed or the load check did not pass; see $server/load/"; return 1; }
  settle "$OUT/$server/settle-load" || { _server_failed "could not read the server's cgroup"; return 1; }
  after=$(_du "$data")
  cases_expiry_check "$data" || { _server_failed "the expiry check failed"; return 1; }
  # relaxed and strict share one data dir, as in run_server
  for level in relaxed strict; do
    if [[ $level == strict ]]; then
      stop_server
      start_server "$server" strict "$data" || { _server_failed "server did not start (strict)"; return 1; }
      parity_check "$server" strict || { _server_failed "parity check failed (strict); see $server/config.txt"; return 1; }
      settle "$OUT/$server/settle-restart" || { _server_failed "could not read the server's cgroup"; return 1; }
    fi
    # shellcheck disable=SC2086
    _cases_level "$level" $passed || { log "$server died; skipping its remaining cases"; break; }
  done
  stop_server
  printf '{"after_load_bytes": %s, "end_bytes": %s, "load_seconds": %s, "logical_bytes": %s}\n' \
    "$after" "$(_du "$data")" "$LOAD_SECONDS" "$(wc_logical_bytes)" >"$OUT/$server/disk.json"
}

# disk_check's formula over the cases' logical bytes: 3x logical + 32 GiB for the one server on disk at a time.
cases_disk_check() {
  local need free
  need=$(( $(wc_logical_bytes) * 3 + 32 * 1024 * 1024 * 1024 ))
  free=${DISK_FREE_OVERRIDE:-$(df --output=avail -B1 "$OUT" | tail -1)}
  if (( free < need )); then log "not enough disk: need $need bytes, $free free under $OUT"; return 1; fi
}

run_cases() {
  wc_sizes "$SIZE"
  cases_disk_check
  local img
  for img in "$KVROCKS_IMAGE" "$MEMTIER_IMAGE" "$CLI_IMAGE" "$GO_IMAGE"; do docker pull -q "$img" >/dev/null; done
  # write_meta records W1-W7's W6 fields, which this mode does not set
  local W6_DEL_RATE=0 W6_DEL_CLIENTS=0
  META_EXTRA=("mode=cases" "#case_repeats=$CASE_REPEATS" "#case_rate=$CASE_RATE")
  write_meta
  run_case_diff
  local server
  for server in tideskv kvrocks; do
    run_cases_server "$server" || log "$server: run failed, continuing"
    remove_data  # one server's data at a time; disk.json already holds its sizes
  done
  local rc=0
  python3 "$HERE/report.py" "$OUT" || rc=$?
  log "report: $OUT/report.md"
  exit $rc
}

# Both servers at both levels: the diff (every case must pass), then every case's probe lines on each (no loads, no
# memtier).
run_cases_smoke() {
  wc_sizes "$SIZE"
  run_case_diff
  local bad
  # shellcheck disable=SC2086  # one argument per case
  bad=$(python3 -c 'import json, sys
d = json.load(open(sys.argv[1]))
print(" ".join("%s=%s" % (c, d.get(c)) for c in sys.argv[2:] if d.get(c) != "pass"))' "$OUT/cases-diff.json" $CASES)
  if [[ -n $bad ]]; then log "smoke: not every case passed the diff: $bad; see $OUT/cases-diff.log"; return 1; fi
  local server level c err
  for server in tideskv kvrocks; do
    for level in relaxed strict; do
      start_server "$server" "$level" "$OUT/data/$server"
      parity_check "$server" "$level"
      for c in $CASES; do
        if ! err=$(_case_probe "$c"); then log "probe $server $c $err"; return 1; fi
      done
      stop_server
    done
  done
  log "smoke ok: $OUT"
}
