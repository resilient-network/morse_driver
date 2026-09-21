#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: capture-throughput-case.sh --case LABEL --peer IP [options]

Capture one non-mutating HaLow throughput case. The script verifies the active
configuration, snapshots driver/radio/host evidence, runs iperf3, and packages
the result. It never reloads a module or changes a channel.

Options:
  --case LABEL             Required case label (letters, digits, dot, dash, underscore)
  --peer IP                Required iperf3 server address
  --interface NAME         HaLow interface (default: wlan1)
  --duration SECONDS       Per iperf direction (default: 20)
  --parallel STREAMS       Parallel TCP streams (default: 1)
  --expect-spi-hz HZ       Fail if active SPI clock differs
  --expect-op-bw-mhz MHZ   Fail if active S1G operating bandwidth differs
  --output DIR             Parent evidence directory (default: current directory)
EOF
}

case_label=
peer=
interface=wlan1
duration=20
parallel=1
expect_spi_hz=
expect_op_bw_mhz=
output_parent=$PWD

while [[ $# -gt 0 ]]; do
  case "$1" in
    --case) case_label=${2-}; shift 2 ;;
    --peer) peer=${2-}; shift 2 ;;
    --interface) interface=${2-}; shift 2 ;;
    --duration) duration=${2-}; shift 2 ;;
    --parallel) parallel=${2-}; shift 2 ;;
    --expect-spi-hz) expect_spi_hz=${2-}; shift 2 ;;
    --expect-op-bw-mhz) expect_op_bw_mhz=${2-}; shift 2 ;;
    --output) output_parent=${2-}; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ $case_label =~ ^[A-Za-z0-9._-]+$ ]] || {
  echo "--case must use only letters, digits, dot, dash, or underscore" >&2
  exit 2
}
[[ -n $peer ]] || { echo "--peer is required" >&2; exit 2; }
[[ $duration =~ ^[1-9][0-9]*$ ]] || { echo "invalid --duration" >&2; exit 2; }
[[ $parallel =~ ^[1-9][0-9]*$ ]] || { echo "invalid --parallel" >&2; exit 2; }

for command in iperf3 iw sha256sum tar timeout; do
  command -v "$command" >/dev/null || { echo "missing command: $command" >&2; exit 1; }
done

debugfs_file=$(find /sys/kernel/debug/ieee80211 -path '*/morse/resilient_performance' \
  -type f -print -quit 2>/dev/null || true)
[[ -n $debugfs_file ]] || {
  echo "resilient_performance is unavailable; r2 is not active or debugfs is not mounted" >&2
  exit 1
}

timestamp=$(date -u +%Y%m%dT%H%M%SZ)
case_dir=$output_parent/morse-throughput-$case_label-$timestamp
mkdir -p "$case_dir"
exec 9>"$output_parent/.morse-throughput.lock"
flock -n 9 || { echo "another throughput capture is running" >&2; exit 1; }

snapshot() {
  local phase=$1
  cp "$debugfs_file" "$case_dir/resilient-performance-$phase.txt"
  iw dev "$interface" link >"$case_dir/iw-link-$phase.txt" 2>&1 || true
  iw dev "$interface" station dump >"$case_dir/iw-station-$phase.txt" 2>&1 || true
  ip -details -statistics link show dev "$interface" \
    >"$case_dir/ip-link-$phase.txt" 2>&1 || true
  cat /sys/class/thermal/thermal_zone0/temp >"$case_dir/cpu-temp-millic-$phase.txt" 2>/dev/null || true
  if command -v vcgencmd >/dev/null; then
    vcgencmd get_throttled >"$case_dir/pi-throttled-$phase.txt" 2>&1 || true
  fi
}

active_spi_hz=$(awk -F= '$1 == "spi_clock_hz" { print $2; exit }' "$debugfs_file")
active_op_bw_mhz=$(awk -F= '$1 == "channel_operating_bw_mhz" { print $2; exit }' "$debugfs_file")
if [[ -n $expect_spi_hz && $active_spi_hz != "$expect_spi_hz" ]]; then
  echo "SPI clock mismatch: expected $expect_spi_hz, active $active_spi_hz" >&2
  exit 1
fi
if [[ -n $expect_op_bw_mhz && $active_op_bw_mhz != "$expect_op_bw_mhz" ]]; then
  echo "operating bandwidth mismatch: expected $expect_op_bw_mhz, active $active_op_bw_mhz" >&2
  exit 1
fi

cat >"$case_dir/case.env" <<EOF
schema=resilient-morse-throughput-case-v1
case=$case_label
captured_at=$timestamp
hostname=$(hostname)
kernel_release=$(uname -r)
module_version=$(modinfo -F version morse 2>/dev/null || true)
interface=$interface
peer=$peer
duration_seconds=$duration
parallel_streams=$parallel
active_spi_hz=$active_spi_hz
active_operating_bw_mhz=$active_op_bw_mhz
EOF

snapshot before
ping -c 10 -W 2 "$peer" >"$case_dir/ping.txt" 2>&1 || true
timeout "$((duration + 15))" iperf3 -c "$peer" -t "$duration" -P "$parallel" -J \
  >"$case_dir/iperf-forward.json"
timeout "$((duration + 15))" iperf3 -c "$peer" -t "$duration" -P "$parallel" -R -J \
  >"$case_dir/iperf-reverse.json"
snapshot after

(
  cd "$case_dir"
  sha256sum ./* >SHA256SUMS
)
archive=$case_dir.tar.gz
tar --sort=name --owner=0 --group=0 --numeric-owner -czf "$archive" \
  -C "$output_parent" "$(basename "$case_dir")"
sha256sum "$archive" >"$archive.sha256"
printf 'CASE_DIRECTORY=%s\nARCHIVE=%s\n' "$case_dir" "$archive"
