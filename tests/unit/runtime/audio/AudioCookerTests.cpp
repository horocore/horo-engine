#include "Horo/Assets/AssetArchive.h"
#include "Horo/Assets/AssetCookService.h"
#include "Horo/Audio/AudioCooker.h"
#include "Horo/Audio/AudioErrors.h"
#include "Horo/Foundation/JobSystem.h"
#include "assets/AssetCookPublicationFixture.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace Horo::Audio {
    namespace {
        void Append16(std::vector<std::uint8_t> &bytes, const std::uint16_t value) {
            bytes.push_back(static_cast<std::uint8_t>(value));
            bytes.push_back(static_cast<std::uint8_t>(value >> 8));
        }

        void Append32(std::vector<std::uint8_t> &bytes, const std::uint32_t value) {
            for (unsigned shift = 0; shift < 32; shift += 8)
                bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }

        void AppendFour(std::vector<std::uint8_t> &bytes, const char (&value)[5]) {
            bytes.insert(bytes.end(), value, value + 4);
        }

        std::vector<std::uint8_t> WaveFixture() {
            constexpr std::array<std::int16_t, 8> samples{0, 16'384, 32'767, -32'768, -16'384, 8'192, 0, -8'192};
            std::vector<std::uint8_t> bytes;
            AppendFour(bytes, "RIFF");
            Append32(bytes, 36 + samples.size() * sizeof(std::int16_t));
            AppendFour(bytes, "WAVE");
            AppendFour(bytes, "fmt ");
            Append32(bytes, 16);
            Append16(bytes, 1);
            Append16(bytes, 2);
            Append32(bytes, 48'000);
            Append32(bytes, 48'000 * 4);
            Append16(bytes, 4);
            Append16(bytes, 16);
            AppendFour(bytes, "data");
            Append32(bytes, samples.size() * sizeof(std::int16_t));
            for (const auto sample : samples)
                Append16(bytes, static_cast<std::uint16_t>(sample));
            return bytes;
        }

        AssetCookTargetId Target(const std::string_view text) {
            auto target = AssetCookTargetId::Parse(text);
            REQUIRE(target.HasValue());
            return target.Value();
        }

        AudioCookToolchain Toolchain(const std::string_view text = "miniaudio-pinned-1") {
            return {std::string(text)};
        }

        struct TemporaryCookRoot final {
            std::filesystem::path path = std::filesystem::temp_directory_path() /
                                         ("horo_audio_cook_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

            TemporaryCookRoot() {
                std::filesystem::create_directories(path / "assets");
            }

            TemporaryCookRoot(const TemporaryCookRoot &) = delete;
            TemporaryCookRoot &operator=(const TemporaryCookRoot &) = delete;

            ~TemporaryCookRoot() {
                std::error_code error;
                std::filesystem::remove_all(path, error);
            }
        };

        template <typename Value> bool HasCode(const Result<Value> &result, const ErrorCodeDescriptor &descriptor) {
            return result.HasError() && result.ErrorValue().code.Value() == descriptor.code.Value();
        }

        void PrepareCookSource(const TemporaryCookRoot &root, Assets::AssetRegistry &registry, const Assets::AssetId &assetId,
                               const Assets::AssetTypeId &type) {
            const auto source = WaveFixture();
            std::ofstream file(root.path / "assets/clip.wav", std::ios::binary);
            file.write(reinterpret_cast<const char *>(source.data()), static_cast<std::streamsize>(source.size()));
            REQUIRE(file.good());
            file.close();
            const auto sourcePath = ProjectPath::Parse("assets/clip.wav");
            const auto metadataPath = ProjectPath::Parse("assets/clip.wav.horo");
            REQUIRE(sourcePath.HasValue());
            REQUIRE(metadataPath.HasValue());
            REQUIRE(registry
                        .Publish({Assets::AssetRecord{.id = assetId,
                                                      .type = type,
                                                      .sourcePath = sourcePath.Value(),
                                                      .metadataPath = metadataPath.Value()}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
        }

        Result<Assets::AssetCookReport> RunAudioCook(const Assets::AssetCookRequest &request, const Assets::AssetTypeId &type,
                                                     const AssetCookTargetId &target, JobSystem &jobs, const AudioCookProfile &profile) {
            Assets::CookerCatalog catalog;
            auto contribution = MakeAudioCookerContribution(type, profile, target, Toolchain());
            REQUIRE(contribution.HasValue());
            REQUIRE(catalog.Register(std::move(contribution).Value()).HasValue());
            auto snapshot = catalog.Publish();
            REQUIRE(snapshot.HasValue());
            Assets::AssetCookService service(jobs, snapshot.Value());
            return service.Cook(request, CancellationToken{});
        }

        /** @brief Real cooked generation shared by filesystem and release-package integration cases. */
        struct CookedStreamFixture final {
            const AssetCookTargetId target = Target("linux-desktop");
            const Assets::AssetTypeId type = Assets::AssetTypeId::Parse("audio.clip").Value();
            const Assets::AssetId asset = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000544").Value();
            static constexpr std::size_t artifactLimit = 4'096;
            AudioProcessingFormat format;
            std::vector<std::uint8_t> encoded;
            TemporaryCookRoot root;

            CookedStreamFixture() {
                AudioCookProfile profile;
                profile.defaults.streamThresholdFrames = 2;
                profile.defaults.streamChunkFrames = 2;
                auto cooked = CookAudioSource(WaveFixture(), profile, target, Toolchain());
                REQUIRE(cooked.HasValue());
                Assets::AssetCookArtifact artifact;
                artifact.id = asset;
                artifact.type = type;
                artifact.target = target;
                artifact.sourceDigest = cooked.Value().manifest.sourceDigest;
                artifact.payload = cooked.Value().bytes;
                artifact.payloadDigest = ComputeSha256(std::as_bytes(std::span(artifact.payload)));
                artifact.cacheKeyDigest = artifact.payloadDigest;
                auto encoding = Assets::EncodeCookedArtifact(artifact);
                REQUIRE(encoding.HasValue());
                encoded = std::move(encoding).Value();
                format = cooked.Value().manifest.format;
                REQUIRE(encoded.size() <= artifactLimit);
            }

            std::shared_ptr<const Assets::IAssetProvider> Filesystem(const std::filesystem::path &directory,
                                                                     const std::size_t providerLimit = artifactLimit) const {
                std::filesystem::create_directories(directory);
                std::ofstream file(directory / (asset.ToString() + ".cooked"), std::ios::binary);
                file.write(reinterpret_cast<const char *>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
                REQUIRE(file.good());
                file.close();
                return std::make_shared<Assets::FilesystemAssetProvider>(directory, Assets::AssetProviderLimits{providerLimit});
            }

            std::shared_ptr<const Assets::IAssetProvider> Archive() const {
                const auto chunk = Assets::AssetChunkId::Parse("core").Value();
                auto plan = Assets::AssetChunkPlan::Create(std::array{Assets::AssetChunkDefinition{.id = chunk, .assets = {asset}}});
                REQUIRE(plan.HasValue());
                auto archive = Assets::BuildAssetArchive(plan.Value(), target, std::array{Assets::AssetArchiveInput{asset, encoded}},
                                                         {.maximumAssetBytes = artifactLimit});
                REQUIRE(archive.HasValue());
                auto opened = Assets::AssetArchiveProvider::Open(archive.Value(), target, {.maximumAssetBytes = artifactLimit});
                REQUIRE(opened.HasValue());
                return std::make_shared<Assets::AssetArchiveProvider>(std::move(opened).Value());
            }

            void CheckOutput(const AudioStreamSnapshot &terminal, const AudioStreamRenderResult output,
                             const std::span<const AudioSample> left, const std::span<const AudioSample> right,
                             const bool expectFailure) const {
                if (expectFailure) {
                    REQUIRE(terminal.failed);
                    REQUIRE(terminal.failure.has_value());
                    CHECK(output.availableFrames == 0);
                    CHECK(output.silentFrames == 8);
                    CHECK(output.stopped);
                } else {
                    REQUIRE(terminal.sourceEnded);
                    CHECK(output.availableFrames == 4);
                    CHECK(output.silentFrames == 4);
                    CHECK(output.ended);
                    CHECK_FALSE(output.stopped);
                    const std::array<AudioSample, 8> expectedLeft{0.0F, 32'767.0F / 32'768.0F, -0.5F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
                    const std::array<AudioSample, 8> expectedRight{0.5F, -1.0F, 0.25F, -0.25F, 0.0F, 0.0F, 0.0F, 0.0F};
                    CHECK(std::equal(left.begin(), left.end(), expectedLeft.begin(), expectedLeft.end()));
                    CHECK(std::equal(right.begin(), right.end(), expectedRight.begin(), expectedRight.end()));
                }
            }

            AudioStreamRequest Request() const {
                AudioStreamRequest request;
                request.asset = asset;
                request.decoder = {AudioCodecIds::Pcm, format, 4, 2, 0, true};
                request.ringFrames = 4;
                request.lookaheadFrames = 4;
                request.maximumPackageBytes = artifactLimit * 3;
                request.underrunPolicy = AudioStreamUnderrunPolicy::StopWithSilence;
                return request;
            }

            void Run(std::shared_ptr<const Assets::IAssetProvider> provider, const bool expectFailure,
                     const std::optional<Error> &expectedError = std::nullopt) const {
                REQUIRE(provider);
                std::weak_ptr<const Assets::IAssetProvider> retained = provider;
                auto source = MakeCookedAudioStreamSource(provider, target, type, artifactLimit);
                REQUIRE(source.HasValue());
                if (!expectFailure) {
                    const auto &binding = source.Value();
                    auto opened = binding.open(binding.context, asset, Request().decoder, artifactLimit * 3, CancellationToken{});
                    REQUIRE(opened.HasValue());
                    auto decoder = std::move(opened).Value();
                    REQUIRE(decoder.Seek(2).HasValue());
                    CHECK(decoder.CursorFrame() == 2);
                }
                JobSystem jobs({.workerCount = 1});
                auto created = AudioStreamingService::Create(jobs, std::move(source).Value());
                REQUIRE(created.HasValue());
                auto service = std::move(created).Value();
                provider.reset();
                CHECK_FALSE(retained.expired());
                const auto admitted = service->Admit(Request());
                REQUIRE(admitted.HasValue());
                auto port = std::move(service->RenderPort(admitted.Value())).Value();
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (!service->Snapshot(admitted.Value()).Value().sourceEnded && !service->Snapshot(admitted.Value()).Value().failed &&
                       std::chrono::steady_clock::now() < deadline) {
                    service->Pump();
                    std::this_thread::yield();
                }
                const auto terminal = service->Snapshot(admitted.Value()).Value();
                alignas(64) std::array<AudioSample, 8> left{};
                alignas(64) std::array<AudioSample, 8> right{};
                std::array<AudioSample *, 2> planes{left.data(), right.data()};
                const auto output = port.Render(planes, 8);
                CheckOutput(terminal, output, left, right, expectFailure);
                if (expectedError) {
                    REQUIRE(terminal.failure.has_value());
                    CHECK(terminal.failure->code.Value() == expectedError->code.Value());
                }
                REQUIRE(service->Retire(admitted.Value()).HasValue());
                service.reset();
                CHECK(retained.expired());
            }
        };
    }  // namespace

    TEST_CASE("Cooked streaming feeds identical PCM from bounded filesystem and release-package providers",
              "[unit][audio][streaming][cook]") {
        CookedStreamFixture fixture;
        SECTION("Pinned filesystem generation with spaces and non-ASCII path") {
            fixture.Run(fixture.Filesystem(fixture.root.path / "cooked generation ö"), false);
        }
        SECTION("Immutable verified assets.horo package") {
            fixture.Run(fixture.Archive(), false);
        }
        SECTION("Missing cooked artifact never falls back to source media") {
            fixture.Run(std::make_shared<Assets::FilesystemAssetProvider>(fixture.root.path,
                                                                          Assets::AssetProviderLimits{CookedStreamFixture::artifactLimit}),
                        true);
        }
        SECTION("Corrupt envelope fails before any PCM publication") {
            std::as_writable_bytes(std::span(fixture.encoded)).back() ^= std::byte{1};
            fixture.Run(fixture.Filesystem(fixture.root.path), true);
        }
        SECTION("Provider rejects an oversized load before allocation") {
            auto provider = fixture.Filesystem(fixture.root.path, 1);
            const auto rejected = provider->Load(fixture.asset, CancellationToken{});
            REQUIRE(rejected.HasError());
            fixture.Run(std::move(provider), true, rejected.ErrorValue());
        }
    }

    TEST_CASE("Audio cook produces identical PCM payload and compatibility manifest for exact inputs", "[unit][audio][cook]") {
        const auto source = WaveFixture();
        const auto target = Target("linux-desktop");
        const AudioCookProfile profile;
        auto first = CookAudioSource(source, profile, target, Toolchain());
        auto second = CookAudioSource(source, profile, target, Toolchain());
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        CHECK(first.Value().bytes == second.Value().bytes);
        CHECK(first.Value().manifest == second.Value().manifest);
        CHECK(first.Value().manifest.frameCount == 4);
        CHECK(first.Value().manifest.sourceContainer == AudioContainerIds::Wave);
        CHECK(first.Value().manifest.sourceCodec == AudioCodecIds::Pcm);
        CHECK(first.Value().manifest.decoderIdentityDigest != Sha256Digest{});
        CHECK(first.Value().manifest.format.sampleRate == 48'000);
        CHECK(first.Value().manifest.format.layout == MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo));
        CHECK(first.Value().manifest.codec == AudioCodecIds::Pcm);
        CHECK(first.Value().manifest.container == AudioContainerIds::HoroCooked);
        CHECK(first.Value().manifest.compression == AudioCookCompression::None);
        CHECK(first.Value().manifest.quality == AudioCookQuality::Float32Exact);
        CHECK(first.Value().manifest.residency == AudioCookResidency::Resident);
        CHECK_FALSE(first.Value().manifest.targetOverride);
        CHECK(first.Value().manifest.streamThresholdFrames == profile.defaults.streamThresholdFrames);
        CHECK(first.Value().manifest.chunkCount == 1);
        CHECK(first.Value().manifest.encoderDelayFrames == 0);
        REQUIRE_FALSE(first.Value().manifest.analysis.waveformLevels.empty());
        CHECK(first.Value().manifest.analysis.residentPcmBytes == first.Value().manifest.payloadByteCount);
        CHECK(first.Value().manifest.analysis.waveformLevels.back().points.front().frameCount == first.Value().manifest.frameCount);
        auto inspected = InspectCookedAudio(first.Value().bytes);
        REQUIRE(inspected.HasValue());
        CHECK(inspected.Value() == first.Value().manifest);
        CHECK(inspected.Value().analysis == first.Value().manifest.analysis);
    }

    TEST_CASE("Audio cook target override resolves streamed chunks without changing source identity", "[unit][audio][cook]") {
        const auto source = WaveFixture();
        const auto target = Target("linux-desktop");
        AudioCookProfile profile;
        auto streamSettings = profile.defaults;
        streamSettings.sampleRate = 48'000;
        streamSettings.layout = MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo);
        streamSettings.streamThresholdFrames = 2;
        streamSettings.streamChunkFrames = 2;
        profile.overrides.push_back({target, streamSettings});
        auto streamed = CookAudioSource(source, profile, target, Toolchain());
        REQUIRE(streamed.HasValue());
        CHECK(streamed.Value().manifest.residency == AudioCookResidency::Streamed);
        CHECK(streamed.Value().manifest.chunkCount == 2);
        CHECK(streamed.Value().manifest.chunkFrames == 2);
        CHECK(streamed.Value().manifest.targetOverride);
        CHECK(streamed.Value().manifest.streamThresholdFrames == 2);
        CHECK(streamed.Value().manifest.format.sampleRate == *streamSettings.sampleRate);
        CHECK(streamed.Value().manifest.format.layout == *streamSettings.layout);
        REQUIRE(InspectCookedAudio(streamed.Value().bytes).HasValue());

        auto other = CookAudioSource(source, profile, Target("macos-desktop"), Toolchain());
        REQUIRE(other.HasValue());
        CHECK(other.Value().manifest.residency == AudioCookResidency::Resident);
        CHECK(other.Value().manifest.configurationDigest != streamed.Value().manifest.configurationDigest);
    }

    TEST_CASE("Audio cook invalidates on source profile and pinned toolchain changes", "[unit][audio][cook]") {
        auto source = WaveFixture();
        const auto target = Target("linux-desktop");
        AudioCookProfile profile;
        const auto baseline = CookAudioSource(source, profile, target, Toolchain());
        REQUIRE(baseline.HasValue());
        source.back() ^= 1;
        const auto changedSource = CookAudioSource(source, profile, target, Toolchain());
        REQUIRE(changedSource.HasValue());
        CHECK(changedSource.Value().bytes != baseline.Value().bytes);
        source.back() ^= 1;
        profile.defaults.residency = AudioCookResidency::Streamed;
        profile.defaults.streamChunkFrames = 2;
        const auto changedProfile = CookAudioSource(source, profile, target, Toolchain());
        REQUIRE(changedProfile.HasValue());
        CHECK(changedProfile.Value().bytes != baseline.Value().bytes);
        const auto changedToolchain = CookAudioSource(source, profile, target, Toolchain("miniaudio-pinned-2"));
        REQUIRE(changedToolchain.HasValue());
        CHECK(changedToolchain.Value().bytes != changedProfile.Value().bytes);
    }

    TEST_CASE("Audio cook fails closed on malformed media, unsupported quality and cancellation", "[unit][audio][cook]") {
        auto source = WaveFixture();
        const auto target = Target("linux-desktop");
        AudioCookProfile profile;
        CHECK(HasCode(CookAudioSource(std::span{source}.first(11), profile, target, Toolchain()), AudioErrors::SourceInvalid));
        profile.defaults.codec = AudioCodecIds::Vorbis;
        CHECK(HasCode(CookAudioSource(source, profile, target, Toolchain()), AudioErrors::CookCombinationUnsupported));
        profile.defaults.codec = AudioCodecIds::Pcm;
        profile.defaults.container = AudioContainerIds::Wave;
        CHECK(HasCode(CookAudioSource(source, profile, target, Toolchain()), AudioErrors::CookCombinationUnsupported));
        profile.defaults.container = AudioContainerIds::HoroCooked;
        profile.defaults.sampleRate = 44'100;
        CHECK(HasCode(CookAudioSource(source, profile, target, Toolchain()), AudioErrors::CookCombinationUnsupported));
        profile.defaults.sampleRate.reset();
        profile.defaults.layout = MakeAudioSpeakerLayout(AudioSpeakerPreset::Mono);
        CHECK(HasCode(CookAudioSource(source, profile, target, Toolchain()), AudioErrors::CookCombinationUnsupported));
        profile.defaults.layout.reset();
        profile.defaults.compression = static_cast<AudioCookCompression>(1);
        CHECK(HasCode(CookAudioSource(source, profile, target, Toolchain()), AudioErrors::CookCombinationUnsupported));
        profile.defaults.compression = AudioCookCompression::None;
        profile.defaults.quality = static_cast<AudioCookQuality>(1);
        CHECK(HasCode(CookAudioSource(source, profile, target, Toolchain()), AudioErrors::CookCombinationUnsupported));
        profile.defaults.quality = AudioCookQuality::Float32Exact;
        profile.defaults.encoderDelayFrames = 1;
        CHECK(HasCode(CookAudioSource(source, profile, target, Toolchain()), AudioErrors::CookCombinationUnsupported));
        profile.defaults.encoderDelayFrames = 0;
        profile.defaults.streamChunkFrames = MaximumAudioStreamChunkFrames + 1;
        CHECK(HasCode(CookAudioSource(source, profile, target, Toolchain()), AudioErrors::CookProfileInvalid));
        profile.defaults.streamChunkFrames = MaximumAudioStreamChunkFrames;
        CHECK(CookAudioSource(source, profile, target, Toolchain()).HasValue());
        profile.overrides.push_back({target, profile.defaults});
        profile.overrides.push_back({target, profile.defaults});
        CHECK(HasCode(CookAudioSource(source, profile, target, Toolchain()), AudioErrors::CookProfileInvalid));
        profile.overrides.clear();
        CHECK(HasCode(CookAudioSource(source, profile, target, Toolchain("")), AudioErrors::CookProfileInvalid));
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        CHECK(HasCode(CookAudioSource(source, profile, target, Toolchain(), cancellation.Token()), AudioErrors::OperationCancelled));
    }

    TEST_CASE("Audio cooked payload rejects header, chunk and media corruption", "[unit][audio][cook]") {
        auto cooked = CookAudioSource(WaveFixture(), AudioCookProfile{}, Target("linux-desktop"), Toolchain());
        REQUIRE(cooked.HasValue());
        auto bytes = cooked.Value().bytes;
        bytes[0] ^= 1;
        CHECK(HasCode(InspectCookedAudio(bytes), AudioErrors::CookPayloadInvalid));
        bytes = cooked.Value().bytes;
        bytes.back() ^= 1;
        CHECK(HasCode(InspectCookedAudio(bytes), AudioErrors::CookPayloadInvalid));
        bytes = cooked.Value().bytes;
        bytes.pop_back();
        CHECK(HasCode(InspectCookedAudio(bytes), AudioErrors::CookPayloadInvalid));
        bytes = cooked.Value().bytes;
        const auto lastAnalysisByte = bytes.size() - cooked.Value().manifest.payloadByteCount - cooked.Value().manifest.chunkCount * 12 - 1;
        bytes[lastAnalysisByte] ^= 1;
        CHECK(HasCode(InspectCookedAudio(bytes), AudioErrors::CookPayloadInvalid));
        bytes = cooked.Value().bytes;
        const auto chunkTableOffset = bytes.size() - cooked.Value().manifest.payloadByteCount - 12;
        bytes[chunkTableOffset] ^= 1;
        CHECK(HasCode(InspectCookedAudio(bytes), AudioErrors::CookPayloadInvalid));
        bytes = cooked.Value().bytes;
        const auto payloadOffset = bytes.size() - cooked.Value().manifest.payloadByteCount;
        bytes[payloadOffset + 0] = 0;
        bytes[payloadOffset + 1] = 0;
        bytes[payloadOffset + 2] = 0xC0;
        bytes[payloadOffset + 3] = 0x7F;
        const auto forgedDigest = ComputeSha256(std::as_bytes(std::span{bytes}.subspan(payloadOffset)));
        const auto digestOffset = payloadOffset - cooked.Value().manifest.chunkCount * 12 - 8 - forgedDigest.bytes.size();
        std::ranges::copy(forgedDigest.bytes, bytes.begin() + static_cast<std::ptrdiff_t>(digestOffset));
        CHECK(HasCode(InspectCookedAudio(bytes), AudioErrors::CookPayloadInvalid));
    }

    TEST_CASE("Audio cooked payload rejects oversized streamed chunk policy bypass", "[unit][audio][cook]") {
        auto cooked = CookAudioSource(WaveFixture(), AudioCookProfile{}, Target("linux-desktop"), Toolchain());
        REQUIRE(cooked.HasValue());
        auto bytes = cooked.Value().bytes;
        REQUIRE(cooked.Value().manifest.frameCount == 4);
        REQUIRE(cooked.Value().manifest.chunkCount == 1);
        // Schema v1: residency is byte 8; stereo chunkFrames starts at byte 55.
        // One four-frame table entry remains valid for a forged 65,537-frame chunk size.
        REQUIRE(bytes.size() > 58);
        bytes[8] = static_cast<std::uint8_t>(AudioCookResidency::Streamed);
        bytes[55] = 0x01;
        bytes[56] = 0x00;
        bytes[57] = 0x01;
        bytes[58] = 0x00;
        CHECK(HasCode(InspectCookedAudio(bytes), AudioErrors::CookPayloadInvalid));
    }

    TEST_CASE("Audio cook contribution exposes effective cache identity to AST", "[unit][audio][cook]") {
        const auto type = Assets::AssetTypeId::Parse("core.audio_clip");
        REQUIRE(type.HasValue());
        const auto target = Target("linux-desktop");
        auto first = MakeAudioCookerContribution(type.Value(), AudioCookProfile{}, target, Toolchain());
        auto second = MakeAudioCookerContribution(type.Value(), AudioCookProfile{}, target, Toolchain("miniaudio-pinned-2"));
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        CHECK(first.Value().strategy->CacheIdentity().settingsDigest != second.Value().strategy->CacheIdentity().settingsDigest);
        Assets::CookerCatalog catalog;
        REQUIRE(catalog.Register(std::move(first).Value()).HasValue());
        auto published = catalog.Publish();
        REQUIRE(published.HasValue());
        const auto *contribution = published.Value()->FindContribution(type.Value(), target);
        REQUIRE(contribution != nullptr);
        CHECK(contribution->contributionId == "horo.audio.pcm-cook");
        CHECK(contribution->strategy->CacheIdentity().settingsSchemaVersion == AudioCookSchemaVersion);
        const auto assetId = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000542");
        REQUIRE(assetId.HasValue());
        const auto source = WaveFixture();
        const Assets::CookSourceView sourceView{
            .id = assetId.Value(),
            .type = type.Value(),
            .target = target,
            .sourceDigest = ComputeSha256(std::as_bytes(std::span{source})),
            .bytes = source,
        };
        auto cooked = contribution->strategy->Cook(sourceView, CancellationToken{});
        REQUIRE(cooked.HasValue());
        CHECK(contribution->strategy->ValidateCookedPayload(sourceView, cooked.Value().payload).HasValue());
        auto corrupted = cooked.Value().payload;
        corrupted.back() ^= 1;
        CHECK(HasCode(contribution->strategy->ValidateCookedPayload(sourceView, corrupted), AudioErrors::CookPayloadInvalid));
    }

    TEST_CASE("AST publishes and reuses exact Audio cook generations", "[integration][audio][cook]") {
        TemporaryCookRoot root;
        const auto assetId = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000542");
        const auto type = Assets::AssetTypeId::Parse("core.audio_clip");
        REQUIRE(assetId.HasValue());
        REQUIRE(type.HasValue());
        Assets::AssetRegistry registry;
        PrepareCookSource(root, registry, assetId.Value(), type.Value());
        const auto target = Target("linux-desktop");
        Assets::AssetCookRequest request{
            .sourceRoot = root.path,
            .cacheRoot = root.path / "cache",
            .cookedRoot = root.path / "cooked",
            .registry = registry.Snapshot(),
            .target = target,
        };
        Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);
        JobSystem jobs;
        const AudioCookProfile baseline;
        auto first = RunAudioCook(request, type.Value(), target, jobs, baseline);
        REQUIRE(first.HasValue());
        CHECK(first.Value().cookedAssets == 1);
        auto cached = RunAudioCook(request, type.Value(), target, jobs, baseline);
        REQUIRE(cached.HasValue());
        CHECK(cached.Value().cacheHits == 1);

        AudioCookProfile streamed;
        streamed.defaults.streamThresholdFrames = 2;
        streamed.defaults.streamChunkFrames = 2;
        auto replaced = RunAudioCook(request, type.Value(), target, jobs, streamed);
        REQUIRE(replaced.HasValue());
        CHECK(replaced.Value().cookedAssets == 1);
        CHECK(replaced.Value().cacheHits == 0);
        auto generation = Assets::ResolveCurrentCookGeneration(request.cookedRoot);
        REQUIRE(generation.HasValue());
        const auto artifactPath = generation.Value().generationRoot / (assetId.Value().ToString() + ".cooked");
        std::ifstream artifactFile(artifactPath, std::ios::binary);
        REQUIRE(artifactFile.good());
        const auto size = std::filesystem::file_size(artifactPath);
        std::vector<std::uint8_t> artifactBytes(size);
        artifactFile.read(reinterpret_cast<char *>(artifactBytes.data()), static_cast<std::streamsize>(artifactBytes.size()));
        REQUIRE(artifactFile.good());
        auto artifact = Assets::DecodeCookedArtifact(artifactBytes);
        REQUIRE(artifact.HasValue());
        auto media = InspectCookedAudio(artifact.Value().payload);
        REQUIRE(media.HasValue());
        CHECK(media.Value().residency == AudioCookResidency::Streamed);
        CHECK(media.Value().chunkCount == 2);
        jobs.Shutdown(ShutdownPolicy::Drain);
    }
}  // namespace Horo::Audio
