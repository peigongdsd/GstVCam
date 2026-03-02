# GstVCam Performance Hot Paths and Mitigations

## Context
This document explains the current CPU/GPU hot paths in the `GstVCam` relay path:

`shm2src/appsink (GStreamer) -> GstPipelineSource -> MediaStream::RequestSample -> MF FrameServer`

The goal is to explain what is expensive, why it is expensive, and which changes are most likely to reduce load.

---

## 1) Most Likely GPU Hot Path: Writing Into Allocator-Provided 2D Buffers

### Where it happens
- `VCamSampleSource/MediaStream.cpp`
  - `IMFVideoSampleAllocatorEx::AllocateSample(...)`
  - `IMF2DBuffer2::Lock2DSize(...)`
  - CPU writes frame bytes into the returned buffer
  - `QueueEventParamUnk(MEMediaSample, ...)`

### Why it can be expensive
Even though our source frames are CPU-side NV12, the allocator provided by FrameServer may be GPU-backed in some runs/environments.

When CPU writes into a GPU-backed surface, the system can do one or more of:
- CPU->GPU upload/copy each frame
- synchronization fences between CPU and GPU timelines
- additional driver memory transitions

This can make GPU usage high even at 1080p30.

### Why this is the first mitigation target
This is the one hot path that directly explains "GPU load is much higher than plain d3d12videosink presentation".

Presentation-only pipelines are often optimized for GPU-local flow. Our current path can force cross-domain memory movement every delivered frame.

### Mitigation design
Prefer a CPU-backed sample path for this stream so writes stay in system memory.

Concrete options:
1. Keep using provided allocator only when it is known CPU-safe.
2. Otherwise use an internal CPU allocator/buffer strategy for sample backing.
3. Verify whether FrameServer accepts this path in all client scenarios.

### Validation
- Compare GPU utilization before/after with same stream settings.
- Keep resolution/fps fixed.
- Confirm no regression in latency and stability.

---

## 2) High CPU Path: Full NV12 Plane Copies Per Delivered Frame

### Where it happens
- `VCamSampleSource/GstPipelineSource.cpp`
  - `gst_video_frame_map(..., GST_MAP_READ)`
  - row-by-row `memcpy` for Y plane
  - row-by-row `memcpy` for UV plane

### Why it is expensive
Each delivered frame performs a full memory copy from appsink sample memory to MF sample memory.
At 1920x1080 NV12, this is roughly 3 MB/frame.
At 30 fps, this is about 90 MB/s raw copied payload, plus stride and mapping overhead.

This is normal for CPU relay, but still a major CPU bandwidth consumer.

### Mitigations
1. Fast contiguous copy path:
   - if source stride == destination stride and contiguous layout permits, use fewer/larger `memcpy` calls.
2. Keep only latest frame policy (already implemented) to avoid wasted copies for stale frames.
3. Explore zero-copy only if memory ownership/contracts allow it (usually hard across these APIs).

### Validation
- profile CPU usage in source process
- count actual delivered frames/sec
- verify output correctness for odd stride cases

---

## 3) Request Cadence Mismatch (FrameServer Pulls Faster Than Source FPS)

### Where it happens
- `VCamSampleSource/MediaStream.cpp`
  - `RequestSample(...)` can be called at a high rate by FrameServer/client pipeline behavior.

### Why it is expensive
If we always create/fill/queue a sample on every request, work scales with request rate, not source frame rate.

### Current status
Already mitigated in code:
- frame-id gating was added
- only deliver when a newer frame exists
- no-new-frame path returns early and yields

This was an important correctness and efficiency fix.

### Further mitigation
If request storms remain high in some clients:
- add tiny adaptive backoff on repeated no-new-frame misses
- example: after N consecutive misses, `Sleep(1)` instead of only `yield()`

This can further reduce scheduler thrash with minimal latency impact.

---

## 4) Lock Contention Between Pull Thread and Request Thread

### Where it happens
- `GstPipelineSource` latest-sample metadata guarded by `_frameLock`
- producer (`StoreSample`) and consumer (`HasNewFrameSince` / `CopyLatestFrameTo`) both contend

### Why it matters
At high request frequency, lock traffic increases. Usually not dominant, but measurable.

### Mitigations
1. Keep lock scope minimal (already mostly done).
2. Consider atomics for small metadata (`latestFrameId`, availability) where safe.
3. Keep sample ref/unref and pointer swaps predictable and short.

---

## 5) Logging Overhead (Lower Priority)

### Where it happens
- hot-path trace points in request/pipeline loops.

### Current status
- release builds now drop debug macro output via `WINTRACE -> __noop`.

This is already a good mitigation. Remaining cost in release should be low.

---

## Recommended Mitigation Order

1. **Allocator strategy (CPU-backed guarantee or fallback)**
   - highest chance to reduce GPU utilization significantly.
2. **Keep and tune no-new-frame handling**
   - optional adaptive backoff (`Sleep(1)` after repeated misses).
3. **Optimize copy path for contiguous stride case**
   - moderate CPU win.
4. **Lock-path micro-optimizations**
   - smaller incremental gains.

---

## Suggested Measurement Protocol

Use the same test clip, same machine state, and fixed settings (e.g., 1920x1080@30 NV12).

For each variant, collect:
- GPU utilization (% and engine breakdown if available)
- CPU utilization for host process and FrameServer process
- delivered frame rate
- end-to-end latency and stability
- dropped frame count if available

Test matrix:
1. Baseline current build
2. CPU-backed allocator variant
3. CPU-backed + adaptive miss backoff
4. CPU-backed + copy fast-path

This isolates high-impact changes from micro-optimizations.

---

## Practical Takeaway

The most probable root cause of unusually high GPU usage is not a classic busy-spin loop; it is memory-domain mismatch and synchronization in the sample buffer path.

The first engineering action should be allocator-path control to keep writes CPU-local, then measure again before spending time on finer optimizations.