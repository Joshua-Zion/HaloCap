#include "audio_source.h"

#include <algorithm>
#include <cmath>

namespace halocap::core {

StereoRingBuffer::StereoRingBuffer(std::size_t capacityFrames)
    : capacityFrames_(capacityFrames), samples_(capacityFrames * 2, 0.0f) {}

void StereoRingBuffer::Push(const float* interleavedStereo, std::int32_t frameCount) {
    if (interleavedStereo == nullptr || frameCount <= 0) {
        return;
    }

    pushedFrames_ += frameCount;

    if (capacityFrames_ == 0) {
        droppedFrames_ += frameCount;
        return;
    }

    // 一次来的比整个缓冲还大：前面的那些根本没地方放，直接算丢。
    std::int32_t first = 0;
    if (static_cast<std::size_t>(frameCount) > capacityFrames_) {
        const std::int32_t skip = frameCount - static_cast<std::int32_t>(capacityFrames_);
        droppedFrames_ += skip;
        first = skip;
    }

    for (std::int32_t i = first; i < frameCount; ++i) {
        samples_[head_ * 2] = interleavedStereo[i * 2];
        samples_[head_ * 2 + 1] = interleavedStereo[i * 2 + 1];
        head_ = (head_ + 1) % capacityFrames_;

        // 满了的话这一写就吃掉最旧的一帧；used_ 不动，tail 随 head 一起前移。
        if (used_ < capacityFrames_) {
            ++used_;
        } else {
            ++droppedFrames_;
        }
    }
}

std::int32_t StereoRingBuffer::AvailableFrames() const {
    return static_cast<std::int32_t>(used_);
}

std::int32_t StereoRingBuffer::Pop(float* destination, std::int32_t frameCount) {
    if (destination == nullptr || frameCount <= 0 || capacityFrames_ == 0) {
        return 0;
    }

    const std::size_t taken = std::min<std::size_t>(static_cast<std::size_t>(frameCount), used_);
    const std::size_t tail = (head_ + capacityFrames_ - used_) % capacityFrames_;

    for (std::size_t i = 0; i < taken; ++i) {
        const std::size_t index = (tail + i) % capacityFrames_;
        destination[i * 2] = samples_[index * 2];
        destination[i * 2 + 1] = samples_[index * 2 + 1];
    }

    used_ -= taken;
    return static_cast<std::int32_t>(taken);
}

void StereoRingBuffer::Reset() {
    used_ = 0;
    head_ = 0;
    droppedFrames_ = 0;
    pushedFrames_ = 0;
}

void MixInto(float* destination, const float* source, std::int32_t frameCount, float gain) {
    if (destination == nullptr || source == nullptr || frameCount <= 0) {
        return;
    }

    const std::size_t samples = static_cast<std::size_t>(frameCount) * 2;
    for (std::size_t i = 0; i < samples; ++i) {
        const float mixed = destination[i] + source[i] * gain;
        destination[i] = std::clamp(mixed, -1.0f, 1.0f);
    }
}

void RemixToStereo(float* destination, const float* source, std::int32_t frameCount,
                   std::int32_t sourceChannels) {
    if (destination == nullptr || source == nullptr || frameCount <= 0) {
        return;
    }

    for (std::int32_t i = 0; i < frameCount; ++i) {
        if (sourceChannels <= 0) {
            destination[i * 2] = 0.0f;
            destination[i * 2 + 1] = 0.0f;
        } else if (sourceChannels == 1) {
            destination[i * 2] = source[i];
            destination[i * 2 + 1] = source[i];
        } else {
            // >=2 取前两路：5.1 的中置与环绕按 1.x 的行为丢掉。
            destination[i * 2] = source[static_cast<std::size_t>(i) * sourceChannels];
            destination[i * 2 + 1] = source[static_cast<std::size_t>(i) * sourceChannels + 1];
        }
    }
}

std::int32_t ResampledFrameCount(std::int32_t sourceFrames, double ratio) {
    if (sourceFrames <= 0 || !(ratio > 0.0)) {
        return 0;
    }
    return static_cast<std::int32_t>(static_cast<double>(sourceFrames) * ratio);
}

void ResampleLinear(const float* source, std::int32_t sourceFrames, std::int32_t channels,
                    double ratio, float* destination, std::int32_t destinationFrames) {
    if (source == nullptr || destination == nullptr || sourceFrames <= 0 || channels <= 0 ||
        !(ratio > 0.0)) {
        return;
    }

    for (std::int32_t d = 0; d < destinationFrames; ++d) {
        const double srcPos = static_cast<double>(d) / ratio;
        std::int32_t i0 = static_cast<std::int32_t>(srcPos);
        if (i0 > sourceFrames - 1) {
            i0 = sourceFrames - 1;
        }
        const std::int32_t i1 = std::min(i0 + 1, sourceFrames - 1);
        const float frac = static_cast<float>(srcPos - i0);

        for (std::int32_t c = 0; c < channels; ++c) {
            const float a = source[static_cast<std::size_t>(i0) * channels + c];
            const float b = source[static_cast<std::size_t>(i1) * channels + c];
            destination[static_cast<std::size_t>(d) * channels + c] = a + (b - a) * frac;
        }
    }
}

std::int32_t SilenceFramesToNextBlock(std::int64_t totalFrames, std::int32_t blockSize) {
    if (blockSize <= 0 || totalFrames < 0) {
        return 0;
    }
    const std::int64_t remainder = totalFrames % blockSize;
    return remainder == 0 ? 0 : static_cast<std::int32_t>(blockSize - remainder);
}

std::int64_t FramesToUs(std::int64_t frames, std::int32_t sampleRateHz) {
    if (sampleRateHz <= 0) {
        return 0;
    }
    return frames * 1000000 / sampleRateHz;
}

std::int64_t UsToFrames(std::int64_t us, std::int32_t sampleRateHz) {
    if (sampleRateHz <= 0 || us <= 0) {
        return 0;
    }
    return us * sampleRateHz / 1000000;
}

}
