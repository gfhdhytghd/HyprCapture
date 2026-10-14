#!/bin/sh
# Run only after all builds have finished. This does not mutate desktop configuration.
set -eu
build=$(realpath "${1:?Usage: run-benchmarks.sh BUILD_DIR OUTPUT_DIR}")
out=${2:?Usage: run-benchmarks.sh BUILD_DIR OUTPUT_DIR}
if pgrep -x cc1plus >/dev/null || pgrep -x cc1 >/dev/null; then
    echo 'Compilers are running; rerun after they finish.' >&2
    exit 2
fi
mkdir -p "$out"
config=$(mktemp -d /tmp/hdr-bench-config-XXXXXX)
trap 'rm -rf "$config"' EXIT HUP INT TERM
python3 - "$out" <<'PY'
import json,os,pathlib,subprocess,sys,time
r=pathlib.Path(sys.argv[1])
def read(path):
    try:return pathlib.Path(path).read_text().strip()
    except OSError:return None
info=dict(time=time.time(),loadavg=os.getloadavg(),cpu_allowed=next(s for s in pathlib.Path('/proc/self/status').read_text().splitlines() if s.startswith('Cpus_allowed_list')),
          ac_online=read('/sys/class/power_supply/AC/online'),battery_status=read('/sys/class/power_supply/BAT0/status'),
          power_profile=subprocess.check_output(['powerprofilesctl','get'],text=True).strip(),
          compiler_count=0,protocol='Release; one cold + four warm trials; serial CPU/GPU; glFinish at GPU boundaries')
(r/'benchmark-load.json').write_text(json.dumps(info,indent=2)+'\n')
PY
QT_QPA_PLATFORM=offscreen XDG_CONFIG_HOME="$config" "$build/ui-benchmark" > "$out/ui.csv" 2> "$out/ui-stderr.log"
env -u DRI_PRIME "$build/gpu-benchmark" > "$out/gpu.csv" 2> "$out/gpu-stderr.log"
python3 "$(dirname "$0")/summarize-bench.py" "$out" > "$out/summary-stdout.log"
