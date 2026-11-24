#include "Horo/Application/Internal/AudioRecording.h"
#include "Horo/Audio/AudioErrors.h"
#include "OwnedTestDirectory.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Application::Internal {
    namespace {
        using namespace Audio;
        using namespace Audio::Backend;

        /** @brief Owns a temporary project, ordinary asset writer, and simulated capture source. */
        struct RecordingFixture final {
            Tests::OwnedTestDirectory temporary{"audio-recording"};
            const std::filesystem::path root = std::filesystem::canonical(temporary.Path());
            NativeDurableFileSystem files;
            Editor::ProjectMutationCoordinator mutations{files};
            Assets::AssetRegistry registry;
            ProjectAssetImportCommitter publisher{mutations, files, &registry};
            const AudioRecordingDestination destination{"record", root, ProjectPath::Parse("assets/take.wav").Value(),
                                                        Assets::AssetId::FromBytes({1})};
            AudioCaptureBufferDescriptor source{.device = {AudioRuntimeId::Create(5).Value(), 2, 3},
                                                .generation = 1,
                                                .clockDomain = 7,
                                                .format = {48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)},
                                                .slots = 2,
                                                .maximumFrames = 2,
                                                .budgetBytes = 4096};

            /** @brief Uses the actual prepared PCM queue without opening a real microphone. */
            AudioCaptureBuffer Buffer() const {
                auto created = AudioCaptureBuffer::Create(source);
                REQUIRE(created.HasValue());
                return std::move(created).Value();
            }

            /** @brief Prepares a short recording with enough space for the fixture timeline. */
            AudioRecording Recording() const {
                auto prepared = AudioRecording::Prepare(source, 16);
                REQUIRE(prepared.HasValue());
                return std::move(prepared).Value();
            }
        };

        constexpr std::array<AudioSample, 2> StereoFrame{0.25F, -0.5F};

        TEST_CASE("A stopped recording drains its tail and publishes registered WAVE and identity files", "[audio][recording]") {
            RecordingFixture fixture;
            auto buffer = fixture.Buffer();
            auto recording = fixture.Recording();
            REQUIRE(buffer.TryWrite({7, 100}, 0, 1, StereoFrame) == AudioCaptureWrite::Accepted);
            AudioCapturedBlock prefix;
            std::array<AudioSample, 2> scratch{};
            REQUIRE(buffer.TryRead(prefix, scratch) == AudioCaptureRead::Copied);
            REQUIRE(recording.Append(fixture.source, prefix, scratch).HasValue());
            REQUIRE(buffer.TryWrite({7, 21'000}, 1, 1, StereoFrame) == AudioCaptureWrite::Accepted);

            const auto published = recording.Publish(std::move(buffer), fixture.destination, fixture.publisher, {});
            INFO((published.HasError() ? published.ErrorValue().message : ""));
            REQUIRE(published.HasValue());
            CHECK(published.Value().frames == 2);
            CHECK(published.Value().sampleRate == 48'000);
            CHECK(published.Value().durationNanoseconds == 41'666);
            CHECK(fixture.publisher.LastReceipt().status == ProjectAssetImportStatus::Published);
            const auto snapshot = fixture.registry.Snapshot();
            const auto *record = snapshot.Find(fixture.destination.id);
            REQUIRE(record);
            CHECK(record->type.Value() == "core.audio");
            CHECK(record->sourcePath.String() == "assets/take.wav");
            CHECK(record->metadataPath.String() == "assets/take.wav.horo");
        }

        TEST_CASE("Recording cancellation and capture loss discard the complete unpublished take", "[audio][recording]") {
            RecordingFixture fixture;
            auto buffer = fixture.Buffer();
            auto recording = fixture.Recording();
            CancellationSource cancellation;
            REQUIRE(buffer.TryWrite({7, 0}, 0, 1, StereoFrame) == AudioCaptureWrite::Accepted);
            SECTION("canceled by owner") {
                cancellation.RequestCancellation();
            }
            SECTION("lost the final block before stop") {
                REQUIRE(buffer.TryWrite({7, 21'000}, 1, 1, StereoFrame) == AudioCaptureWrite::Accepted);
                REQUIRE(buffer.TryWrite({7, 42'000}, 2, 1, StereoFrame) == AudioCaptureWrite::Full);
            }
            SECTION("producer was cancelled instead of stopped normally") {
                buffer.Cancel();
            }
            const auto discarded = recording.Publish(std::move(buffer), fixture.destination, fixture.publisher, cancellation.Token());
            CHECK(discarded.HasError());
            CHECK_FALSE(std::filesystem::exists(fixture.root / "assets/take.wav"));
            CHECK_FALSE(std::filesystem::exists(fixture.root / "assets/take.wav.horo"));
            CHECK_FALSE(fixture.registry.Snapshot().Find(fixture.destination.id));
        }

        TEST_CASE("A stale capture block discards the private recording and cannot later publish", "[audio][recording]") {
            RecordingFixture fixture;
            auto recording = fixture.Recording();
            auto stale = fixture.source;
            ++stale.generation;
            const AudioCapturedBlock block{.start = {7, 0}, .sequence = 1, .frames = 1};
            CHECK(recording.Append(stale, block, StereoFrame).HasError());
            auto buffer = fixture.Buffer();
            REQUIRE(buffer.TryWrite({7, 0}, 0, 1, StereoFrame) == AudioCaptureWrite::Accepted);
            const auto discarded = recording.Publish(std::move(buffer), fixture.destination, fixture.publisher, {});
            CHECK(discarded.HasError());
            CHECK_FALSE(std::filesystem::exists(fixture.root / "assets/take.wav"));
        }
    }  // namespace
}  // namespace Horo::Application::Internal
