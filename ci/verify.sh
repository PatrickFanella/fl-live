#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
results="$PWD/build/ci-results"
mkdir -p "$results"
collect() {
    for preset in desktop release engine-only pipewire-sanitize; do
        if [[ -f "build/$preset/Testing/Temporary/LastTest.log" ]]; then
            cp "build/$preset/Testing/Temporary/LastTest.log" "$results/$preset-LastTest.log"
        fi
    done
    if [[ -f build/desktop/desktop-session.png ]]; then
        cp build/desktop/desktop-session.png "$results/desktop-session.png"
    fi
}
trap collect EXIT
{
    printf 'Source commit: %s\n' "${CI_SOURCE_SHA:-local-uncommitted}"
    cmake --version
    c++ --version
    pkg-config --modversion Qt6Quick libpipewire-0.3 sndfile
    python3 --version
    if command -v pacman >/dev/null; then pacman -Q; fi
} > "$results/toolchain.txt"
for preset in desktop release engine-only pipewire-sanitize; do
    cmake --preset "$preset" 2>&1 | tee "$results/$preset-configure.log"
    cmake --build --preset "$preset" 2>&1 | tee "$results/$preset-build.log"
    ctest --preset "$preset" --output-junit "$results/$preset-junit.xml" 2>&1 | tee "$results/$preset-test.log"
done
