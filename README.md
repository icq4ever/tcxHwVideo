# tcxHwVideo — archived

> **⚠️ This addon is archived.**
>
> The hardware decode functionality originally developed here has been **merged into TrussC core** as of PR [#30](https://github.com/TrussC-org/TrussC/pull/30). `tc::VideoPlayer` on Linux now auto-detects VAAPI / V4L2M2M / DRM / CUDA / VDPAU backends with a software fallback — no addon needed.
>
> This repository is kept for reference / git history only; no further development will happen here.

**Use `tc::VideoPlayer` from TrussC core instead:**

```cpp
tc::VideoPlayer video;
video.load("movie.mp4");
video.play();

video.isUsingHwAccel();   // true if a HW backend is active
video.getHwAccelName();   // "vaapi", "v4l2m2m", "drm", "cuda", "vdpau", or "software"
video.setUseHwAccel(false);  // force software decode (call before load)
```

Related PRs:

- [#28](https://github.com/TrussC-org/TrussC/pull/28) — Linux audio playback
- [#30](https://github.com/TrussC-org/TrussC/pull/30) — HW decode integration
- [#31](https://github.com/TrussC-org/TrussC/pull/31) — AV sync (audio-master clock + hard re-sync)

---

## Original description (for reference)

TrussC addon for hardware-accelerated video playback via FFmpeg.

Automatically selects the best available hardware backend at runtime:

| Backend    | Platform              |
|------------|-----------------------|
| VAAPI      | Intel / AMD (Linux)   |
| V4L2M2M    | Raspberry Pi / ARM    |
| DRM        | Raspberry Pi 5        |
| *(fallback)* | Software decode     |

### Requirements

- TrussC
- FFmpeg with hardware decode support (`libavcodec`, `libavutil`)
  - VAAPI: `ffmpeg` built with `--enable-vaapi`
  - V4L2M2M: `ffmpeg` built with `--enable-v4l2-m2m`

On Raspberry Pi OS and most Linux distros, the system FFmpeg package includes these.

### Usage

```cpp
#include "tcxHwVideoPlayer.h"

tcx::HwVideoPlayer video;

void setup() {
    video.load("movie.mp4");
    video.play();
}

void update() {
    video.update();
}

void draw() {
    video.draw(0, 0);
}
```

Drop-in replacement for `tc::VideoPlayer` with the same API.

### API

```cpp
// Load / playback — same as VideoPlayer
video.load(path);
video.play();
video.stop();
video.setPaused(bool);
video.setPosition(float);   // 0.0 – 1.0
video.setVolume(float);     // 0.0 – 1.0
video.setSpeed(float);
video.setLoop(bool);

// Query
video.isLoaded();
video.isPlaying();
video.getDuration();
video.getPosition();

// Hardware info
video.isUsingHwAccel();     // true if a HW backend is active
video.getHwAccelName();     // "vaapi", "v4l2m2m", "drm", or "software"

// Pixel access (avoid per-frame — triggers DMA transfer from GPU)
video.getPixels();
```

---

## License

MIT
