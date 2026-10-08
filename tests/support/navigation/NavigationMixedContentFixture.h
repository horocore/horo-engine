#pragma once

#include "Horo/Audio/AudioCooker.h"
#include "assets/AssetCookPublicationFixture.h"
#include "navigation/NavigationReleaseFixture.h"

namespace Horo::Application::ContentTestSupport {
    /** @brief Valid four-frame stereo PCM-WAV input; no hardware device or native audio renderer is involved. */
    inline constexpr std::array<std::uint8_t, 60> MixedWaveSource{'R',  'I',  'F', 'F',  52, 0,    0, 0,    'W', 'A', 'V', 'E',
                                                                  'f',  'm',  't', ' ',  16, 0,    0, 0,    1,   0,   2,   0,
                                                                  0x80, 0xbb, 0,   0,    0,  0xee, 2, 0,    4,   0,   16,  0,
                                                                  'd',  'a',  't', 'a',  16, 0,    0, 0,    0,   0,   0,   0x40,
                                                                  0xff, 0x7f, 0,   0x80, 0,  0xc0, 0, 0x20, 0,   0,   0,   0xe0};

    [[nodiscard]] inline Assets::AssetId AudioAsset() {
        const auto id = Assets::AssetId::Parse("00000000-0000-0000-0000-0000000000a1");
        REQUIRE(id.HasValue());
        return id.Value();
    }

    /** @brief Genuine Audio contribution and generic cook/cache/publication owners produce the non-navigation envelope. */
    [[nodiscard]] inline Assets::AssetCookGeneration CookSharedAudio(const std::filesystem::path &root, const AssetCookTargetId &target) {
        const auto type = Assets::AssetTypeId::Parse("audio.clip");
        const auto source = ProjectPath::Parse("assets/clip.wav");
        const auto sidecar = ProjectPath::Parse("assets/clip.wav.horo");
        REQUIRE(type.HasValue());
        REQUIRE(source.HasValue());
        REQUIRE(sidecar.HasValue());
        Write(root / "source/assets/clip.wav", MixedWaveSource);
        Assets::AssetRegistry registry;
        REQUIRE(registry.Publish({{AudioAsset(), type.Value(), source.Value(), sidecar.Value()}}).status ==
                Assets::AssetRegistryBuildStatus::Complete);
        Assets::CookerCatalog catalog;
        auto contribution = Audio::MakeAudioCookerContribution(type.Value(), {}, target, {"miniaudio-pinned-1"});
        REQUIRE(contribution.HasValue());
        REQUIRE(catalog.Register(std::move(contribution).Value()).HasValue());
        auto snapshot = catalog.Publish();
        REQUIRE(snapshot.HasValue());
        JobSystem jobs{JobSystemConfig{2, 16}};
        Assets::AssetCookService service{jobs, std::move(snapshot).Value()};
        Assets::AssetCookRequest request{.sourceRoot = root / "source",
                                         .cacheRoot = root / "cache",
                                         .cookedRoot = root / "cooked",
                                         .registry = registry.Snapshot(),
                                         .target = target};
        Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);
        const auto cooked = service.Cook(request, {});
        REQUIRE(cooked.HasValue());
        REQUIRE(cooked.Value().cookedAssets == 1);
        REQUIRE(cooked.Value().cacheHits == 0);
        return cooked.Value().generation;
    }

    /** @brief Only immutable promoted outputs are assembled; neither existing producer is invoked during this publication. */
    [[nodiscard]] inline Assets::AssetCookGeneration PublishMixed(const std::filesystem::path &root,
                                                                  const Assets::AssetCookGeneration &navigation,
                                                                  const Assets::AssetCookGeneration &audio) {
        auto nav = Assets::ReadCookGenerationContents(navigation, 8U * 1024U * 1024U);
        auto clip = Assets::ReadCookGenerationContents(audio, 8U * 1024U * 1024U);
        REQUIRE(nav.HasValue());
        REQUIRE(clip.HasValue());
        REQUIRE(nav.Value().entries.size() == 1);
        REQUIRE(clip.Value().entries.size() == 1);
        REQUIRE(navigation.target == audio.target);
        auto navigationContents = std::move(nav).Value();
        auto audioContents = std::move(clip).Value();
        const std::array entries{audioContents.entries.front(), navigationContents.entries.front()};
        const std::array artifacts{std::move(audioContents.artifacts.front()), std::move(navigationContents.artifacts.front())};
        REQUIRE(entries.front().assetId < entries.back().assetId);
        REQUIRE(std::filesystem::create_directories(root));
        NativeDurableFileSystem files;
        auto writer = files.TryAcquireExclusive(root / ".cook-writer.lock", "mixed package corpus");
        REQUIRE(writer.HasValue());
        auto published = Assets::PublishCookGeneration(root, navigation.target, entries, artifacts, {},
                                                       {.files = &files,
                                                        .operationId = "20000000-0000-0000-0000-000000000001",
                                                        .writerLease = &writer.Value()});
        REQUIRE(published.HasValue());
        REQUIRE_FALSE(published.Value().durabilityError.has_value());
        return std::move(published).Value();
    }

    /** @brief Explicit host plan declares CPU-readable shared Base content; dedicated navigation depends on that real Base. */
    [[nodiscard]] inline Assets::AssetChunkPlan MixedPlan(const Assets::AssetId navigation,
                                                          const Release::DistributionProductKind product) {
        const auto base = Assets::AssetChunkId::Parse("base");
        const auto server = Assets::AssetChunkId::Parse("server");
        REQUIRE(base.HasValue());
        REQUIRE(server.HasValue());
        std::vector<Assets::AssetChunkDefinition> chunks;
        if (product == Release::DistributionProductKind::GameDedicatedServer) {
            chunks.push_back({base.Value(), Assets::AssetChunkKind::Base, {AudioAsset()}, {}});
            chunks.push_back({server.Value(), Assets::AssetChunkKind::DedicatedServer, {navigation}, {base.Value()}});
        } else {
            chunks.push_back({base.Value(), Assets::AssetChunkKind::Base, {AudioAsset(), navigation}, {}});
        }
        auto plan = Assets::AssetChunkPlan::Create(chunks);
        REQUIRE(plan.HasValue());
        return std::move(plan).Value();
    }

    /** @brief Actual archive provider preserves generic non-nav bytes; inspection verifies the genuine cooked Audio schema. */
    inline void RequireSharedAudio(const Assets::AssetArchiveProvider &provider, const AssetCookTargetId &target) {
        const auto bytes = provider.Load(AudioAsset(), {});
        REQUIRE(bytes.HasValue());
        const auto envelope = Assets::DecodeCookedArtifact(bytes.Value());
        REQUIRE(envelope.HasValue());
        REQUIRE(envelope.Value().id == AudioAsset());
        REQUIRE(envelope.Value().target == target);
        REQUIRE(envelope.Value().type.Value() == "audio.clip");
        const auto audio = Audio::InspectCookedAudio(envelope.Value().payload);
        REQUIRE(audio.HasValue());
        REQUIRE(audio.Value().frameCount == 4);
        REQUIRE(audio.Value().sourceDigest == ComputeSha256(std::as_bytes(std::span{MixedWaveSource})));
    }
}  // namespace Horo::Application::ContentTestSupport
