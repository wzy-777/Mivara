# Performance

> [Docs index](../README.md) · [Back to README](../../README_EN.md) · [中文](../zh/performance.md)

This page collects Mivara's bandwidth and latency figures, plus the receiver's statistics output. For the streaming and codec path see [Architecture & Protocol](architecture.md); for ports and link requirements see [Network & Firewall](network.md).

## Bandwidth (uncompressed RGB24)

| Resolution | 30 FPS | 60 FPS |
|------------|--------|--------|
| 1280×720 | ~125 MB/s | ~250 MB/s |
| 1920×1080 | ~280 MB/s | ~560 MB/s |
| 2560×1440 | ~500 MB/s | ~1 GB/s |

> RLE compression typically cuts 30–70% of bandwidth on desktop content (text/UI); video content compresses less.

For where RLE and the codecs sit in the streaming path, see [Architecture & Protocol](architecture.md).

## Latency

| Link | Typical |
|------|---------|
| Gigabit wired | < 5 ms |
| 5 GHz WiFi | 10–15 ms |
| 2.4 GHz WiFi | 20–35 ms |

For the link's bandwidth and firewall requirements, see [Network & Firewall](network.md).

## Statistics Output

The receiver writes statistics to `gpu_stats.json` every 60 seconds and prints a console summary every 30 seconds.
