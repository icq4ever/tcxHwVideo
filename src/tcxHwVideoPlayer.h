#pragma once

// =============================================================================
// tcxHwVideoPlayer.h - Hardware-accelerated video player
// =============================================================================
// VideoPlayerBase implementation using FFmpeg hardware decode backends.
// Linux: tries VAAPI (Intel/AMD) then V4L2M2M (RPi/ARM) automatically.
//
// Usage:
//   tcx::HwVideoPlayer video;
//   video.load("movie.mp4");
//   video.play();
//
//   void update() { video.update(); }
//   void draw()   { video.draw(0, 0); }
//
// Notes:
//   - getPixels() performs a DMA transfer from GPU to CPU (expensive).
//     Avoid calling per-frame unless pixel access is required.
//   - If no hardware backend is available, falls back to software decoding.
// =============================================================================

#include <TrussC.h>
#include <memory>
#include <string>

namespace tcx {

class HwVideoPlayer : public tc::VideoPlayerBase {
public:
    HwVideoPlayer();
    ~HwVideoPlayer() override;

    // Non-copyable
    HwVideoPlayer(const HwVideoPlayer&) = delete;
    HwVideoPlayer& operator=(const HwVideoPlayer&) = delete;

    // Move-enabled
    HwVideoPlayer(HwVideoPlayer&& other) noexcept;
    HwVideoPlayer& operator=(HwVideoPlayer&& other) noexcept;

    // =========================================================================
    // Load / Close
    // =========================================================================

    bool load(const std::string& path) override;
    void close() override;

    // =========================================================================
    // Update
    // =========================================================================

    void update() override;

    // =========================================================================
    // Properties
    // =========================================================================

    float getDuration() const override;
    float getPosition() const override;

    // =========================================================================
    // Frame control
    // =========================================================================

    int getCurrentFrame() const override;
    int getTotalFrames() const override;
    void setFrame(int frame) override;
    void nextFrame() override;
    void previousFrame() override;

    // =========================================================================
    // Pixel access
    // =========================================================================

    // Warning: calls av_hwframe_transfer_data (DMA copy, slow per-frame).
    // For display-only use, prefer draw() which uses the texture directly.
    unsigned char* getPixels() override;
    const unsigned char* getPixels() const override;

    // =========================================================================
    // Audio (VideoPlayerBase overrides)
    // =========================================================================

    bool                 hasAudio()          const override;
    uint32_t             getAudioCodec()     const override;
    std::vector<uint8_t> getAudioData()      const override;
    int                  getAudioSampleRate() const override;
    int                  getAudioChannels()  const override;

    // =========================================================================
    // HW-specific info
    // =========================================================================

    // Returns true if a hardware backend is active
    bool isUsingHwAccel() const;

    // Returns backend name: "vaapi", "v4l2m2m", "software", etc.
    std::string getHwAccelName() const;

protected:
    void playImpl() override;
    void stopImpl() override;
    void setPausedImpl(bool paused) override;
    void setPositionImpl(float pct) override;
    void setVolumeImpl(float vol) override;
    void setSpeedImpl(float speed) override;
    void setPanImpl(float pan) override;
    void setLoopImpl(bool loop) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    void clearTexture();
};

} // namespace tcx
