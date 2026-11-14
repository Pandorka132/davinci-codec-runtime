# Third-party source and runtime dependencies

## FFmpeg 6.1.6

`scripts/build.sh` downloads the pinned upstream source archive from
`https://ffmpeg.org/releases/ffmpeg-6.1.6.tar.xz` into the ignored
`build/downloads/` directory. Before extracting or compiling it, the script
verifies SHA-256
`d4fcb164028dd3beee5d92c0ac72e46aac6973c75ea12dc14de07bf8f407370a` and the
upstream `VERSION` file (`6.1.6`). The archive is not a repository dependency
and is not committed.

The build configures only `libavcodec`, `libavutil`, the AV1 parser, and the
libdav1d decoder. GPL and nonfree components are explicitly disabled; FFmpeg
reports the resulting shared-library build license as LGPL version 2.1 or
later. Keep the upstream license notices with the source archive and retain
applicable notices when redistributing built artifacts.

The generated private shared libraries are build outputs and are not
committed. Their hashes are recorded in
`build/clang-libcxx/artifact-manifest.txt` and embedded into the locally built
session controller.

## libdav1d

The private FFmpeg build links against the system libdav1d development package.
The build requires the `dav1d` pkg-config module. The validated local build used
libdav1d 1.5.4 and runtime SONAME `libdav1d.so.7`; the build manifest records
the version selected by `pkg-config`. Ensure the matching runtime SONAME from
that build is available when Resolve starts. Install the distribution's
libdav1d development/runtime package before building. The libdav1d library
itself is not vendored here.

The runtime loads the private FFmpeg pair into a separate linker namespace. It
does not use Resolve's bundled FFmpeg for AV1 decode.
