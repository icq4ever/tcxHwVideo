// =============================================================================
// tcxHwVideoPlayer_linux.cpp - Hardware-accelerated VideoPlayer (Linux)
// =============================================================================
// Uses FFmpeg hardware decode backends in priority order:
//   1. VAAPI  — Intel / AMD (libva)
//   2. V4L2M2M — Raspberry Pi / ARM SoCs
//   3. DRM    — Raspberry Pi 5
//   4. Software fallback
// Audio is pre-decoded via FFmpeg + libswresample into a SoundBuffer.
// =============================================================================

#ifdef __linux__

#include "tcxHwVideoPlayer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

#include <thread>
#include <atomic>
#include <queue>
#include <condition_variable>
#include <cstring>

using namespace trussc;

// =============================================================================
// Backend detection
// =============================================================================

// Try backends by name in priority order (runtime — no compile-time ifdefs).
// Returns AV_HWDEVICE_TYPE_NONE if none are available.
static AVHWDeviceType tryCreateHwDevice(AVBufferRef** outCtx) {
    static const char* kOrder[] = {
        "vaapi",    // Intel / AMD
        "v4l2m2m",  // RPi / ARM SoCs (hardware codec)
        "drm",      // RPi 5 DRM/KMS path
        nullptr,    // sentinel
    };

    for (int i = 0; kOrder[i] != nullptr; ++i) {
        AVHWDeviceType type = av_hwdevice_find_type_by_name(kOrder[i]);
        if (type == AV_HWDEVICE_TYPE_NONE) continue;
        if (av_hwdevice_ctx_create(outCtx, type, nullptr, nullptr, 0) >= 0) {
            return type;
        }
    }
    return AV_HWDEVICE_TYPE_NONE;
}

// =============================================================================
// TCHwVideoPlayerImpl
// =============================================================================

class TCHwVideoPlayerImpl {
public:
    TCHwVideoPlayerImpl() = default;
    ~TCHwVideoPlayerImpl() { close(); }

    bool load(const std::string& path);
    void close();
    void play();
    void stop();
    void setPaused(bool paused);
    void update(tcx::HwVideoPlayer* player);

    bool hasNewFrame() const { return hasNewFrame_; }
    bool isFinished()  const { return isFinished_; }

    float getPosition() const;
    void  setPosition(float pct);
    float getDuration() const;

    void setVolume(float vol);
    void setSpeed(float speed);
    void setLoop(bool loop) { isLoop_ = loop; }

    int  getCurrentFrame() const;
    int  getTotalFrames()  const;
    void setFrame(int frame);
    void nextFrame();
    void previousFrame();

    int getWidth()  const { return width_; }
    int getHeight() const { return height_; }

    // HW info
    bool        isUsingHwAccel() const { return hwType_ != AV_HWDEVICE_TYPE_NONE; }
    std::string getHwAccelName() const {
        if (hwType_ == AV_HWDEVICE_TYPE_NONE) return "software";
        const char* n = av_hwdevice_get_type_name(hwType_);
        return n ? n : "unknown";
    }

    // Audio info
    bool     hasAudio()          const { return hasAudio_; }
    uint32_t getAudioCodec()     const { return audioCodec_; }
    int      getAudioSampleRate() const { return audioSampleRate_; }
    int      getAudioChannels()  const { return audioChannels_; }
    std::vector<uint8_t> getAudioData() const;

    // CPU pixel buffer (for getPixels())
    const uint8_t* getPixelsCpu() { return rgbaBuffer_; }

    // Audio objects (public for seek/volume access)
    Sound                      audioSound_;
    std::shared_ptr<SoundBuffer> audioBuffer_;

private:
    bool loadAudioForPlayback();
    void decodeThreadFn();
    bool decodeNextFrame();
    void seekToTime(double seconds);

    // FFmpeg — video
    AVFormatContext* formatCtx_    = nullptr;
    AVCodecContext*  codecCtx_     = nullptr;
    SwsContext*      swsCtx_       = nullptr;
    AVFrame*         frame_        = nullptr;
    AVFrame*         frameRGBA_    = nullptr;
    AVPacket*        packet_       = nullptr;
    AVBufferRef*     hwDeviceCtx_  = nullptr;
    AVHWDeviceType   hwType_       = AV_HWDEVICE_TYPE_NONE;
    AVPixelFormat    lastScalerFmt_ = AV_PIX_FMT_NONE;

    int videoStreamIndex_ = -1;
    int audioStreamIndex_ = -1;

    // Video properties
    int    width_     = 0;
    int    height_    = 0;
    double duration_  = 0.0;
    double frameRate_ = 30.0;
    AVRational timeBase_ = {1, 1};

    // Audio properties
    bool     hasAudio_       = false;
    uint32_t audioCodec_     = 0;
    int      audioSampleRate_ = 0;
    int      audioChannels_  = 0;
    std::string filePath_;

    // Playback state
    std::atomic<bool>   isLoaded_      {false};
    std::atomic<bool>   isPlaying_     {false};
    std::atomic<bool>   isPaused_      {false};
    std::atomic<bool>   hasNewFrame_   {false};
    std::atomic<bool>   isFinished_    {false};
    std::atomic<bool>   isLoop_        {false};
    std::atomic<bool>   shouldStop_    {false};
    std::atomic<bool>   seekRequested_ {false};
    std::atomic<double> seekTarget_    {0.0};

    float volume_ = 1.0f;
    float speed_  = 1.0f;

    // Thread
    std::thread             decodeThread_;
    std::mutex              mutex_;
    std::condition_variable cv_;

    // Frame queue
    struct FrameData {
        std::vector<uint8_t> pixels;
        double pts;
    };
    std::queue<FrameData> frameQueue_;
    static constexpr size_t MAX_QUEUE_SIZE = 4;

    // Timing
    double currentPts_        = 0.0;
    double playbackStartTime_ = 0.0;
    double pausedTime_        = 0.0;

    // RGBA CPU buffer
    uint8_t* rgbaBuffer_     = nullptr;
    int      rgbaBufferSize_ = 0;
};

// =============================================================================
// load
// =============================================================================

bool TCHwVideoPlayerImpl::load(const std::string& path) {
    filePath_ = path;

    if (avformat_open_input(&formatCtx_, path.c_str(), nullptr, nullptr) < 0) {
        logError("HwVideoPlayer") << "Failed to open file: " << path;
        return false;
    }
    if (avformat_find_stream_info(formatCtx_, nullptr) < 0) {
        logError("HwVideoPlayer") << "Failed to find stream info";
        avformat_close_input(&formatCtx_);
        return false;
    }

    // Find video and audio streams
    for (unsigned int i = 0; i < formatCtx_->nb_streams; i++) {
        auto type = formatCtx_->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && videoStreamIndex_ < 0) videoStreamIndex_ = i;
        if (type == AVMEDIA_TYPE_AUDIO && audioStreamIndex_ < 0) audioStreamIndex_ = i;
    }

    if (videoStreamIndex_ < 0) {
        logError("HwVideoPlayer") << "No video stream found";
        avformat_close_input(&formatCtx_);
        return false;
    }

    // Audio stream info
    if (audioStreamIndex_ >= 0) {
        AVCodecParameters* ap = formatCtx_->streams[audioStreamIndex_]->codecpar;
        hasAudio_        = true;
        audioSampleRate_ = ap->sample_rate;
        audioChannels_   = ap->ch_layout.nb_channels;
        if      (ap->codec_id == AV_CODEC_ID_AAC) audioCodec_ = 0x61616320; // 'aac '
        else if (ap->codec_id == AV_CODEC_ID_MP3) audioCodec_ = 0x6D703320; // 'mp3 '
        else                                       audioCodec_ = ap->codec_tag;
        logNotice("HwVideoPlayer") << "Audio: " << audioChannels_ << "ch, " << audioSampleRate_ << "Hz";
    }

    // Video codec
    AVStream*          videoStream = formatCtx_->streams[videoStreamIndex_];
    AVCodecParameters* codecPar    = videoStream->codecpar;
    const AVCodec*     codec       = avcodec_find_decoder(codecPar->codec_id);
    if (!codec) {
        logError("HwVideoPlayer") << "Codec not found";
        avformat_close_input(&formatCtx_);
        return false;
    }

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_ || avcodec_parameters_to_context(codecCtx_, codecPar) < 0) {
        logError("HwVideoPlayer") << "Failed to set up codec context";
        avcodec_free_context(&codecCtx_);
        avformat_close_input(&formatCtx_);
        return false;
    }

    // Try hardware backends
    hwType_ = tryCreateHwDevice(&hwDeviceCtx_);
    if (hwType_ != AV_HWDEVICE_TYPE_NONE) {
        codecCtx_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
        logNotice("HwVideoPlayer") << "Using HW backend: " << av_hwdevice_get_type_name(hwType_);
    } else {
        logNotice("HwVideoPlayer") << "No HW backend available, using software decoding";
    }

    // Open codec (fallback to SW if HW open fails)
    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
        if (hwType_ != AV_HWDEVICE_TYPE_NONE) {
            logWarning("HwVideoPlayer") << "HW codec open failed, falling back to SW";
            hwType_ = AV_HWDEVICE_TYPE_NONE;
            avcodec_free_context(&codecCtx_);
            codecCtx_ = avcodec_alloc_context3(codec);
            avcodec_parameters_to_context(codecCtx_, codecPar);
        }
        if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
            logError("HwVideoPlayer") << "Failed to open codec";
            avcodec_free_context(&codecCtx_);
            avformat_close_input(&formatCtx_);
            return false;
        }
    }

    // Properties
    width_    = codecCtx_->width;
    height_   = codecCtx_->height;
    timeBase_ = videoStream->time_base;

    if (videoStream->avg_frame_rate.num > 0 && videoStream->avg_frame_rate.den > 0)
        frameRate_ = av_q2d(videoStream->avg_frame_rate);
    else if (videoStream->r_frame_rate.num > 0 && videoStream->r_frame_rate.den > 0)
        frameRate_ = av_q2d(videoStream->r_frame_rate);

    if (formatCtx_->duration > 0)
        duration_ = formatCtx_->duration / (double)AV_TIME_BASE;
    else if (videoStream->duration > 0)
        duration_ = videoStream->duration * av_q2d(timeBase_);

    logNotice("HwVideoPlayer") << "Video: " << width_ << "x" << height_
                                << " @ " << frameRate_ << " fps, " << duration_ << " sec";

    // Scaler (recreated dynamically if pixel format changes after HW transfer)
    swsCtx_ = sws_getContext(
        width_, height_, codecCtx_->pix_fmt,
        width_, height_, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    frame_     = av_frame_alloc();
    frameRGBA_ = av_frame_alloc();
    packet_    = av_packet_alloc();
    if (!frame_ || !frameRGBA_ || !packet_) {
        logError("HwVideoPlayer") << "Failed to allocate frames";
        close();
        return false;
    }

    rgbaBufferSize_ = av_image_get_buffer_size(AV_PIX_FMT_RGBA, width_, height_, 1);
    rgbaBuffer_     = (uint8_t*)av_malloc(rgbaBufferSize_);
    av_image_fill_arrays(frameRGBA_->data, frameRGBA_->linesize,
                         rgbaBuffer_, AV_PIX_FMT_RGBA, width_, height_, 1);

    isLoaded_ = true;

    // Pre-decode audio
    if (hasAudio_) {
        if (loadAudioForPlayback())
            logNotice("HwVideoPlayer") << "Audio decoded and ready";
        else
            logWarning("HwVideoPlayer") << "Failed to decode audio";
    }

    return true;
}

// =============================================================================
// loadAudioForPlayback
// =============================================================================

bool TCHwVideoPlayerImpl::loadAudioForPlayback() {
    if (!hasAudio_ || filePath_.empty()) return false;

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, filePath_.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    int audioIdx = -1;
    for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioIdx = i;
            break;
        }
    }
    if (audioIdx < 0) { avformat_close_input(&fmtCtx); return false; }

    AVCodecParameters* par   = fmtCtx->streams[audioIdx]->codecpar;
    const AVCodec*     codec = avcodec_find_decoder(par->codec_id);
    if (!codec) { avformat_close_input(&fmtCtx); return false; }

    AVCodecContext* audioCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(audioCtx, par);
    if (avcodec_open2(audioCtx, codec, nullptr) < 0) {
        avcodec_free_context(&audioCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    // Resample → F32 stereo at AudioEngine::SAMPLE_RATE
    SwrContext* swr = nullptr;
    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&swr,
            &outLayout,           AV_SAMPLE_FMT_FLT, AudioEngine::SAMPLE_RATE,
            &audioCtx->ch_layout, audioCtx->sample_fmt, audioCtx->sample_rate,
            0, nullptr) < 0 || swr_init(swr) < 0) {
        if (swr) swr_free(&swr);
        avcodec_free_context(&audioCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    std::vector<float> samples;
    if (duration_ > 0)
        samples.reserve((size_t)(duration_ * AudioEngine::SAMPLE_RATE * 2 * 1.05));

    AVFrame*  frame = av_frame_alloc();
    AVPacket* pkt   = av_packet_alloc();

    auto appendConverted = [&](int nbIn) {
        int outN = av_rescale_rnd(
            swr_get_delay(swr, audioCtx->sample_rate) + nbIn,
            AudioEngine::SAMPLE_RATE, audioCtx->sample_rate, AV_ROUND_UP);
        std::vector<float> buf(outN * 2);
        uint8_t* outData[1] = { (uint8_t*)buf.data() };
        int n = swr_convert(swr, outData, outN,
                            nbIn > 0 ? (const uint8_t**)frame->data : nullptr, nbIn);
        if (n > 0)
            samples.insert(samples.end(), buf.begin(), buf.begin() + n * 2);
    };

    while (av_read_frame(fmtCtx, pkt) >= 0) {
        if (pkt->stream_index == audioIdx) {
            if (avcodec_send_packet(audioCtx, pkt) == 0) {
                while (avcodec_receive_frame(audioCtx, frame) == 0) {
                    appendConverted(frame->nb_samples);
                    av_frame_unref(frame);
                }
            }
        }
        av_packet_unref(pkt);
    }

    // Flush
    avcodec_send_packet(audioCtx, nullptr);
    while (avcodec_receive_frame(audioCtx, frame) == 0) {
        appendConverted(frame->nb_samples);
        av_frame_unref(frame);
    }
    appendConverted(0);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&audioCtx);
    avformat_close_input(&fmtCtx);

    if (samples.empty()) return false;

    auto buf         = std::make_shared<SoundBuffer>();
    buf->samples     = std::move(samples);
    buf->channels    = 2;
    buf->sampleRate  = AudioEngine::SAMPLE_RATE;
    buf->numSamples  = buf->samples.size() / 2;
    audioBuffer_     = buf;
    audioSound_.loadFromBuffer(audioBuffer_);

    logNotice("HwVideoPlayer") << "Audio: " << buf->numSamples
                                << " frames @ " << buf->sampleRate << " Hz";
    return true;
}

// =============================================================================
// getAudioData (ADTS-wrapped raw AAC, for encoding workflows)
// =============================================================================

static int adtsSampleRateIndex(int sr) {
    static const int rates[] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
    for (int i = 0; i < 13; i++) if (rates[i] == sr) return i;
    return 4;
}

static void createADTSHeader(uint8_t* h, int frameSize, int sr, int ch) {
    int idx = adtsSampleRateIndex(sr);
    int len = frameSize + 7;
    h[0] = 0xFF; h[1] = 0xF1;
    h[2] = (1 << 6) | ((idx & 0x0F) << 2) | ((ch >> 2) & 0x01);
    h[3] = ((ch & 0x03) << 6) | ((len >> 11) & 0x03);
    h[4] = (len >> 3) & 0xFF;
    h[5] = ((len & 0x07) << 5) | 0x1F;
    h[6] = 0xFC;
}

std::vector<uint8_t> TCHwVideoPlayerImpl::getAudioData() const {
    if (!hasAudio_ || filePath_.empty()) return {};

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, filePath_.c_str(), nullptr, nullptr) < 0) return {};
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return {};
    }

    int audioIdx = -1;
    for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioIdx = i; break;
        }
    }
    if (audioIdx < 0) { avformat_close_input(&fmtCtx); return {}; }

    bool isAAC = (fmtCtx->streams[audioIdx]->codecpar->codec_id == AV_CODEC_ID_AAC);
    std::vector<uint8_t> audioData;
    AVPacket* pkt = av_packet_alloc();

    while (av_read_frame(fmtCtx, pkt) >= 0) {
        if (pkt->stream_index == audioIdx) {
            if (isAAC) {
                uint8_t hdr[7];
                createADTSHeader(hdr, pkt->size, audioSampleRate_, audioChannels_);
                audioData.insert(audioData.end(), hdr, hdr + 7);
            }
            audioData.insert(audioData.end(), pkt->data, pkt->data + pkt->size);
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    avformat_close_input(&fmtCtx);
    return audioData;
}

// =============================================================================
// close
// =============================================================================

void TCHwVideoPlayerImpl::close() {
    shouldStop_ = true;
    cv_.notify_all();
    if (decodeThread_.joinable()) decodeThread_.join();

    { std::lock_guard<std::mutex> lock(mutex_); while (!frameQueue_.empty()) frameQueue_.pop(); }

    audioSound_.stop();
    audioBuffer_.reset();

    if (hwDeviceCtx_) { av_buffer_unref(&hwDeviceCtx_); hwDeviceCtx_ = nullptr; }
    hwType_ = AV_HWDEVICE_TYPE_NONE;

    if (rgbaBuffer_) { av_free(rgbaBuffer_);       rgbaBuffer_  = nullptr; }
    if (packet_)     { av_packet_free(&packet_);   packet_      = nullptr; }
    if (frameRGBA_)  { av_frame_free(&frameRGBA_); frameRGBA_   = nullptr; }
    if (frame_)      { av_frame_free(&frame_);     frame_       = nullptr; }
    if (swsCtx_)     { sws_freeContext(swsCtx_);   swsCtx_      = nullptr; }
    if (codecCtx_)   { avcodec_free_context(&codecCtx_); codecCtx_ = nullptr; }
    if (formatCtx_)  { avformat_close_input(&formatCtx_); }

    lastScalerFmt_   = AV_PIX_FMT_NONE;
    isLoaded_        = false;
    isPlaying_       = false;
    isPaused_        = false;
    hasNewFrame_     = false;
    isFinished_      = false;
    width_           = 0;
    height_          = 0;
    hasAudio_        = false;
    audioStreamIndex_ = -1;
    audioCodec_      = 0;
    audioSampleRate_ = 0;
    audioChannels_   = 0;
    filePath_.clear();
}

// =============================================================================
// Playback control
// =============================================================================

void TCHwVideoPlayerImpl::play() {
    if (!isLoaded_) return;

    isFinished_ = false;
    shouldStop_ = false;

    if (currentPts_ >= duration_ - 0.1) seekToTime(0.0);

    if (!decodeThread_.joinable())
        decodeThread_ = std::thread(&TCHwVideoPlayerImpl::decodeThreadFn, this);

    playbackStartTime_ = av_gettime_relative() / 1000000.0 - currentPts_;
    isPlaying_ = true;
    isPaused_  = false;
    cv_.notify_all();

    if (audioBuffer_) {
        audioSound_.play();
        audioSound_.setPosition(static_cast<float>(currentPts_));
        audioSound_.setVolume(volume_);
    }
}

void TCHwVideoPlayerImpl::stop() {
    isPlaying_ = false;
    isPaused_  = false;
    audioSound_.stop();
    seekToTime(0.0);
    currentPts_ = 0.0;
    std::lock_guard<std::mutex> lock(mutex_);
    while (!frameQueue_.empty()) frameQueue_.pop();
}

void TCHwVideoPlayerImpl::setPaused(bool paused) {
    if (paused && !isPaused_) {
        pausedTime_ = av_gettime_relative() / 1000000.0;
        isPaused_ = true;
        if (audioBuffer_) audioSound_.pause();
    } else if (!paused && isPaused_) {
        double d = av_gettime_relative() / 1000000.0 - pausedTime_;
        playbackStartTime_ += d;
        isPaused_ = false;
        cv_.notify_all();
        if (audioBuffer_) audioSound_.resume();
    }
}

// =============================================================================
// update
// =============================================================================

void TCHwVideoPlayerImpl::update(tcx::HwVideoPlayer* player) {
    hasNewFrame_ = false;
    if (!isLoaded_ || !isPlaying_ || isPaused_) return;

    double elapsed   = av_gettime_relative() / 1000000.0 - playbackStartTime_;
    double targetPts = elapsed * speed_;

    std::lock_guard<std::mutex> lock(mutex_);

    while (!frameQueue_.empty()) {
        FrameData& front = frameQueue_.front();
        if (front.pts <= targetPts) {
            trussc::Texture& tex = player->getTexture();
            tex.loadData(front.pixels.data(), width_, height_, 4);
            if ((int)front.pixels.size() == rgbaBufferSize_)
                std::memcpy(rgbaBuffer_, front.pixels.data(), rgbaBufferSize_);
            hasNewFrame_ = true;
            currentPts_  = front.pts;
            frameQueue_.pop();
        } else {
            break;
        }
    }

    if (frameQueue_.empty() && isFinished_) {
        if (isLoop_) {
            seekToTime(0.0);
            playbackStartTime_ = av_gettime_relative() / 1000000.0;
            isFinished_ = false;
            cv_.notify_all();
        } else {
            isPlaying_ = false;
        }
    }

    cv_.notify_all();
}

// =============================================================================
// Decode thread
// =============================================================================

void TCHwVideoPlayerImpl::decodeThreadFn() {
    while (!shouldStop_) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return shouldStop_ ||
                       (isPlaying_ && !isPaused_ && frameQueue_.size() < MAX_QUEUE_SIZE) ||
                       seekRequested_;
            });
        }
        if (shouldStop_) break;

        if (seekRequested_) {
            double target  = seekTarget_;
            seekRequested_ = false;
            int64_t ts     = (int64_t)(target / av_q2d(timeBase_));
            av_seek_frame(formatCtx_, videoStreamIndex_, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(codecCtx_);
            { std::lock_guard<std::mutex> lock(mutex_); while (!frameQueue_.empty()) frameQueue_.pop(); }
            currentPts_ = target;
            continue;
        }

        if (!decodeNextFrame()) isFinished_ = true;
    }
}

bool TCHwVideoPlayerImpl::decodeNextFrame() {
    while (true) {
        int ret = av_read_frame(formatCtx_, packet_);
        if (ret < 0) return false;

        if (packet_->stream_index != videoStreamIndex_) { av_packet_unref(packet_); continue; }

        ret = avcodec_send_packet(codecCtx_, packet_);
        av_packet_unref(packet_);
        if (ret < 0) continue;

        ret = avcodec_receive_frame(codecCtx_, frame_);
        if (ret == AVERROR(EAGAIN)) continue;
        if (ret < 0) return false;

        // HW frames: transfer to CPU
        AVFrame* srcFrame = frame_;
        AVFrame* swFrame  = nullptr;
        if (hwType_ != AV_HWDEVICE_TYPE_NONE && frame_->hw_frames_ctx) {
            swFrame = av_frame_alloc();
            swFrame->format = AV_PIX_FMT_YUV420P;
            if (av_hwframe_transfer_data(swFrame, frame_, 0) < 0) {
                av_frame_free(&swFrame);
                av_frame_unref(frame_);
                continue;
            }
            srcFrame = swFrame;
        }

        // Recreate scaler if pixel format changed
        AVPixelFormat srcFmt = (AVPixelFormat)srcFrame->format;
        if (srcFmt != lastScalerFmt_ && srcFmt != AV_PIX_FMT_NONE) {
            if (swsCtx_) sws_freeContext(swsCtx_);
            swsCtx_ = sws_getContext(
                width_, height_, srcFmt,
                width_, height_, AV_PIX_FMT_RGBA,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            lastScalerFmt_ = srcFmt;
        }

        if (swsCtx_ && srcFrame->data[0]) {
            sws_scale(swsCtx_,
                      srcFrame->data, srcFrame->linesize, 0, height_,
                      frameRGBA_->data, frameRGBA_->linesize);
        }
        if (swFrame) av_frame_free(&swFrame);

        double pts = 0.0;
        if (frame_->pts != AV_NOPTS_VALUE) pts = frame_->pts * av_q2d(timeBase_);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            FrameData data;
            data.pixels.resize(width_ * height_ * 4);
            std::memcpy(data.pixels.data(), rgbaBuffer_, data.pixels.size());
            data.pts = pts;
            frameQueue_.push(std::move(data));
        }

        av_frame_unref(frame_);
        return true;
    }
}

// =============================================================================
// Position / timing
// =============================================================================

void TCHwVideoPlayerImpl::seekToTime(double seconds) {
    seekTarget_ = seconds; seekRequested_ = true; cv_.notify_all();
}

float TCHwVideoPlayerImpl::getPosition() const {
    return (duration_ <= 0) ? 0.0f : static_cast<float>(currentPts_ / duration_);
}

void TCHwVideoPlayerImpl::setPosition(float pct) {
    double t = pct * duration_;
    seekToTime(t);
    playbackStartTime_ = av_gettime_relative() / 1000000.0 - t;
    if (audioBuffer_) audioSound_.setPosition(static_cast<float>(t));
}

float TCHwVideoPlayerImpl::getDuration() const {
    return static_cast<float>(duration_);
}

void TCHwVideoPlayerImpl::setVolume(float vol) {
    volume_ = vol;
    audioSound_.setVolume(vol);
}

void TCHwVideoPlayerImpl::setSpeed(float speed) {
    speed_ = speed;
    playbackStartTime_ = av_gettime_relative() / 1000000.0 - currentPts_ / speed_;
}

int TCHwVideoPlayerImpl::getCurrentFrame() const {
    return (frameRate_ > 0) ? static_cast<int>(currentPts_ * frameRate_) : 0;
}
int TCHwVideoPlayerImpl::getTotalFrames() const {
    return (frameRate_ > 0) ? static_cast<int>(duration_ * frameRate_) : 0;
}
void TCHwVideoPlayerImpl::setFrame(int frame) {
    if (frameRate_ > 0) setPosition(static_cast<float>((frame / frameRate_) / duration_));
}
void TCHwVideoPlayerImpl::nextFrame() {
    if (frameRate_ > 0) seekToTime(currentPts_ + 1.0 / frameRate_);
}
void TCHwVideoPlayerImpl::previousFrame() {
    if (frameRate_ > 0) seekToTime(std::max(0.0, currentPts_ - 1.0 / frameRate_));
}

// =============================================================================
// HwVideoPlayer — public class
// =============================================================================

namespace tcx {

struct HwVideoPlayer::Impl {
    std::unique_ptr<TCHwVideoPlayerImpl> player;
    std::vector<uint8_t> pixelsBuf;
};

HwVideoPlayer::HwVideoPlayer() : impl_(std::make_unique<Impl>()) {}
HwVideoPlayer::~HwVideoPlayer() { close(); }

HwVideoPlayer::HwVideoPlayer(HwVideoPlayer&& other) noexcept
    : impl_(std::move(other.impl_)) {
    width_ = other.width_; height_ = other.height_;
    initialized_ = other.initialized_; playing_ = other.playing_;
    paused_ = other.paused_; frameNew_ = other.frameNew_;
    firstFrameReceived_ = other.firstFrameReceived_;
    done_ = other.done_; loop_ = other.loop_;
    volume_ = other.volume_; speed_ = other.speed_;
    texture_ = std::move(other.texture_);
    other.initialized_ = false; other.width_ = 0; other.height_ = 0;
}

HwVideoPlayer& HwVideoPlayer::operator=(HwVideoPlayer&& other) noexcept {
    if (this != &other) { close(); new (this) HwVideoPlayer(std::move(other)); }
    return *this;
}

bool HwVideoPlayer::load(const std::string& path) {
    if (initialized_) close();
    std::string resolved = getDataPath(path);
    impl_->player = std::make_unique<TCHwVideoPlayerImpl>();
    if (!impl_->player->load(resolved)) { impl_->player.reset(); return false; }
    width_  = impl_->player->getWidth();
    height_ = impl_->player->getHeight();
    texture_.allocate(width_, height_, 4, TextureUsage::Stream);
    clearTexture();
    initialized_ = true;
    firstFrameReceived_ = false;
    return true;
}

void HwVideoPlayer::close() {
    if (!initialized_) return;
    impl_->player.reset();
    texture_.clear();
    impl_->pixelsBuf.clear();
    initialized_ = false; playing_ = false; paused_ = false;
    frameNew_ = false; firstFrameReceived_ = false; done_ = false;
    width_ = 0; height_ = 0;
}

void HwVideoPlayer::update() {
    if (!initialized_) return;
    frameNew_ = false;
    impl_->player->update(this);
    if (impl_->player->hasNewFrame()) markFrameNew();
    if (playing_ && !paused_ && impl_->player->isFinished()) markDone();
}

void HwVideoPlayer::clearTexture() {
    if (width_ > 0 && height_ > 0) {
        std::vector<uint8_t> black(width_ * height_ * 4, 0);
        texture_.loadData(black.data(), width_, height_, 4);
    }
}

float HwVideoPlayer::getDuration()     const { return initialized_ ? impl_->player->getDuration()     : 0.0f; }
float HwVideoPlayer::getPosition()     const { return initialized_ ? impl_->player->getPosition()     : 0.0f; }
int   HwVideoPlayer::getCurrentFrame() const { return initialized_ ? impl_->player->getCurrentFrame() : 0; }
int   HwVideoPlayer::getTotalFrames()  const { return initialized_ ? impl_->player->getTotalFrames()  : 0; }

void HwVideoPlayer::setFrame(int f)       { if (initialized_) impl_->player->setFrame(f); }
void HwVideoPlayer::nextFrame()           { if (initialized_) impl_->player->nextFrame(); }
void HwVideoPlayer::previousFrame()       { if (initialized_) impl_->player->previousFrame(); }

unsigned char* HwVideoPlayer::getPixels() {
    if (!initialized_) return nullptr;
    const uint8_t* src = impl_->player->getPixelsCpu();
    if (!src) return nullptr;
    size_t sz = (size_t)(width_ * height_ * 4);
    impl_->pixelsBuf.assign(src, src + sz);
    return impl_->pixelsBuf.data();
}
const unsigned char* HwVideoPlayer::getPixels() const {
    return impl_->pixelsBuf.empty() ? nullptr : impl_->pixelsBuf.data();
}

bool        HwVideoPlayer::isUsingHwAccel()  const { return initialized_ && impl_->player->isUsingHwAccel(); }
std::string HwVideoPlayer::getHwAccelName()  const { return initialized_ ? impl_->player->getHwAccelName() : "none"; }

// Audio (VideoPlayerBase overrides)
bool                 HwVideoPlayer::hasAudio()          const { return initialized_ && impl_->player->hasAudio(); }
uint32_t             HwVideoPlayer::getAudioCodec()     const { return initialized_ ? impl_->player->getAudioCodec()     : 0; }
std::vector<uint8_t> HwVideoPlayer::getAudioData()      const { return initialized_ ? impl_->player->getAudioData()      : std::vector<uint8_t>{}; }
int                  HwVideoPlayer::getAudioSampleRate() const { return initialized_ ? impl_->player->getAudioSampleRate() : 0; }
int                  HwVideoPlayer::getAudioChannels()  const { return initialized_ ? impl_->player->getAudioChannels()  : 0; }

// Protected impl
void HwVideoPlayer::playImpl()                 { impl_->player->play(); }
void HwVideoPlayer::stopImpl()                 { impl_->player->stop(); clearTexture(); }
void HwVideoPlayer::setPausedImpl(bool paused) { impl_->player->setPaused(paused); }
void HwVideoPlayer::setPositionImpl(float pct) { impl_->player->setPosition(pct); }
void HwVideoPlayer::setVolumeImpl(float vol)   { impl_->player->setVolume(vol); }
void HwVideoPlayer::setSpeedImpl(float speed)  { impl_->player->setSpeed(speed); }
void HwVideoPlayer::setPanImpl(float)          {}
void HwVideoPlayer::setLoopImpl(bool loop)     { impl_->player->setLoop(loop); }

} // namespace tcx

#endif // __linux__
