#include "Horo/Audio/Internal/Sdl3AudioBackend.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>
#include <vector>

namespace Horo::Audio::Backend {
    namespace {
        /** @brief Creates an independent capture backend for the SDL dummy driver selected by CTest. */
        std::unique_ptr<Sdl3AudioBackend> MakeBackend() {
            const Sdl3AudioBackendConfig config{.owner = AudioRuntimeId::Create(731).Value(), .clockDomain = 4};
            auto backend = CreateSdl3AudioBackend(config);
            REQUIRE(backend.HasValue());
            return std::move(backend).Value();
        }

        /** @brief Pins an enumerated endpoint and its advertised PCM layout without the default route alias. */
        AudioCaptureBufferDescriptor SelectEndpoint(const AudioCaptureDeviceSnapshot &catalog) {
            const auto device = std::ranges::find_if(catalog.devices, [](const auto &entry) {
                return entry.deviceClass != AudioDeviceClass::Virtual && entry.preferredFormat.has_value();
            });
            REQUIRE(device != catalog.devices.end());
            return {.device = device->id,
                    .generation = 1,
                    .clockDomain = 4,
                    .format = *device->preferredFormat,
                    .slots = 2,
                    .maximumFrames = 4096,
                    .budgetBytes = 256 * 1024};
        }

        /** @brief Polls within a fixed test deadline while the actual SDL recording thread supplies PCM. */
        Sdl3CaptureReadout WaitForOverflow(Sdl3AudioBackend &backend, const std::uint64_t generation) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            AudioCapturedBlock ignored;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto read = backend.ReadCapture(generation, ignored, {});
                REQUIRE(read.HasValue());
                if (read.Value().droppedFrames != 0)
                    return read.Value();
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            FAIL("The SDL recording callback did not fill and overflow the bounded ring.");
            return {};
        }

        /** @brief Reads the next complete live block, including a following generation or gap. */
        AudioCapturedBlock WaitForBlock(Sdl3AudioBackend &backend, const std::uint64_t generation, std::span<AudioSample> pcm) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            AudioCapturedBlock block;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto read = backend.ReadCapture(generation, block, pcm);
                REQUIRE(read.HasValue());
                if (read.Value().status == AudioCaptureRead::Copied)
                    return block;
                REQUIRE(read.Value().status == AudioCaptureRead::Empty);
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            FAIL("The SDL recording callback did not publish another complete block.");
            return {};
        }

        /** @brief Checks that live PCM and the source timeline survive a stalled reader. */
        void VerifyOverflow(Sdl3AudioBackend &backend, const AudioCaptureBufferDescriptor &descriptor, std::span<AudioSample> pcm) {
            const auto full = WaitForOverflow(backend, descriptor.generation);
            CHECK(full.status == AudioCaptureRead::InsufficientSpace);
            CHECK(full.droppedFrames > 0);
            const auto channels = descriptor.format.layout.orderedChannels.size();
            AudioCapturedBlock first;
            const auto readFirst = backend.ReadCapture(descriptor.generation, first, pcm);
            REQUIRE(readFirst.HasValue());
            REQUIRE(readFirst.Value().status == AudioCaptureRead::Copied);
            REQUIRE(first.frames > 0);
            CHECK(first.start.clockDomain == descriptor.clockDomain);
            CHECK(first.firstFrame == 0);
            CHECK(first.sequence == 1);
            CHECK(std::ranges::all_of(std::span{pcm}.first(first.frames * channels), [](const AudioSample sample) {
                return sample == 0.0F;
            }));

            AudioCapturedBlock second;
            const auto readSecond = backend.ReadCapture(descriptor.generation, second, pcm);
            REQUIRE(readSecond.HasValue());
            REQUIRE(readSecond.Value().status == AudioCaptureRead::Copied);
            CHECK(second.firstFrame == first.frames);
            CHECK(second.sequence == 2);
            CHECK(second.start.nanoseconds - first.start.nanoseconds ==
                  std::uint64_t{first.frames} * 1'000'000'000 / descriptor.format.sampleRate);
            const auto afterGap = WaitForBlock(backend, descriptor.generation, pcm);
            CHECK(afterGap.sequence > second.sequence + 1);
            CHECK(afterGap.firstFrame == second.firstFrame + second.frames + afterGap.droppedFramesBefore);
            CHECK(afterGap.start.nanoseconds - first.start.nanoseconds ==
                  afterGap.firstFrame * 1'000'000'000 / descriptor.format.sampleRate);
        }

        TEST_CASE("SDL recording callbacks feed bounded PCM only under an exact grant", "[audio][sdl3][capture]") {
            auto backend = MakeBackend();
            const auto discovery = backend->DiscoverCapture();
            REQUIRE(discovery.HasValue());
            const auto &catalog = discovery.Value().snapshot;
            REQUIRE(catalog.permission == AudioCapturePermission::NotRequired);
            const auto descriptor = SelectEndpoint(catalog);
            Sdl3CaptureGrant grant{descriptor.device, catalog.revision, descriptor.generation, 0};
            const auto denied = backend->StartCapture(descriptor, grant);
            REQUIRE(denied.HasError());
            CHECK(denied.ErrorValue().code.Value() == AudioErrors::CapturePermissionDenied.code.Value());
            grant.authorityRevision = 1;
            --grant.discoveryRevision;
            CHECK(backend->StartCapture(descriptor, grant).HasError());
            grant.discoveryRevision = catalog.revision;
            REQUIRE(backend->StartCapture(descriptor, grant).HasValue());
            CHECK(backend->StartCapture(descriptor, grant).HasError());

            std::vector pcm(descriptor.maximumFrames * descriptor.format.layout.orderedChannels.size(), 1.0F);
            VerifyOverflow(*backend, descriptor, pcm);
            REQUIRE(backend->StopCapture(descriptor.generation).HasValue());
            REQUIRE(backend->StopCapture(descriptor.generation).HasValue());
            AudioCapturedBlock stopped;
            CHECK(backend->ReadCapture(descriptor.generation, stopped, pcm).HasError());
            CHECK(backend->StartCapture(descriptor, grant).HasError());
            auto replacement = descriptor;
            ++replacement.generation;
            grant.generation = replacement.generation;
            ++grant.authorityRevision;
            REQUIRE(backend->StartCapture(replacement, grant).HasValue());
            CHECK(backend->StopCapture(descriptor.generation).HasError());
            const auto restarted = WaitForBlock(*backend, replacement.generation, pcm);
            CHECK(restarted.firstFrame == 0);
            CHECK(restarted.sequence == 1);
            CHECK(restarted.droppedFramesBefore == 0);
            REQUIRE(backend->StopCapture(replacement.generation).HasValue());
        }
    }  // namespace
}  // namespace Horo::Audio::Backend
