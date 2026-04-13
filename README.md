# tcxHwVideo

TrussC addon for hardware-accelerated video playback via FFmpeg.

Automatically selects the best available hardware backend at runtime:

| Backend    | Platform              |
|------------|-----------------------|
| VAAPI      | Intel / AMD (Linux)   |
| V4L2M2M    | Raspberry Pi / ARM    |
| DRM        | Raspberry Pi 5        |
| *(fallback)* | Software decode     |

---

## Requirements

- TrussC
- FFmpeg with hardware decode support (`libavcodec`, `libavutil`)
  - VAAPI: `ffmpeg` built with `--enable-vaapi`
  - V4L2M2M: `ffmpeg` built with `--enable-v4l2-m2m`

On Raspberry Pi OS and most Linux distros, the system FFmpeg package includes these.

---

## Usage

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

---

## API

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

## addons.make

```
tcxHwVideo
```

---

## Notes

- `getPixels()` performs a GPU→CPU DMA transfer (`av_hwframe_transfer_data`). Avoid calling every frame unless pixel access is required. For display-only use, `draw()` uses the GPU texture directly.
- Audio is pre-decoded at load time (same as `tc::VideoPlayer`).
- macOS / Windows support is planned.

---

## Roadmap

- [ ] macOS — VideoToolbox (`AV_HWDEVICE_TYPE_VIDEOTOOLBOX`)
- [ ] Windows — D3D11VA / NVDEC
- [ ] NVIDIA Linux — CUDA / NVDEC (`AV_HWDEVICE_TYPE_CUDA`)
- [ ] Streaming audio (instead of pre-loading) for very long videos
- [ ] `getPixels()` zero-copy path via DMA-BUF

---

## License

MIT
