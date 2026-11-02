#!/usr/bin/env bash
set -euo pipefail
src="${1:?source checkout required}"
gen="${2:?generation directory required}"
repo="${3:?repository root required}"
jobs="${4:-4}"
fluid="$src/fluidsynth"
[[ -f "$fluid/CMakeLists.txt" ]] || { echo "pinned FluidSynth source missing" >&2; exit 1; }
# FluidSynth pins gcem as a nested submodule. Fetch its immutable upstream
# commit explicitly because the store builder tracks only top-level submodules.
if [[ ! -f "$fluid/gcem/include/gcem.hpp" ]]; then
  rm -rf "$fluid/gcem"
  git init -q "$fluid/gcem"
  git -C "$fluid/gcem" remote add origin https://github.com/kthohr/gcem.git
  git -C "$fluid/gcem" fetch -q --depth 1 origin 012ae73c6d0a2cb09ffe86475f5c6fba3926e200
  git -C "$fluid/gcem" checkout -q FETCH_HEAD
fi
export CROSS_COMPILE="${CROSS_COMPILE:-riscv32-esp-elf-}"
export FLUIDSYNTH_PAX_ABI=ilp32f
cmake -S "$fluid" -B "$gen/fluidsynth-build" \
  -DCMAKE_TOOLCHAIN_FILE="$repo/apps/stratagus/fluidsynth-pax-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_SHARED_LIBS=OFF -Dosal=embedded \
  -DCMAKE_C_FLAGS_MINSIZEREL='-O3 -fno-math-errno -DNDEBUG' \
  -DCMAKE_CXX_FLAGS_MINSIZEREL='-O3 -fno-math-errno -DNDEBUG' \
  -Denable-floats=ON -Denable-threads=OFF -Denable-openmp=OFF \
  -Denable-alsa=OFF -Denable-aufile=OFF -Denable-dbus=OFF \
  -Denable-jack=OFF -Denable-ladspa=OFF -Denable-libinstpatch=OFF \
  -Denable-libsndfile=OFF -Denable-midishare=OFF -Denable-network=OFF \
  -Denable-oss=OFF -Denable-dsound=OFF -Denable-wasapi=OFF \
  -Denable-waveout=OFF -Denable-winmidi=OFF -Denable-sdl3=OFF \
  -Denable-pulseaudio=OFF -Denable-pipewire=OFF -Denable-readline=OFF \
  -Denable-native-dls=OFF -Denable-systemd=OFF -Denable-ipv6=OFF \
  -Denable-portaudio=OFF -Denable-opensles=OFF -Denable-oboe=OFF
cmake --build "$gen/fluidsynth-build" --target libfluidsynth -j "$jobs"
test -s "$gen/fluidsynth-build/src/libfluidsynth.a"
