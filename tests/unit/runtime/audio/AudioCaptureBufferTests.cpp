#include "AllocationProbe.h"
#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/Internal/AudioCaptureBuffer.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>

namespace Horo::Audio::Backend {
    namespace {
        /** @brief Prepares a small stereo ring with a distinct capture and clock generation. */
        AudioCaptureBufferDescriptor Descriptor() {
            return {.device = {AudioRuntimeId::Create(7).Value(), 14, 3},
                    .generation = 9,
                    .clockDomain = 5,
                    .format = {48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)},
                    .slots = 2,
                    .maximumFrames = 2,
                    .budgetBytes = 4096};
        }

        /** @brief Acquires the actual bounded ring before starting either endpoint. */
        AudioCaptureBuffer MakeBuffer() {
            auto result = AudioCaptureBuffer::Create(Descriptor());
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        TEST_CASE("Capture overflow drops the newest whole block and preserves the next source timestamp", "[audio][capture]") {
            auto buffer = MakeBuffer();
            const std::array<AudioSample, 4> stereo{0.5F, -0.25F, 0.25F, -0.5F};
            REQUIRE(buffer.TryWrite({5, 0}, 0, 2, stereo) == AudioCaptureWrite::Accepted);
            REQUIRE(buffer.TryWrite({5, 42'000}, 2, 2, stereo) == AudioCaptureWrite::Accepted);
            REQUIRE(buffer.TryWrite({5, 84'000}, 4, 2, stereo) == AudioCaptureWrite::Full);
            CHECK(buffer.DroppedFrames() == 2);

            AudioCapturedBlock block{.sequence = 99};
            std::array<AudioSample, 2> shortOutput{};
            CHECK(buffer.TryRead(block, shortOutput) == AudioCaptureRead::InsufficientSpace);
            CHECK(block.sequence == 99);
            CHECK(shortOutput[0] == 0.0F);
            std::array<AudioSample, 4> output{};
            REQUIRE(buffer.TryRead(block, output) == AudioCaptureRead::Copied);
            CHECK(block.sequence == 1);
            CHECK(block.droppedFramesBefore == 0);
            CHECK(output == stereo);
            REQUIRE(buffer.TryWrite({5, 126'000}, 6, 2, stereo) == AudioCaptureWrite::Accepted);
            REQUIRE(buffer.TryRead(block, output) == AudioCaptureRead::Copied);
            CHECK(block.sequence == 2);
            REQUIRE(buffer.TryRead(block, output) == AudioCaptureRead::Copied);
            CHECK(block.sequence == 4);
            CHECK(block.firstFrame == 6);
            CHECK(block.frames == 2);
            CHECK(block.start.nanoseconds == 126'000);
            CHECK(block.droppedFramesBefore == 2);
            CHECK(buffer.TryRead(block, output) == AudioCaptureRead::Empty);
        }

        TEST_CASE("Capture rejects bad chronology and cancellation fences its admitted generation", "[audio][capture]") {
            auto buffer = MakeBuffer();
            REQUIRE(buffer.Descriptor());
            CHECK(buffer.Descriptor()->device == Descriptor().device);
            CHECK(buffer.Descriptor()->generation == Descriptor().generation);
            const std::array<AudioSample, 2> stereo{0.125F, -0.125F};
            CHECK(buffer.TryWrite({4, 0}, 0, 1, stereo) == AudioCaptureWrite::Invalid);
            CHECK(buffer.TryWrite({5, 0}, std::numeric_limits<std::uint64_t>::max(), 1, stereo) == AudioCaptureWrite::Invalid);
            CHECK(buffer.TryWrite({5, 0}, 0, 2, stereo) == AudioCaptureWrite::Invalid);
            REQUIRE(buffer.TryWrite({5, 20}, 0, 1, stereo) == AudioCaptureWrite::Accepted);
            CHECK(buffer.TryWrite({5, 19}, 1, 1, stereo) == AudioCaptureWrite::Invalid);
            CHECK(buffer.TryWrite({5, 21}, 0, 1, stereo) == AudioCaptureWrite::Invalid);
            REQUIRE(buffer.TryWrite({5, 21}, 1, 1, stereo) == AudioCaptureWrite::Accepted);
            buffer.Cancel();
            buffer.Cancel();
            AudioCapturedBlock block{};
            std::array<AudioSample, 2> output{};
            CHECK(buffer.TryRead(block, output) == AudioCaptureRead::Cancelled);
            CHECK(buffer.TryWrite({5, 50}, 2, 1, stereo) == AudioCaptureWrite::Cancelled);
            CHECK(block.sequence == 0);
            CHECK(buffer.DroppedFrames() == 0);
        }

        TEST_CASE("Capture preparation rejects over-budget or invalid fixed storage", "[audio][capture]") {
            auto spec = Descriptor();
            spec.budgetBytes = 1;
            const auto budget = AudioCaptureBuffer::Create(spec);
            REQUIRE(budget.HasError());
            CHECK(budget.ErrorValue().code.Value() == AudioErrors::MemoryBudgetExceeded.code.Value());
            spec.slots = 3;
            const auto shape = AudioCaptureBuffer::Create(spec);
            REQUIRE(shape.HasError());
            CHECK(shape.ErrorValue().code.Value() == AudioErrors::MemoryInvalid.code.Value());
        }

        TEST_CASE("Capture publication overflow and consumer copy use no heap allocation after preparation", "[audio][capture]") {
            auto buffer = MakeBuffer();
            const std::array<AudioSample, 2> stereo{0.125F, -0.125F};
            std::array<AudioSample, 2> output{};
            AudioCapturedBlock block;
            std::array<AudioCaptureWrite, 3> writes;
            AudioCaptureRead read{};
            Tests::AllocationProbe::Measurement measured;
            {
                Tests::AllocationProbe::ScopedMeasurement probe;
                writes[0] = buffer.TryWrite({5, 0}, 0, 1, stereo);
                writes[1] = buffer.TryWrite({5, 20'833}, 1, 1, stereo);
                writes[2] = buffer.TryWrite({5, 41'666}, 2, 1, stereo);
                read = buffer.TryRead(block, output);
                measured = probe.Snapshot();
            }
            CHECK(writes[0] == AudioCaptureWrite::Accepted);
            CHECK(writes[1] == AudioCaptureWrite::Accepted);
            CHECK(writes[2] == AudioCaptureWrite::Full);
            CHECK(read == AudioCaptureRead::Copied);
            CHECK(measured.requests == 0);
        }

        TEST_CASE("Concurrent capture producer terminates and each observed PCM block retains its exact sequence", "[audio][capture]") {
            auto buffer = MakeBuffer();
            constexpr std::uint64_t frames = 8192;
            std::atomic<bool> finished{};
            std::jthread producer([&buffer, &finished] {
                for (std::uint64_t index = 0; index < frames; ++index) {
                    const std::array<AudioSample, 2> pcm{static_cast<float>(index), -static_cast<float>(index)};
                    static_cast<void>(buffer.TryWrite({5, index * 20'833}, index, 1, pcm));
                }
                finished.store(true);
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            std::uint64_t consumed{};
            std::uint64_t previous{};
            bool exact = true;
            while (std::chrono::steady_clock::now() < deadline) {
                const bool done = finished.load();
                AudioCapturedBlock block;
                std::array<AudioSample, 2> pcm{};
                if (buffer.TryRead(block, pcm) == AudioCaptureRead::Copied) {
                    exact = exact && block.sequence > previous && block.sequence == block.firstFrame + 1 &&
                            block.start.nanoseconds == block.firstFrame * 20'833 && pcm[0] == static_cast<float>(block.firstFrame) &&
                            pcm[1] == -static_cast<float>(block.firstFrame);
                    previous = block.sequence;
                    ++consumed;
                } else if (done) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
            producer.join();
            CHECK(exact);
            CHECK(consumed > 0);
            CHECK(consumed + buffer.DroppedFrames() == frames);
        }
    }  // namespace
}  // namespace Horo::Audio::Backend
