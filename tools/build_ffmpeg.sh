#!/usr/bin/env bash
# Builds the minimal static FFmpeg the runner links (build.sh runs it when PREFIX is missing or was
# configured differently): only what Deception uses. Its movies (video/title.mp4, menu.mp4) are MP4
# with H.264 video; AvPlayer also accepts AAC audio; save icons and the start-up picture are PNG.
# AvPlayer opens the movie's host path (the file protocol) or reads through the game's own I/O
# callbacks; no network protocol is built. The release then carries no FFmpeg DLLs or their codec
# libraries.
#
# usage: tools/build_ffmpeg.sh PREFIX     (CC names the compiler; nasm on PATH for the x86 assembly)
set -euo pipefail
prefix=${1:?usage: tools/build_ffmpeg.sh PREFIX}
version=9.0.2
# The release tarball, checked against FFmpeg's release signing key (FCF986EA15E6E293A5644F10B4322F04D67658D8).
sha256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
options=(
    --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-network
    --disable-avdevice --disable-avfilter --disable-debug
    --enable-static --disable-shared --enable-zlib
    --enable-decoder=h264,aac,png --enable-parser=h264,aac,png --enable-demuxer=mov --enable-protocol=file
    --enable-swscale --enable-swresample
)
stamp="$version ${options[*]} ${CC:-clang}"
if [[ -f $prefix/.stamp && $(<"$prefix/.stamp") == "$stamp" ]]; then exit 0; fi

command -v nasm >/dev/null || echo 'FFmpeg: no nasm, building without the x86 assembly (slower decoding).' >&2
asm=(); command -v nasm >/dev/null || asm=(--disable-x86asm)
mkdir -p out/downloads
archive=out/downloads/ffmpeg-$version.tar.xz
if [[ ! -f $archive ]]; then
    # ffmpeg.org drops connections now and then.
    curl -fsSL --retry 5 --retry-all-errors --retry-delay 10 -o "$archive.part" "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz"
    mv "$archive.part" "$archive"
fi
if ! echo "$sha256  $archive" | sha256sum -c --status; then
    rm -f "$archive"
    echo "FFmpeg: $archive does not match its SHA-256; removed, run the build again." >&2; exit 1
fi
work=out/ffmpeg-build
rm -rf "$work" "$prefix"
mkdir -p "$work"
tar -xJf "$archive" -C "$work" --strip-components=1
prefix_abs=$(mkdir -p "$prefix" && cd "$prefix" && pwd)
# Windows: D:/... (the native pkgconf and clang read the .pc files, not MSYS's /d/...).
command -v cygpath >/dev/null && prefix_abs=$(cygpath -m "$prefix_abs")
echo "FFmpeg $version: configuring the minimal build (log: $work/build.log)"
(
    cd "$work"
    ./configure --prefix="$prefix_abs" --cc="${CC:-clang}" "${options[@]}" "${asm[@]}"
    make -j"$(nproc 2>/dev/null || echo 4)"
    make install
) > "$work/build.log" 2>&1 || { tail -30 "$work/build.log" >&2; echo "FFmpeg build failed (log: $work/build.log)" >&2; exit 1; }
echo "$stamp" > "$prefix/.stamp"
rm -rf "$work"
echo "FFmpeg $version: built into $prefix"
