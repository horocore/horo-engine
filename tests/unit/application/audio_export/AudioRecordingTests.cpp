#include "../../../../apps/HoroEditor/app/EditorMicrophoneRecording.h"
#include "Horo/Application/Internal/AudioRecording.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Audio/AudioErrors.h"
#include "Horo/Foundation/ImmutableFileStorage.h"
#include "Horo/Foundation/Paths.h"
#include "NativePublicationFiles.h"
#include "OwnedTestDirectory.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace Horo::Application::Internal {
    namespace {
        using namespace Audio;
        using namespace Audio::Backend;

        /** @brief Injects storage failures and cancellation at recording publication boundaries. */
        class RecordingStorage final : public TestSupport::NativePublicationFiles {
        public:
            const ErrorCodeDescriptor *secondWriteFailure{};
            CancellationSource *cancelDuringStageCleanup{};
            std::optional<std::uint64_t> freeBytes;

            Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
                return freeBytes ? Result<std::uint64_t>::Success(*freeBytes) : native.AvailableBytes(path);
            }

            Result<void> WritePrivateDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes, bool &created,
                                             const std::filesystem::perms permissions) override {
                if (secondWriteFailure && ++writes_ == 2)
                    return Result<void>::Failure(MakeError(*secondWriteFailure));
                return native.WritePrivateDurable(path, bytes, created, permissions);
            }

            Result<void> RemoveDurable(const std::filesystem::path &path) override {
                auto removed = native.RemoveDurable(path);
                if (removed.HasValue() && cancelDuringStageCleanup && path.filename().string().starts_with(".horo-import-"))
                    cancelDuringStageCleanup->RequestCancellation();
                return removed;
            }

        private:
            unsigned writes_{};
        };

        constexpr std::array<AudioSample, 2> StereoFrame{0.25F, -0.5F};

        /** @brief Owns a temporary project, ordinary asset writer, and simulated capture source. */
        struct RecordingFixture final {
            Tests::OwnedTestDirectory temporary{"audio-recording"};
            const std::filesystem::path root = std::filesystem::canonical(temporary.Path());
            RecordingStorage files;
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

            /** @brief Starts a real editor save with one unread frame from the detached input. */
            void StartEditorSave(Editor::EditorRecordingPublication &publication) const {
                auto take = Editor::EditorRecordingTake::Prepare(source, destination);
                REQUIRE(take.HasValue());
                auto buffer = Buffer();
                REQUIRE(buffer.TryWrite({7, 0}, 0, 1, StereoFrame) == AudioCaptureWrite::Accepted);
                REQUIRE(publication.Start(std::move(take).Value(), std::move(buffer)));
            }
        };

        /** @brief Waits for the real background publication to reach its atomic index write or to finish. */
        template <class Condition> bool WaitFor(Condition complete) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!complete()) {
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            return true;
        }

        /** @brief Pumps the completed worker receipt through the editor adoption boundary. */
        Result<Editor::EditorPublishedRecording> WaitForSave(Editor::EditorRecordingPublication &publication) {
            std::optional<Result<Editor::EditorPublishedRecording>> result;
            REQUIRE(WaitFor([&] {
                result = publication.Poll();
                return result.has_value();
            }));
            return std::move(*result);
        }

        TEST_CASE("The editor adopts a background recording only while its project still owns the result", "[audio][recording][editor]") {
            RecordingFixture fixture;
            Editor::EditorRecordingPublication publication{fixture.mutations, fixture.files, fixture.registry};
            fixture.StartEditorSave(publication);

            bool cancelled = false;
            SECTION("the project stays open") {}
            SECTION("the project stops owning the result") {
                publication.Detach();
                cancelled = true;
            }

            const auto result = WaitForSave(publication);
            const auto snapshot = fixture.registry.Snapshot();
            if (cancelled) {
                CHECK(result.HasError());
                CHECK_FALSE(snapshot.Find(fixture.destination.id));
            } else {
                REQUIRE(result.HasValue());
                CHECK(result.Value().id == fixture.destination.id);
                CHECK(result.Value().path == "assets/take.wav");
                REQUIRE(snapshot.Find(fixture.destination.id));
                CHECK(std::filesystem::exists(fixture.root / result.Value().path));
            }
        }

        TEST_CASE("The originating project can adopt a recording after its durable commit", "[audio][recording][editor]") {
            RecordingFixture fixture;
            Editor::EditorRecordingPublication publication{fixture.mutations, fixture.files, fixture.registry};
            fixture.StartEditorSave(publication);

            REQUIRE(WaitFor([&] { return std::filesystem::exists(fixture.root / ProjectLayout::AssetIndexPath); }));
            SECTION("a late cancel retains the completed save") {
                publication.Cancel();
                const auto result = WaitForSave(publication);
                REQUIRE(result.HasValue());
                CHECK(result.Value().id == fixture.destination.id);
                CHECK(fixture.registry.Snapshot().Find(fixture.destination.id));
            }
            SECTION("a retired project leaves its saved file on disk without altering the live registry") {
                publication.Detach();
                const auto result = WaitForSave(publication);
                REQUIRE(result.HasError());
                CHECK(result.ErrorValue().code.Value() == AudioErrors::OperationCancelled.code.Value());
                CHECK_FALSE(fixture.registry.Snapshot().Find(fixture.destination.id));
                CHECK(std::filesystem::exists(fixture.root / fixture.destination.path.String()));
            }
        }

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

            const auto published = recording.Publish(std::move(buffer), fixture.destination, fixture.publisher, fixture.files, {});
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

        TEST_CASE("A recording write failure withdraws both the staged recording and its identity", "[audio][recording]") {
            RecordingFixture fixture;
            fixture.files.secondWriteFailure = &FileStorageErrors::IoFailed;
            auto buffer = fixture.Buffer();
            REQUIRE(buffer.TryWrite({7, 0}, 0, 1, StereoFrame) == AudioCaptureWrite::Accepted);
            auto recording = fixture.Recording();
            const auto result = recording.Publish(std::move(buffer), fixture.destination, fixture.publisher, fixture.files, {});
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == FileStorageErrors::IoFailed.code.Value());
            CHECK(std::filesystem::is_empty(fixture.root / "assets"));
            CHECK_FALSE(fixture.registry.Snapshot().Find(fixture.destination.id));
        }

        TEST_CASE("Recording reports insufficient space without staging an asset", "[audio][recording]") {
            RecordingFixture fixture;
            fixture.files.freeBytes = 0;
            auto buffer = fixture.Buffer();
            REQUIRE(buffer.TryWrite({7, 0}, 0, 1, StereoFrame) == AudioCaptureWrite::Accepted);
            auto recording = fixture.Recording();

            const auto result = recording.Publish(std::move(buffer), fixture.destination, fixture.publisher, fixture.files, {});
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == FileStorageErrors::Capacity.code.Value());
            CHECK_FALSE(std::filesystem::exists(fixture.root / "assets"));
            CHECK_FALSE(fixture.registry.Snapshot().Find(fixture.destination.id));
        }

        TEST_CASE("Editor recording can retry after staging runs out of disk space", "[audio][recording][editor]") {
            RecordingFixture fixture;
            fixture.files.freeBytes = 32U << 20U;
            fixture.files.secondWriteFailure = &FileStorageErrors::Capacity;
            Editor::EditorRecordingPublication publication{fixture.mutations, fixture.files, fixture.registry};
            fixture.StartEditorSave(publication);
            const auto full = WaitForSave(publication);
            REQUIRE(full.HasError());
            CHECK(full.ErrorValue().code.Value() == FileStorageErrors::Capacity.code.Value());
            CHECK_FALSE(publication.Busy());
            CHECK(std::filesystem::is_empty(fixture.root / "assets"));
            CHECK_FALSE(fixture.registry.Snapshot().Find(fixture.destination.id));

            fixture.files.secondWriteFailure = nullptr;
            fixture.StartEditorSave(publication);
            REQUIRE(WaitForSave(publication).HasValue());
            CHECK(fixture.registry.Snapshot().Find(fixture.destination.id));
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
            const auto discarded =
                recording.Publish(std::move(buffer), fixture.destination, fixture.publisher, fixture.files, cancellation.Token());
            CHECK(discarded.HasError());
            CHECK_FALSE(std::filesystem::exists(fixture.root / "assets/take.wav"));
            CHECK_FALSE(std::filesystem::exists(fixture.root / "assets/take.wav.horo"));
            CHECK_FALSE(fixture.registry.Snapshot().Find(fixture.destination.id));
        }

        TEST_CASE("Recording cancellation after source promotion rolls back before registry publication", "[audio][recording]") {
            RecordingFixture fixture;
            CancellationSource cancellation;
            fixture.files.cancelDuringStageCleanup = &cancellation;
            auto buffer = fixture.Buffer();
            REQUIRE(buffer.TryWrite({7, 0}, 0, 1, StereoFrame) == AudioCaptureWrite::Accepted);
            auto recording = fixture.Recording();

            const auto result =
                recording.Publish(std::move(buffer), fixture.destination, fixture.publisher, fixture.files, cancellation.Token());
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == Assets::AssetSourceErrors::Cancelled.code.Value());
            CHECK(fixture.publisher.LastReceipt().status == ProjectAssetImportStatus::RolledBack);
            CHECK(fixture.publisher.LastReceipt().rolledBackFiles == 2);
            CHECK(std::filesystem::is_empty(fixture.root / "assets"));
            CHECK_FALSE(fixture.registry.Snapshot().Find(fixture.destination.id));
        }

        TEST_CASE("An invalid capture block discards the private recording and cannot later publish", "[audio][recording]") {
            RecordingFixture fixture;
            auto recording = fixture.Recording();
            auto source = fixture.source;
            AudioCapturedBlock block{.start = {7, 0}, .sequence = 1, .frames = 1};
            SECTION("stale device generation") {
                ++source.generation;
            }
            SECTION("capture source reports a discontinuity") {
                block.missingSourceFramesBefore = 1;
            }
            CHECK(recording.Append(source, block, StereoFrame).HasError());
            auto buffer = fixture.Buffer();
            REQUIRE(buffer.TryWrite({7, 0}, 0, 1, StereoFrame) == AudioCaptureWrite::Accepted);
            const auto discarded = recording.Publish(std::move(buffer), fixture.destination, fixture.publisher, fixture.files, {});
            CHECK(discarded.HasError());
            CHECK_FALSE(std::filesystem::exists(fixture.root / "assets/take.wav"));
        }
    }  // namespace
}  // namespace Horo::Application::Internal
