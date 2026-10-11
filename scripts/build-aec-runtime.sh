#!/usr/bin/env bash
# Build-only tooling: installed AEC never needs Python or a compiler.
set -euo pipefail
scriptdir=$(cd -- "$(dirname -- "$0")" && pwd)
manifest="$scriptdir/../nix/aec-sources.json"
preparer="$scriptdir/prepare-aec-sources.cmake"
if [[ ! -f $manifest ]]; then
    manifest="$scriptdir/hyprcapture-aec-sources.json"
    preparer="$scriptdir/hyprcapture-prepare-aec-sources.cmake"
fi
cache=${HYPRCAPTURE_AEC_BUILD_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/hyprcapture/aec-build-2.14.0}
destination=${1:-${XDG_DATA_HOME:-$HOME/.local/share}/hyprcapture/aec/runtime}
mkdir -p "$cache" "$destination"
exec 9>"$cache/build.lock"
flock 9
fetch() {
    local url=$1 file=$2 digest=$3
    if ! printf '%s  %s\n' "$digest" "$file" | sha256sum --check --status 2>/dev/null; then
        if [[ ${HYPRCAPTURE_AEC_OFFLINE:-0} == 1 ]]; then
            echo "Missing checksum-verified archive: $file" >&2; return 1
        fi
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
[[ -d "$cache/$cmake_name" ]] || tar -xzf "$cache/cmake.tar.gz" -C "$cache"
export PATH="$cache/$cmake_name/bin:$PATH"
cmake -DCACHE="$cache" -DMANIFEST="$manifest" -DOFFLINE="${HYPRCAPTURE_AEC_OFFLINE:-0}" -P "$preparer"
tensorflow="$cache/pinned-tensorflow"
if [[ ! -f $tensorflow/.verified ]]; then
    rm -rf "$tensorflow.part" "$tensorflow"
    mkdir -p "$tensorflow.part"
    tar -xzf "$cache/tensorflow.tar.gz" --strip-components=1 -C "$tensorflow.part"
    mv "$tensorflow.part" "$tensorflow"
    touch "$tensorflow/.verified"
fi
# Upstream's license fallback can fetch a mutable website without a checksum.
# All required licenses are in the verified archives; fail if one is absent.
license_module="$tensorflow/tensorflow/lite/tools/cmake/modules/OverridableFetchContent.cmake"
sed -i 's@file(DOWNLOAD "${LICENSE_URL}" "${LICENSE_FILE_DOWNLOAD}" STATUS RESULT)@message(FATAL_ERROR "Missing bundled license for ${CONTENT_NAME}; network fallback disabled")@' "$license_module"
# Use a new build directory so legacy unverified download stamps cannot survive.
cmake -S "$tensorflow/tensorflow/lite/c" -B "$cache/build-pinned" -G Ninja \
    -C "$cache/pinned-sources.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DTFLITE_ENABLE_XNNPACK=ON -DTFLITE_ENABLE_GPU=OFF \
    -DTFLITE_ENABLE_NNAPI=OFF -DTFLITE_ENABLE_INSTALL=OFF \
    -DBUILD_TESTING=OFF -DABSL_BUILD_TESTING=OFF -DFLATBUFFERS_BUILD_TESTS=OFF \
    -DFLATBUFFERS_BUILD_BENCHMARKS=OFF -DRUY_BUILD_TESTS=OFF \
    -DCPUINFO_BUILD_UNIT_TESTS=OFF -DCPUINFO_BUILD_MOCK_TESTS=OFF \
    -DCPUINFO_BUILD_BENCHMARKS=OFF \
    -DFP16_BUILD_TESTS=OFF -DFP16_BUILD_BENCHMARKS=OFF \
    -DFXDIV_BUILD_TESTS=OFF -DFXDIV_BUILD_BENCHMARKS=OFF \
    -DPTHREADPOOL_BUILD_TESTS=OFF -DPTHREADPOOL_BUILD_BENCHMARKS=OFF
[[ ${HYPRCAPTURE_AEC_CONFIGURE_ONLY:-0} != 1 ]] || exit 0
cmake --build "$cache/build-pinned" --target tensorflowlite_c --parallel "${HYPRCAPTURE_AEC_BUILD_JOBS:-4}"
install -m755 "$cache/build-pinned/libtensorflowlite_c.so" "$destination/libtensorflowlite_c.so.part"
mv "$destination/libtensorflowlite_c.so.part" "$destination/libtensorflowlite_c.so"
install -m644 "$tensorflow/LICENSE" "$destination/TensorFlow-LICENSE.txt"
# Retain dependency licenses alongside the built runtime.
mkdir -p "$destination/licenses"
while IFS= read -r -d '' file; do
    name=${file#"$cache/"}; name=${name//\//_}
    install -m644 "$file" "$destination/licenses/$name"
done < <(find "$cache/build-pinned" "$cache/pinned-sources" -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' \) -print0)
