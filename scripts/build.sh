#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/clang-libcxx}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
CXX_COMPILER="${CXX:-clang++}"
CXX_FLAGS="${CXXFLAGS:-}"
FFMPEG_DOWNLOAD_DIR="$ROOT/build/downloads"
FFMPEG_ARCHIVE="$FFMPEG_DOWNLOAD_DIR/ffmpeg-6.1.6.tar.xz"
FFMPEG_ARCHIVE_PART="$FFMPEG_ARCHIVE.part"
FFMPEG_SOURCE_URL="https://ffmpeg.org/releases/ffmpeg-6.1.6.tar.xz"
EXPECTED_FFMPEG_ARCHIVE_SHA256="d4fcb164028dd3beee5d92c0ac72e46aac6973c75ea12dc14de07bf8f407370a"
FFMPEG_SOURCE_DIR="$ROOT/build/ffmpeg-6.1.6-src-${EXPECTED_FFMPEG_ARCHIVE_SHA256:0:8}"
FFMPEG_PREFIX="${FFMPEG_PREFIX:-$ROOT/build/private-ffmpeg}"
FFMPEG_CONFIG_MARKER="$FFMPEG_PREFIX/.davinci-ffmpeg-6.1.6-qsv"

for command_name in cmake pkg-config tar sha256sum curl patchelf; do
    command -v "$command_name" >/dev/null 2>&1 || {
        printf 'Error: %s is required.\n' "$command_name" >&2
        exit 1
    }
done
command -v "$CXX_COMPILER" >/dev/null 2>&1 || {
    printf 'Error: C++ compiler not found: %s\n' "$CXX_COMPILER" >&2
    exit 1
}
mkdir -p "$FFMPEG_DOWNLOAD_DIR"
if [[ ! -r "$FFMPEG_ARCHIVE" ]]; then
    printf 'Downloading pinned FFmpeg source: %s\n' "$FFMPEG_SOURCE_URL"
    curl --fail --location --retry 3 --retry-delay 2 \
        --output "$FFMPEG_ARCHIVE_PART" "$FFMPEG_SOURCE_URL"
    downloaded_archive_sha256="$(sha256sum "$FFMPEG_ARCHIVE_PART" | awk '{print $1}')"
    [[ "$downloaded_archive_sha256" == "$EXPECTED_FFMPEG_ARCHIVE_SHA256" ]] || {
        printf 'Error: downloaded FFmpeg source SHA-256 mismatch: %s\n' "$downloaded_archive_sha256" >&2
        exit 1
    }
    mv -- "$FFMPEG_ARCHIVE_PART" "$FFMPEG_ARCHIVE"
fi
actual_archive_sha256="$(sha256sum "$FFMPEG_ARCHIVE" | awk '{print $1}')"
[[ "$actual_archive_sha256" == "$EXPECTED_FFMPEG_ARCHIVE_SHA256" ]] || {
    printf 'Error: FFmpeg source archive SHA-256 mismatch: %s\n' "$actual_archive_sha256" >&2
    exit 1
}
pkg-config --exists dav1d || {
    printf '%s\n' 'Error: libdav1d development files are required (pkg-config module: dav1d).' >&2
    exit 1
}
pkg-config --exists libva || {
    printf '%s\n' 'Error: libva development files are required for the Linux QSV device path.' >&2
    exit 1
}
pkg-config --exists "vpl >= 2.6" || {
    printf '%s\n' 'Error: Intel oneVPL development files >= 2.6 are required (pkg-config module: vpl).' >&2
    exit 1
}

DAV1D_VERSION="$(pkg-config --modversion dav1d)"
VPL_VERSION="$(pkg-config --modversion vpl)"

if [[ ! -f "$FFMPEG_SOURCE_DIR/configure" ]]; then
    mkdir -p "$FFMPEG_SOURCE_DIR"
    tar -xJf "$FFMPEG_ARCHIVE" -C "$FFMPEG_SOURCE_DIR" --strip-components=1
fi
FFMPEG_SOURCE_VERSION="$(tr -d '\r\n' < "$FFMPEG_SOURCE_DIR/VERSION")"
[[ "$FFMPEG_SOURCE_VERSION" == "6.1.6" ]] || {
    printf 'Error: expected FFmpeg source 6.1.6, found: %s\n' "$FFMPEG_SOURCE_VERSION" >&2
    exit 1
}

if [[ ! -r "$FFMPEG_PREFIX/include/libavcodec/avcodec.h" ||
      ! -r "$FFMPEG_PREFIX/lib/libavcodec.so.60" ||
      ! -r "$FFMPEG_PREFIX/lib/libavutil.so.58" ||
      ! -r "$FFMPEG_CONFIG_MARKER" ]]; then
    mkdir -p "$FFMPEG_PREFIX"
    (
        cd "$FFMPEG_SOURCE_DIR"
        ./configure \
            --prefix="$FFMPEG_PREFIX" \
            --disable-everything \
            --disable-avformat --disable-avdevice --disable-avfilter \
            --disable-swscale --disable-swresample --disable-postproc \
            --enable-avcodec --enable-avutil \
            --enable-decoder=libdav1d --enable-decoder=av1_qsv \
            --enable-parser=av1 --enable-libdav1d --enable-libvpl \
            --enable-vaapi \
            --disable-programs --disable-doc --disable-network --disable-autodetect \
            --disable-static --enable-shared --disable-debug \
            --disable-gpl --disable-version3 --disable-nonfree \
            --enable-pic --disable-x86asm
        make --jobs "${FFMPEG_BUILD_JOBS:-$(nproc)}"
        make install
    )
    printf '%s\n' 'FFmpeg 6.1.6: libdav1d + oneVPL QSV AV1 decode; shared libavcodec/libavutil' \
        > "$FFMPEG_CONFIG_MARKER"
fi

# Give the private FFmpeg DSOs process-unique SONAMEs. QSV's VA-API/oneVPL
# plugin loader must run in the base linker namespace; dlmopen isolation is
# incompatible with the driver's plugin loading. Keep the on-disk filenames
# stable for the provider, but make the ELF dependency identity unambiguous.
PRIVATE_AVUTIL="$FFMPEG_PREFIX/lib/libavutil.so.58"
PRIVATE_AVCODEC="$FFMPEG_PREFIX/lib/libavcodec.so.60"
if [[ "$(patchelf --print-soname "$PRIVATE_AVUTIL")" != "libdavinci_avutil.so.58" ]]; then
    patchelf --set-soname libdavinci_avutil.so.58 "$PRIVATE_AVUTIL"
fi
if [[ "$(patchelf --print-soname "$PRIVATE_AVCODEC")" != "libdavinci_avcodec.so.60" ]]; then
    patchelf --set-soname libdavinci_avcodec.so.60 "$PRIVATE_AVCODEC"
fi
if patchelf --print-needed "$PRIVATE_AVCODEC" | grep -Fxq 'libavutil.so.58'; then
    patchelf --replace-needed libavutil.so.58 libdavinci_avutil.so.58 "$PRIVATE_AVCODEC"
fi
patchelf --set-rpath '$ORIGIN' "$PRIVATE_AVCODEC"
ln -sfn libavutil.so.58 "$FFMPEG_PREFIX/lib/libdavinci_avutil.so.58"
ln -sfn libavcodec.so.60 "$FFMPEG_PREFIX/lib/libdavinci_avcodec.so.60"

case "$CXX_COMPILER" in
    *clang*)
        CXX_FLAGS="${CXX_FLAGS:+$CXX_FLAGS }-stdlib=libc++"
        ;;
    *)
        printf '%s\n' 'Error: Clang++ with libc++ is required for Resolve exception-ABI compatibility.' >&2
        exit 1
        ;;
esac

cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_CXX_COMPILER="$CXX_COMPILER" \
    -DCMAKE_CXX_FLAGS="$CXX_FLAGS" \
    -DDAVINCI_FFMPEG_616_ROOT="$FFMPEG_PREFIX"

# Build the independently loadable components first. Their exact hashes are
# then embedded into the session controller generated for this local build.
cmake --build "$BUILD_DIR" --target davinci_av1_bootstrap davinci_codec_runtime \
    --parallel "${BUILD_JOBS:-$(nproc)}"

BOOTSTRAP_SHA256="$(sha256sum "$BUILD_DIR/libdavinci_av1_bootstrap.so.0.1.0" | awk '{print $1}')"
RUNTIME_SHA256="$(sha256sum "$BUILD_DIR/libdavinci_codec_runtime.so.0.1.0" | awk '{print $1}')"
AVUTIL_SHA256="$(sha256sum "$FFMPEG_PREFIX/lib/libavutil.so.58" | awk '{print $1}')"
AVCODEC_SHA256="$(sha256sum "$FFMPEG_PREFIX/lib/libavcodec.so.60" | awk '{print $1}')"

cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_CXX_COMPILER="$CXX_COMPILER" \
    -DCMAKE_CXX_FLAGS="$CXX_FLAGS" \
    -DDAVINCI_FFMPEG_616_ROOT="$FFMPEG_PREFIX" \
    -DDAVINCI_EXPECTED_BOOTSTRAP_SHA256="$BOOTSTRAP_SHA256" \
    -DDAVINCI_EXPECTED_RUNTIME_SHA256="$RUNTIME_SHA256" \
    -DDAVINCI_EXPECTED_AVUTIL_SHA256="$AVUTIL_SHA256" \
    -DDAVINCI_EXPECTED_AVCODEC_SHA256="$AVCODEC_SHA256"

cmake --build "$BUILD_DIR" --parallel "${BUILD_JOBS:-$(nproc)}"
CONTROLLER_SHA256="$(sha256sum "$BUILD_DIR/libdavinci_av1_session_controller.so.0.1.0" | awk '{print $1}')"

cat > "$BUILD_DIR/artifact-manifest.txt" <<MANIFEST
controller_sha256=$CONTROLLER_SHA256
bootstrap_sha256=$BOOTSTRAP_SHA256
runtime_sha256=$RUNTIME_SHA256
avutil_sha256=$AVUTIL_SHA256
avcodec_sha256=$AVCODEC_SHA256
dav1d_version=$DAV1D_VERSION
onevpl_version=$VPL_VERSION
MANIFEST

printf 'Build complete. Artifact manifest: %s\n' "$BUILD_DIR/artifact-manifest.txt"
