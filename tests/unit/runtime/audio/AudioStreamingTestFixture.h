#pragma once

#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/AudioStreamingService.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <exception>
#include <thread>
#include <utility>

// Shared deterministic package/decoder fixture; no production behavior is replaced here.
namespace Horo::Audio::StreamingTests {
    enum class OpenFailure : std::uint8_t {
        None,
        TypedFailure,
        StandardException,
        UnknownException
    };

    /** @brief Dedicated standard exception injected at the package-opening boundary. */
    class PackageOpenException final : public std::exception {
    public:
        const char *what() const noexcept override {
            return "Injected package open failure";
        }
    };

    /** @brief Dedicated non-standard provider failure requiring catch-all containment. */
    struct UnknownPackageOpenFailure final {};

    /** @brief Injects provider faults independently of package-opening and ownership logic. */
    inline void InjectOpenFailure(const OpenFailure failure) {
        if (failure == OpenFailure::StandardException)
            throw PackageOpenException{};
        if (failure == OpenFailure::UnknownException)
            throw UnknownPackageOpenFailure{};
    }

    struct PackageFixture final {
        std::atomic<std::uint32_t> opens{};
        std::atomic<std::uint32_t> releases{};
        std::atomic<std::uint32_t> decodes{};
        std::atomic<std::uint32_t> firstOpenedAsset{};
        std::atomic<bool> opening{};
        std::atomic<bool> holdOpen{};
        std::atomic<bool> finalDecodeEntered{};
        std::atomic<bool> holdFinalDecode{};
        std::atomic<bool> ignoreCancellation{};
        std::atomic<bool> holdFirstDecode{};
        std::atomic<bool> firstDecodeEntered{};
        std::atomic<bool> privateCancellationObserved{};
        std::atomic<bool> privateCancellationTimedOut{};
        bool openAfterCancellation{};
        bool decodeFailure{};
        bool wrongSpec{};
        OpenFailure openFailure{OpenFailure::None};
    };

    /** @brief Stable stereo callback storage shared by ring and cancellation regressions. */
    struct CallbackBlock final {
        std::array<AudioSample, 4> left{};
        std::array<AudioSample, 4> right{};
        std::array<AudioSample *, 2> planes{left.data(), right.data()};

        AudioStreamRenderResult Render(const AudioStreamRenderPort &port, const std::uint32_t frames) {
            return port.Render(planes, frames);
        }
    };

    struct DecoderContext final {
        PackageFixture *fixture{};
        std::uint64_t frameCount{};
    };

    inline Assets::AssetId Asset(const std::uint8_t suffix) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = suffix;
        return Assets::AssetId::FromBytes(bytes);
    }

    inline AudioStreamRequest Request(const std::uint8_t assetSuffix = 1) {
        AudioStreamRequest request;
        request.asset = Asset(assetSuffix);
        request.decoder = {AudioCodecIds::Pcm, {48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)}, 8, 2, 16, false};
        request.ringFrames = 4;
        request.lookaheadFrames = 4;
        request.maximumPackageBytes = 1'024;
        return request;
    }

    inline bool Until(const auto &condition) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!condition()) {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::yield();
        }
        return true;
    }

    /** @brief Models a provider blocked on only its private decoder cancellation, never the parent token. */
    inline void AwaitPrivateCancellation(PackageFixture &fixture, const std::uint64_t firstFrame, const std::atomic<bool> &cancelled) {
        if (firstFrame != 0 || !fixture.holdFirstDecode.load())
            return;
        fixture.firstDecodeEntered.store(true);
        const bool observed = Until([&cancelled] {
            return cancelled.load();
        });
        fixture.privateCancellationObserved.store(observed);
        fixture.privateCancellationTimedOut.store(!observed);
    }

    /** @brief Checks bounded stereo storage independently of provider progress and cancellation. */
    inline bool ValidDecodeStorage(const std::span<AudioSample> output, const std::span<std::byte> scratch) {
        return scratch.size() == 16 && !output.empty() && output.size() <= 4 && output.size() % 2 == 0;
    }

    inline Result<AudioStreamDecodeProgress> Decode(const BorrowedCallbackContext &borrowed, const std::uint64_t firstFrame,
                                                    const std::span<AudioSample> output, const std::span<std::byte> scratch,
                                                    const std::atomic<bool> &cancelled) {
        auto *resolved = borrowed.Get<DecoderContext>();
        if (resolved == nullptr)
            return Result<AudioStreamDecodeProgress>::Failure(MakeError(AudioErrors::StreamReadFailed));
        auto &context = *resolved;
        auto &fixture = *context.fixture;
        fixture.decodes.fetch_add(1);
        AwaitPrivateCancellation(fixture, firstFrame, cancelled);
        const auto requestedFrames = output.size() / 2;
        if (firstFrame + requestedFrames >= context.frameCount) {
            fixture.finalDecodeEntered.store(true);
            while (fixture.holdFinalDecode.load() && (fixture.ignoreCancellation.load() || !cancelled.load()))
                std::this_thread::yield();
        }
        if (cancelled.load())
            return Result<AudioStreamDecodeProgress>::Failure(MakeError(AudioErrors::OperationCancelled));
        if (fixture.decodeFailure)
            return Result<AudioStreamDecodeProgress>::Failure(MakeError(AudioErrors::StreamReadFailed));
        if (!ValidDecodeStorage(output, scratch))
            return Result<AudioStreamDecodeProgress>::Failure(MakeError(AudioErrors::StreamReadFailed));
        const auto frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(requestedFrames, context.frameCount - firstFrame));
        for (std::size_t index = 0; index < frames * 2U; ++index)
            output[index] = static_cast<float>(firstFrame + index / 2 + 1);
        return Result<AudioStreamDecodeProgress>::Success({frames, firstFrame + frames == context.frameCount});
    }

    inline void Release(const BorrowedCallbackContext &borrowed) noexcept {
        const std::unique_ptr<DecoderContext> context(borrowed.Get<DecoderContext>());
        if (context)
            context->fixture->releases.fetch_add(1);
    }

    /** @brief Completes the fixture's held-open phase while preserving explicit late-open cancellation behavior. */
    inline bool AwaitPackageOpening(PackageFixture &fixture, const CancellationToken &cancelled) {
        while (fixture.holdOpen.load() && (fixture.ignoreCancellation.load() || !cancelled.IsCancellationRequested()))
            std::this_thread::yield();
        return !cancelled.IsCancellationRequested() || fixture.openAfterCancellation;
    }

    inline Result<AudioStreamDecoder> Open(const BorrowedCallbackContext &borrowed, const Assets::AssetId asset,
                                           const AudioStreamDecoderSpec &expected, const std::size_t maximumPackageBytes,
                                           const CancellationToken &cancelled) {
        auto *resolved = borrowed.Get<PackageFixture>();
        if (resolved == nullptr)
            return Result<AudioStreamDecoder>::Failure(MakeError(AudioErrors::StreamReadFailed));
        auto &fixture = *resolved;
        fixture.opening.store(true);
        InjectOpenFailure(fixture.openFailure);
        if (fixture.openFailure == OpenFailure::TypedFailure)
            return Result<AudioStreamDecoder>::Failure(MakeError(AudioErrors::CookPayloadInvalid));
        if (!AwaitPackageOpening(fixture, cancelled))
            return Result<AudioStreamDecoder>::Failure(MakeError(AudioErrors::OperationCancelled));
        if (maximumPackageBytes < 32)
            return Result<AudioStreamDecoder>::Failure(MakeError(AudioErrors::StreamCapacityExceeded));
        fixture.opens.fetch_add(1);
        std::uint32_t firstExpected = 0;
        (void)fixture.firstOpenedAsset.compare_exchange_strong(firstExpected, asset.Bytes().back());
        auto spec = expected;
        if (fixture.wrongSpec)
            ++spec.frameCount;
        auto context = std::make_unique<DecoderContext>(DecoderContext{&fixture, spec.frameCount});
        auto opened = AudioStreamDecoder::Create(spec, {BorrowedCallbackContext{context.get()}, &Decode, nullptr, &Release});
        if (opened.HasValue())
            (void)context.release();
        return opened;
    }

    inline std::unique_ptr<AudioStreamingService> Service(JobSystem &jobs, PackageFixture &fixture,
                                                          AudioStreamingLimits limits = {2, 1, 1U << 20U}) {
        auto created = AudioStreamingService::Create(jobs, {BorrowedCallbackContext{&fixture}, &Open}, limits);
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    inline bool PumpUntil(AudioStreamingService &service, const AudioStreamHandle handle, const std::uint32_t buffered) {
        const bool reached = Until([&service, handle, buffered] {
            service.Pump();
            const auto snapshot = service.Snapshot(handle);
            return snapshot.HasValue() && snapshot.Value().bufferedFrames >= buffered;
        });
        if (!reached) {
            const auto state = service.Snapshot(handle);
            if (state.HasValue()) {
                const auto &value = state.Value();
                WARN("Buffer deadline: frames=" << value.bufferedFrames << " ended=" << value.sourceEnded << " failed=" << value.failed
                                                << " cancelled=" << value.cancelled << " stopped=" << value.stopped);
            }
        }
        return reached;
    }

    /** @brief Reaps a worker failure on control before any test inspects its original error. */
    inline bool PumpUntilFailure(AudioStreamingService &service, const AudioStreamHandle handle) {
        return Until([&service, handle] {
            service.Pump();
            return service.Snapshot(handle).Value().failed;
        });
    }

}  // namespace Horo::Audio::StreamingTests
