#pragma once

/** @file Sdl3CaptureStream.h @brief Private SDL input callback and its bounded PCM owner. */
#include "Horo/Audio/Internal/Sdl3AudioBackend.h"

#include <SDL3/SDL_audio.h>
#include <atomic>
#include <vector>

namespace Horo::Audio::Backend::Sdl3Detail {
    /** @brief Reads platform authorization without prompting; the synthetic dummy driver requires no OS permission. */
    [[nodiscard]] AudioCapturePermission CapturePermission() noexcept;

    /** @brief Owns one explicit recording device, detached on Audio control before the PCM buffer retires. */
    class CaptureStream final {
    public:
        /** @brief Adopts prepared storage before attaching any native callback. */
        CaptureStream(SDL_AudioDeviceID endpoint, AudioCaptureBuffer buffer) noexcept;
        ~CaptureStream();
        CaptureStream(const CaptureStream &) = delete;
        CaptureStream &operator=(const CaptureStream &) = delete;

        /** @brief Opens the pinned endpoint and validates actual rate, channel count and bounded period before resume. */
        [[nodiscard]] Result<void> Open();
        /** @brief Checks control-side permission, device presence and callback health. */
        [[nodiscard]] const ErrorCodeDescriptor *Failure() const noexcept;
        /** @brief Borrows the queue owned by this exact capture generation. */
        [[nodiscard]] AudioCaptureBuffer &Buffer() noexcept;

    private:
        /** @brief Copies one borrowed SDL float buffer into prepared storage on the capture callback. */
        static void SDLCALL OnSamples(void *context, const SDL_AudioSpec *spec, float *pcm, int bytes) noexcept;
        /** @brief Checks the fixed format and publishes frame-indexed PCM without allocating. */
        bool Publish(const SDL_AudioSpec &spec, const float *pcm, int bytes) noexcept;
        /** @brief Estimates the first block's host-clock origin and projects later frames from that origin. */
        std::optional<AudioMonotonicTimestamp> Timestamp(std::uint32_t frames) noexcept;
        const SDL_AudioDeviceID endpoint_;
        AudioCaptureBuffer buffer_;
        SDL_AudioDeviceID device_{};
        std::atomic<bool> failed_{};
        std::uint64_t firstFrame_{};
        std::uint64_t clockOrigin_{};
    };

    /** @brief Associates a capture identity with one private SDL recording endpoint. */
    struct CaptureDeviceBinding final {
        AudioCaptureDeviceId horo;
        SDL_AudioDeviceID native{};
    };

    /** @brief Control-thread inventory and active session, independent of playback lifecycle. */
    struct CaptureControl final {
        AudioCaptureDeviceSnapshot snapshot;
        std::vector<CaptureDeviceBinding> devices;
        std::uint32_t nextSlot{1};
        std::unique_ptr<CaptureStream> stream;
        std::uint64_t lastGeneration{};
        Sdl3CaptureGrant activeGrant;

        /** @brief Resolves an explicit endpoint with the advertised semantic PCM format. */
        [[nodiscard]] Result<SDL_AudioDeviceID> ResolveEndpoint(const AudioCaptureBufferDescriptor &descriptor) const;
        /** @brief Prepares storage and publishes the already admitted generation after native open succeeds. */
        [[nodiscard]] Result<void> OpenStream(SDL_AudioDeviceID endpoint, const AudioCaptureBufferDescriptor &descriptor);
        /** @brief Detaches the stream and retires the active permission decision. */
        void Stop() noexcept;
    };
}  // namespace Horo::Audio::Backend::Sdl3Detail
