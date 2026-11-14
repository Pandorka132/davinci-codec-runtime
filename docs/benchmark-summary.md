# AV1 decoder benchmark summary

The local benchmark work compared libdav1d, libaom-av1, and available hardware decode paths on the development host. The recorded software comparison used 14 threads, one warm-up run, and three measured runs. Across the tested 1080p 8-bit, 1080p 10-bit, and 4K 10-bit streams, libdav1d reached approximately 3.1x the throughput of libaom-av1 in that environment.

AMD Radeon RX 6700 XT AV1 Profile 0 VA-API decode was verified for 8-bit and 10-bit Main-profile samples. In the host FFmpeg 9 comparison on an Intel Core Ultra 5 245K, GPU-surface throughput was below dav1d CPU decode; downloading frames to system memory reduced throughput further. Intel Quick Sync was not measured because no Intel display-class PCI device or Intel DRM render node was exposed by that boot.

These are host-specific measurements, not universal performance claims. Re-run the benchmark on the target machine and report the exact FFmpeg/libdav1d versions, sample properties, thread count, warm-up policy, and frame-transfer behavior when submitting performance changes.
