#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 4 ]]; then
    echo 'Usage: package-aec-candidate.sh BUILD_DIR OUTPUT_DIR RUNTIME_DIR MODEL_DIR' >&2
    exit 2
fi
build=$(realpath "$1")
output=$(realpath -m "$2")
runtime=$(realpath "$3")
models=$(realpath "$4")
source_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
[[ ! -e "$output" ]] || { echo 'Candidate output already exists' >&2; exit 1; }
(cd "$models" && sha256sum --check "$source_root/resources/aec/models.sha256")
mkdir -p "$output"
cmake --install "$build" --prefix "$output"
mkdir -p "$output/share/hyprcapture/aec/models" "$output/share/licenses/hyprcapture/runtime"
while read -r digest name; do
    install -m644 "$models/$name" "$output/share/hyprcapture/aec/models/$name"
done < "$source_root/resources/aec/models.sha256"
install -m755 "$runtime/libtensorflowlite_c.so" "$output/lib/hyprcapture/libtensorflowlite_c.so"
cp -r "$runtime/licenses" "$output/share/licenses/hyprcapture/runtime/"
install -m644 "$runtime/TensorFlow-LICENSE.txt" "$output/share/licenses/hyprcapture/runtime/"
install -m644 "$source_root/LICENSE" "$output/share/licenses/hyprcapture/"
install -m644 "$source_root/resources/aec/README.md" "$output/AEC-ASSETS.md"
install -m644 "$source_root/resources/aec/ACCEPTANCE.md" "$output/ACCEPTANCE.md"
# No cache or user preferences in a distributable artifact. Check on target host.
printf '%s\n' 'DTLN-AEC release candidate; not a portable ABI-independent build.' \
    'Run bin/hyprcapture-aec --check on the target machine before testing.' \
    'The optional NPU module requires a compatible system OpenVINO installation.' \
    'Do not load the compositor plugin into a different Hyprland ABI.' > "$output/CANDIDATE.txt"
(cd "$output" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
