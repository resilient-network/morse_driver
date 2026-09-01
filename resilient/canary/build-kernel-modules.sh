#!/usr/bin/env bash
set -euo pipefail

usage() {
  printf 'Usage: %s DRIVER_SOURCE OUTPUT_DIR\n' "$0" >&2
}

[[ $# -eq 2 ]] || { usage; exit 2; }
driver_source="$1"
output_dir="$2"

: "${DRIVER_SOURCE_COMMIT:?missing DRIVER_SOURCE_COMMIT}"
: "${KERNEL_SOURCE_COMMIT:?missing KERNEL_SOURCE_COMMIT}"
: "${KERNEL_RELEASE:?missing KERNEL_RELEASE}"

[[ "$(uname -m)" = aarch64 ]] || {
  echo "Canary module build requires native arm64" >&2
  exit 1
}

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  bc bison build-essential ca-certificates flex git kmod libelf-dev libssl-dev \
  python3 rsync xz-utils

[[ -d "$driver_source/.git" || -f "$driver_source/.git" ]] || {
  echo "Driver source is not a Git worktree" >&2
  exit 1
}
[[ "$(git -C "$driver_source" rev-parse HEAD)" = "$DRIVER_SOURCE_COMMIT" ]] || {
  echo "Driver source commit mismatch" >&2
  exit 1
}
[[ -z "$(git -C "$driver_source" status --short)" ]] || {
  echo "Driver source worktree is dirty" >&2
  exit 1
}
git -C "$driver_source" submodule status --recursive | awk '$1 ~ /^[-+U]/ {bad=1} END {exit bad ? 0 : 1}' && {
  echo "Driver submodule state is incomplete or divergent" >&2
  exit 1
}

kernel_dir=/build/rpi-linux
stage=/build/module-stage
package_root=/build/package-root
rm -rf "$kernel_dir" "$stage" "$package_root" "$output_dir"
mkdir -p "$kernel_dir" "$stage" "$package_root/modules" "$output_dir"

git -C "$kernel_dir" init
git -C "$kernel_dir" remote add origin https://github.com/MorseMicro/rpi-linux.git
git -C "$kernel_dir" fetch --depth 1 origin "$KERNEL_SOURCE_COMMIT"
git -C "$kernel_dir" checkout --detach FETCH_HEAD
[[ "$(git -C "$kernel_dir" rev-parse HEAD)" = "$KERNEL_SOURCE_COMMIT" ]] || {
  echo "Kernel source commit mismatch" >&2
  exit 1
}

make -C "$kernel_dir" ARCH=arm64 bcm2711_defconfig
make -C "$kernel_dir" ARCH=arm64 olddefconfig
make -C "$kernel_dir" -j"$(nproc)" ARCH=arm64 Image.gz modules

actual_kernel_release="$(make -s -C "$kernel_dir" ARCH=arm64 kernelrelease)"
[[ "$actual_kernel_release" = "$KERNEL_RELEASE" ]] || {
  echo "Kernel release mismatch: expected $KERNEL_RELEASE, got $actual_kernel_release" >&2
  exit 1
}
[[ -s "$kernel_dir/Module.symvers" ]] || {
  echo "Kernel build did not produce Module.symvers" >&2
  exit 1
}

make -C "$driver_source" clean
make -C "$driver_source" -j"$(nproc)" \
  ARCH=arm64 CROSS_COMPILE="" KERNEL_SRC="$kernel_dir" \
  CONFIG_WLAN_VENDOR_MORSE=m CONFIG_MORSE_SPI=y CONFIG_MORSE_USER_ACCESS=y \
  CONFIG_MORSE_VENDOR_COMMAND=y CONFIG_MORSE_DEBUGFS=y \
  CONFIG_MORSE_COUNTRY=US DEBUG=n
make -C "$driver_source" \
  ARCH=arm64 CROSS_COMPILE="" KERNEL_SRC="$kernel_dir" \
  CONFIG_WLAN_VENDOR_MORSE=m CONFIG_MORSE_SPI=y CONFIG_MORSE_USER_ACCESS=y \
  CONFIG_MORSE_VENDOR_COMMAND=y CONFIG_MORSE_DEBUGFS=y \
  CONFIG_MORSE_COUNTRY=US DEBUG=n \
  INSTALL_MOD_PATH="$stage" INSTALL_MOD_DIR=updates modules_install

morse_module="$(find "$stage/lib/modules/$KERNEL_RELEASE" -type f -name morse.ko -print -quit)"
dot11ah_module="$(find "$stage/lib/modules/$KERNEL_RELEASE" -type f -name dot11ah.ko -print -quit)"
[[ -s "$morse_module" && -s "$dot11ah_module" ]] || {
  echo "Module installation was incomplete" >&2
  exit 1
}

expected_version=0-rel_mm6108_2_0_1_resilient_r1_2026_Sep_01
[[ "$(modinfo -F version "$morse_module")" = "$expected_version" ]] || {
  echo "Morse module version mismatch" >&2
  exit 1
}
[[ "$(modinfo -F version "$dot11ah_module")" = "$expected_version" ]] || {
  echo "dot11ah module version mismatch" >&2
  exit 1
}
for module in "$morse_module" "$dot11ah_module"; do
  modinfo -F vermagic "$module" | grep -q "^$KERNEL_RELEASE " || {
    echo "Module vermagic mismatch: $module" >&2
    exit 1
  }
done
modinfo -p "$morse_module" | grep -q '^spi_post_write_status_bytes:' || {
  echo "Morse module lacks the required SPI timing parameter" >&2
  exit 1
}

install -m 0644 "$morse_module" "$package_root/modules/morse.ko"
install -m 0644 "$dot11ah_module" "$package_root/modules/dot11ah.ko"
modinfo "$package_root/modules/morse.ko" >"$package_root/morse.modinfo.txt"
modinfo "$package_root/modules/dot11ah.ko" >"$package_root/dot11ah.modinfo.txt"
xz -9e "$package_root/modules/morse.ko"
xz -9e "$package_root/modules/dot11ah.ko"

python3 - "$package_root" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from datetime import datetime, timezone

root = Path(sys.argv[1])
driver = Path("/build/driver-source")
kernel = Path("/build/rpi-linux")

def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

manifest = {
    "schema": "resilient-morse-canary-modules-v1",
    "canary_only": True,
    "architecture": "arm64",
    "kernel_release": os.environ["KERNEL_RELEASE"],
    "kernel_source_commit": os.environ["KERNEL_SOURCE_COMMIT"],
    "kernel_config_sha256": sha256(kernel / ".config"),
    "driver_source_commit": os.environ["DRIVER_SOURCE_COMMIT"],
    "driver_version": "0-rel_mm6108_2_0_1_resilient_r1_2026_Sep_01",
    "built_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
    "compiler": subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0],
    "files": {
        "modules/morse.ko.xz": sha256(root / "modules/morse.ko.xz"),
        "modules/dot11ah.ko.xz": sha256(root / "modules/dot11ah.ko.xz"),
        "morse.modinfo.txt": sha256(root / "morse.modinfo.txt"),
        "dot11ah.modinfo.txt": sha256(root / "dot11ah.modinfo.txt"),
    },
}
(root / "manifest.json").write_text(
    json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
)
PY

package="$output_dir/morse-mm6108-resilient-r1-6.12.21-v8plus-arm64.tar.gz"
tar -C "$package_root" --sort=name --mtime='UTC 2026-09-01' \
  --owner=0 --group=0 --numeric-owner -czf "$package" .
sha256sum "$package" >"$package.sha256"
chmod -R a+rX "$output_dir"
echo "Canary module package ready: $package"
