#!/usr/bin/env bash
# Build-only tooling: installed AEC never needs Python or a compiler.
set -euo pipefail
cache=${HYPRCAPTURE_AEC_BUILD_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/hyprcapture/aec-build-2.14.0}
destination=${1:-${XDG_DATA_HOME:-$HOME/.local/share}/hyprcapture/aec/runtime}
mkdir -p "$cache" "$destination"
exec 9>"$cache/build.lock"
flock 9
fetch() {
    local url=$1 file=$2 digest=$3
    if ! printf '%s  %s\n' "$digest" "$file" | sha256sum --check --status 2>/dev/null; then
        curl --fail --location --retry 2 --connect-timeout 15 --max-time 600 "$url" -o "$file.part"
        printf '%s  %s\n' "$digest" "$file.part" | sha256sum --check --status
        mv "$file.part" "$file"
    fi
}
case $(uname -m) in
    x86_64) cmake_sha=5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf ;;
    aarch64) cmake_sha=b4cc788d63112b2749b40627e719eb5d3b8ed8f00c36d77189f4019cfe64bc9e ;;
    *) echo 'AEC runtime build supports Linux x86_64 and aarch64' >&2; exit 1 ;;
esac
cmake_name=cmake-3.31.6-linux-$(uname -m)
fetch "https://github.com/Kitware/CMake/releases/download/v3.31.6/$cmake_name.tar.gz" "$cache/cmake.tar.gz" "$cmake_sha"
fetch 'https://codeload.github.com/tensorflow/tensorflow/tar.gz/refs/tags/v2.14.0' "$cache/tensorflow.tar.gz" ce357fd0728f0d1b0831d1653f475591662ec5bca736a94ff789e6b1944df19f
fetch 'https://codeload.github.com/Maratyszcza/psimd/tar.gz/072586a71b55b7f8c584153d223e95687148a900' "$cache/psimd.tar.gz" f6c4dab91ae9a03b3019e7cab0572743afd0e1b6e75b97fcca50259c737c924e
[[ -d "$cache/$cmake_name" ]] || tar -xzf "$cache/cmake.tar.gz" -C "$cache"
[[ -d "$cache/tensorflow-2.14.0" ]] || tar -xzf "$cache/tensorflow.tar.gz" -C "$cache"
[[ -d "$cache/psimd-072586a71b55b7f8c584153d223e95687148a900" ]] || tar -xzf "$cache/psimd.tar.gz" -C "$cache"
export PATH="$cache/$cmake_name/bin:$PATH"
cmake -S "$cache/tensorflow-2.14.0/tensorflow/lite/c" -B "$cache/build" \
    -DCMAKE_BUILD_TYPE=Release -DTFLITE_ENABLE_XNNPACK=ON -DTFLITE_ENABLE_GPU=OFF \
    -DTFLITE_ENABLE_NNAPI=OFF -DTFLITE_ENABLE_INSTALL=OFF \
    -DPSIMD_SOURCE_DIR="$cache/psimd-072586a71b55b7f8c584153d223e95687148a900"
cmake --build "$cache/build" --target tensorflowlite_c --parallel "${HYPRCAPTURE_AEC_BUILD_JOBS:-4}"
install -m755 "$cache/build/libtensorflowlite_c.so" "$destination/libtensorflowlite_c.so.part"
mv "$destination/libtensorflowlite_c.so.part" "$destination/libtensorflowlite_c.so"
install -m644 "$cache/tensorflow-2.14.0/LICENSE" "$destination/TensorFlow-LICENSE.txt"
# Retain dependency licenses alongside the built runtime.
mkdir -p "$destination/licenses"
while IFS= read -r -d '' file; do
    name=${file#"$cache/build/"}; name=${name//\//_}
    install -m644 "$file" "$destination/licenses/$name"
done < <(find "$cache/build" -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' \) -print0)
