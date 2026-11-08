#!/usr/bin/env bash
# Local build and test on Linux: build.sh plus the Python tests, a boot check and the package.
#
#   ./build_linux.sh                  build out/deception and out/deception-setup
#   ./build_linux.sh --test           also run every C/C++ test and the Python tests
#   ./build_linux.sh --run [--game DIR] [--timeout N]
#                                     also start the game (DECEPTION_GAME_DIR, or DIR) for N seconds
#                                     (default 20) and print its first STOP:/Fault: line
#   ./build_linux.sh --package        also write the release tarballs (tools/package.py --zip)
#   ./build_linux.sh --clean          remove out/ first (a full rebuild)
#
# Options combine. Dependencies: docs/building.md (.github/scripts/ci-deps.sh on Ubuntu 24.04).
set -euo pipefail
cd -- "$(dirname -- "$0")"

test=0 run=0 package=0 clean=0 timeout=20 game=${DECEPTION_GAME_DIR:-}
while (($#)); do
    case $1 in
    --test) test=1 ;;
    --run) run=1 ;;
    --package) package=1 ;;
    --clean) clean=1 ;;
    --game) game=${2:?--game needs a folder}; shift ;;
    --timeout) timeout=${2:?--timeout needs seconds}; shift ;;
    -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option $1 (--help lists them)." >&2; exit 2 ;;
    esac
    shift
done
[[ $(uname -s) == Linux ]] || { echo 'This script is for Linux; on Windows use build_windows.ps1.' >&2; exit 1; }

# clang as named by the distribution when there is no plain clang (Ubuntu's clang-18).
if [[ -z ${CC:-} ]] && ! command -v clang >/dev/null; then
    for v in 20 19 18; do
        if command -v "clang-$v" >/dev/null; then export CC=clang-$v CXX=clang++-$v; break; fi
    done
fi
command -v python3 >/dev/null || { echo 'Need python3 (3.10 or newer).' >&2; exit 1; }

((clean)) && rm -rf out
if ((test)); then bash build.sh --test; else bash build.sh; fi
if ((test)); then
    (cd tests && python3 -m unittest discover -p 'test_*.py')
fi
if ((run)); then
    [[ -n $game ]] || { echo '--run needs the game dump: --game DIR or DECEPTION_GAME_DIR.' >&2; exit 1; }
    mkdir -p out
    status=0
    python3 run.py --game "$game" --timeout "$timeout" > out/boot.log 2>&1 || status=$?
    grep -m1 -E '^(STOP|Fault):' out/boot.log || echo 'No STOP: or Fault: line.'
    echo "Exit code $status; the full log is out/boot.log."
fi
if ((package)); then
    python3 tools/package.py --zip
fi
