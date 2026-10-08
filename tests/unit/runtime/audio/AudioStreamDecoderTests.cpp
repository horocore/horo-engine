#include "AllocationProbe.h"
#include "Horo/Audio/AudioStreamDecoder.h"
#include "Horo/Audio/AudioStreamDecoderErrors.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <utility>

namespace Horo::Audio {
    namespace {
        enum class Behavior {
            Normal,
            Error,
            Throw,
            ThrowNonStd,
            BadProgress,
            Cancel
        };

        struct FakeProvider final {
            Behavior behavior{Behavior::Normal};
            AudioStreamDecoder *owner{};
            std::uint64_t lastFirstFrame{};
            std::uint64_t lastSeek{};
            std::uint32_t decodeCalls{};
            std::uint32_t seekCalls{};
            std::uint32_t releases{};

            static Result<AudioStreamDecodeProgress> Decode(const BorrowedCallbackContext &context, const std::uint64_t firstFrame,
                                                            const std::span<AudioSample> output, const std::span<std::byte> scratch,
                                                            const std::atomic<bool> &cancelled) {
                auto *resolved = context.Get<FakeProvider>();
                if (resolved == nullptr)
                    return Result<AudioStreamDecodeProgress>::Failure(MakeError(AudioStreamDecoderErrors::Invalid));
                auto &self = *resolved;
                ++self.decodeCalls;
                self.lastFirstFrame = firstFrame;
                if (scratch.size() != 16 || cancelled.load())
                    return Result<AudioStreamDecodeProgress>::Failure(MakeError(AudioStreamDecoderErrors::Invalid));
                if (self.behavior == Behavior::Throw)
                    throw std::runtime_error("provider failure");
                if (self.behavior == Behavior::ThrowNonStd)
                    throw 42;
                if (self.behavior == Behavior::Error)
                    return Result<AudioStreamDecodeProgress>::Failure(MakeError(AudioStreamDecoderErrors::SeekUnsupported));
                if (self.behavior == Behavior::Cancel)
                    self.owner->Cancel();
                for (auto &sample : output)
                    sample = 0.5F;
                if (self.behavior == Behavior::BadProgress)
                    return Result<AudioStreamDecodeProgress>::Success({3, false});
                const auto frames = std::min(static_cast<std::uint32_t>(output.size() / 2), static_cast<std::uint32_t>(5 - firstFrame));
                return Result<AudioStreamDecodeProgress>::Success({frames, firstFrame + frames == 5});
            }

            static Result<void> Seek(const BorrowedCallbackContext &context, const std::uint64_t target, const std::atomic<bool> &) {
                auto *resolved = context.Get<FakeProvider>();
                if (resolved == nullptr)
                    return Result<void>::Failure(MakeError(AudioStreamDecoderErrors::Invalid));
                auto &self = *resolved;
                ++self.seekCalls;
                self.lastSeek = target;
                if (self.behavior == Behavior::ThrowNonStd)
                    throw 42;
                if (self.behavior == Behavior::Error)
                    return Result<void>::Failure(MakeError(AudioStreamDecoderErrors::ProviderFailed));
                return Result<void>::Success();
            }

            static void Release(const BorrowedCallbackContext &context) noexcept {
                if (auto *resolved = context.Get<FakeProvider>(); resolved != nullptr)
                    ++resolved->releases;
            }
        };

        AudioStreamDecoderSpec Spec(const bool seekable = true) {
            return {AudioCodecIds::Pcm, {48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)}, 5, 2, 16, seekable};
        }

        AudioStreamDecoderProvider Provider(FakeProvider &fake, const bool seekable = true) {
            return {BorrowedCallbackContext{&fake}, &FakeProvider::Decode, seekable ? &FakeProvider::Seek : nullptr,
                    &FakeProvider::Release};
        }

        template <typename T> void RequireCode(const Result<T> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == descriptor.domain.Value());
            CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
        }
    }  // namespace

    TEST_CASE("Runtime stream decoder admits bounded progressive blocks and exact terminal seek", "[unit][audio][stream_decoder]") {
        FakeProvider fake;
        auto created = AudioStreamDecoder::Create(Spec(), Provider(fake));
        REQUIRE(created.HasValue());
        auto decoder = std::move(created).Value();
        std::array<AudioSample, 4> output{};
        std::array<std::byte, 16> scratch{};

        const auto allocationsBefore = Tests::AllocationProbe::Count();
        const auto first = decoder.Decode(output, scratch, 2);
        const auto allocationsAfter = Tests::AllocationProbe::Count();
        REQUIRE(first.HasValue());
        CHECK(allocationsAfter == allocationsBefore);
        CHECK(first.Value().firstFrame == 0);
        CHECK(first.Value().frames == 2);
        CHECK_FALSE(first.Value().endOfStream);
        CHECK(output[0] == 0.5F);
        CHECK(output[3] == 0.5F);
        CHECK(decoder.CursorFrame() == 2);

        RequireCode(decoder.Decode(output, scratch, 3), AudioStreamDecoderErrors::CapacityExceeded);
        RequireCode(decoder.Decode(output, std::span<std::byte>{}, 1), AudioStreamDecoderErrors::CapacityExceeded);
        RequireCode(decoder.Decode(output, scratch, 0), AudioStreamDecoderErrors::Invalid);
        CHECK(decoder.CursorFrame() == 2);
        CHECK(fake.decodeCalls == 1);

        REQUIRE(decoder.Seek(4).HasValue());
        CHECK(fake.lastSeek == 4);
        const auto last = decoder.Decode(output, scratch, 2);
        REQUIRE(last.HasValue());
        CHECK(last.Value().firstFrame == 4);
        CHECK(last.Value().frames == 1);
        CHECK(last.Value().endOfStream);
        CHECK(decoder.State() == AudioStreamDecoderState::Ended);
        const auto repeated = decoder.Decode(output, scratch, 1);
        REQUIRE(repeated.HasValue());
        CHECK(repeated.Value().frames == 0);
        CHECK(fake.decodeCalls == 2);

        REQUIRE(decoder.Seek(0).HasValue());
        CHECK(decoder.State() == AudioStreamDecoderState::Ready);
        decoder.Close();
        decoder.Close();
        CHECK(fake.releases == 1);
        RequireCode(decoder.Seek(0), AudioStreamDecoderErrors::LifecycleUnavailable);
    }

    TEST_CASE("Runtime stream decoder rejects invalid admission without taking provider ownership", "[unit][audio][stream_decoder]") {
        FakeProvider fake;
        auto invalid = Spec();
        invalid.maximumFramesPerDecode = 0;
        RequireCode(AudioStreamDecoder::Create(invalid, Provider(fake)), AudioStreamDecoderErrors::Invalid);
        invalid = Spec();
        invalid.frameCount = 6;
        AudioStreamDecoderLimits limits;
        limits.maximumFrames = 5;
        RequireCode(AudioStreamDecoder::Create(invalid, Provider(fake), limits), AudioStreamDecoderErrors::CapacityExceeded);
        RequireCode(AudioStreamDecoder::Create(Spec(), Provider(fake, false)), AudioStreamDecoderErrors::Invalid);
        auto emptyContext = Provider(fake);
        emptyContext.context = {};
        RequireCode(AudioStreamDecoder::Create(Spec(), emptyContext), AudioStreamDecoderErrors::Invalid);
        CHECK(fake.releases == 0);
    }

    TEST_CASE("Runtime stream decoder rejects mismatched borrowed state before provider access", "[unit][audio][stream_decoder]") {
        std::uint32_t foreignState{73};
        FakeProvider fake;
        auto provider = Provider(fake);
        const auto allocationsBefore = Tests::AllocationProbe::Count();
        provider.context = BorrowedCallbackContext{&foreignState};
        CHECK(Tests::AllocationProbe::Count() == allocationsBefore);
        std::array<AudioSample, 4> output{};
        std::array<std::byte, 16> scratch{};
        std::atomic<bool> cancelled{};
        RequireCode(provider.decode(provider.context, 0, output, scratch, cancelled), AudioStreamDecoderErrors::Invalid);
        RequireCode(provider.seek(provider.context, 0, cancelled), AudioStreamDecoderErrors::Invalid);
        provider.release(provider.context);
        CHECK(foreignState == 73);
        CHECK(fake.decodeCalls == 0);
        CHECK(fake.seekCalls == 0);
        CHECK(fake.releases == 0);

        auto created = AudioStreamDecoder::Create(Spec(), provider);
        REQUIRE(created.HasValue());
        auto decoder = std::move(created).Value();
        RequireCode(decoder.Decode(output, scratch, 2), AudioStreamDecoderErrors::Invalid);
        CHECK(decoder.State() == AudioStreamDecoderState::Failed);
        CHECK(decoder.CursorFrame() == 0);
        decoder.Close();
        CHECK(foreignState == 73);
        CHECK(fake.releases == 0);
    }

    TEST_CASE("Runtime stream decoder preserves provider failure and latches unsafe progress", "[unit][audio][stream_decoder]") {
        std::array<AudioSample, 4> output{};
        std::array<std::byte, 16> scratch{};
        for (const Behavior behavior : {Behavior::Error, Behavior::Throw, Behavior::ThrowNonStd, Behavior::BadProgress, Behavior::Cancel}) {
            FakeProvider fake;
            fake.behavior = behavior;
            auto created = AudioStreamDecoder::Create(Spec(), Provider(fake));
            REQUIRE(created.HasValue());
            auto decoder = std::move(created).Value();
            fake.owner = &decoder;
            const auto result = decoder.Decode(output, scratch, 2);
            if (behavior == Behavior::Error)
                RequireCode(result, AudioStreamDecoderErrors::SeekUnsupported);
            else if (behavior == Behavior::BadProgress)
                RequireCode(result, AudioStreamDecoderErrors::ProtocolViolation);
            else if (behavior == Behavior::Cancel)
                RequireCode(result, AudioStreamDecoderErrors::Cancelled);
            else
                RequireCode(result, AudioStreamDecoderErrors::ProviderFailed);
            CHECK(decoder.CursorFrame() == 0);
            CHECK(decoder.State() == (behavior == Behavior::Cancel ? AudioStreamDecoderState::Cancelled : AudioStreamDecoderState::Failed));
            RequireCode(decoder.Decode(output, scratch, 1), AudioStreamDecoderErrors::LifecycleUnavailable);
            decoder.Close();
            CHECK(fake.releases == 1);
        }
    }

    TEST_CASE("Runtime stream decoder rejects unsupported seek and latches failed seek", "[unit][audio][stream_decoder]") {
        FakeProvider fake;
        auto created = AudioStreamDecoder::Create(Spec(false), Provider(fake, false));
        REQUIRE(created.HasValue());
        auto decoder = std::move(created).Value();
        RequireCode(decoder.Seek(0), AudioStreamDecoderErrors::SeekUnsupported);
        CHECK(fake.seekCalls == 0);
        decoder.Close();

        fake.behavior = Behavior::Error;
        auto seekable = AudioStreamDecoder::Create(Spec(), Provider(fake));
        REQUIRE(seekable.HasValue());
        auto failing = std::move(seekable).Value();
        RequireCode(failing.Seek(6), AudioStreamDecoderErrors::Invalid);
        CHECK(fake.seekCalls == 0);
        RequireCode(failing.Seek(2), AudioStreamDecoderErrors::ProviderFailed);
        CHECK(failing.State() == AudioStreamDecoderState::Failed);

        FakeProvider throwingProvider;
        throwingProvider.behavior = Behavior::ThrowNonStd;
        auto throwingResult = AudioStreamDecoder::Create(Spec(), Provider(throwingProvider));
        REQUIRE(throwingResult.HasValue());
        auto throwing = std::move(throwingResult).Value();
        RequireCode(throwing.Seek(2), AudioStreamDecoderErrors::ProviderFailed);
        CHECK(throwing.CursorFrame() == 0);
        CHECK(throwing.State() == AudioStreamDecoderState::Failed);
    }

    TEST_CASE("Runtime stream decoder handles empty media and cancellation before provider entry", "[unit][audio][stream_decoder]") {
        FakeProvider emptyProvider;
        auto emptySpec = Spec();
        emptySpec.frameCount = 0;
        auto emptyResult = AudioStreamDecoder::Create(emptySpec, Provider(emptyProvider));
        REQUIRE(emptyResult.HasValue());
        auto empty = std::move(emptyResult).Value();
        std::array<AudioSample, 4> output{};
        std::array<std::byte, 16> scratch{};
        const auto terminal = empty.Decode(output, scratch, 2);
        REQUIRE(terminal.HasValue());
        CHECK(terminal.Value().frames == 0);
        CHECK(terminal.Value().endOfStream);
        CHECK(emptyProvider.decodeCalls == 0);

        FakeProvider cancelledProvider;
        auto created = AudioStreamDecoder::Create(Spec(), Provider(cancelledProvider));
        REQUIRE(created.HasValue());
        auto cancelled = std::move(created).Value();
        cancelled.Cancel();
        RequireCode(cancelled.Decode(output, scratch, 2), AudioStreamDecoderErrors::Cancelled);
        CHECK(cancelledProvider.decodeCalls == 0);
        CHECK(cancelled.State() == AudioStreamDecoderState::Cancelled);
    }
}  // namespace Horo::Audio
