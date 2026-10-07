#pragma once

#include "Horo/Assets/AssetCook.h"
#include "Horo/Runtime/Scene/SavedSceneBootstrap.h"
#include "SceneTestIdentity.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Runtime::SceneContentTest {
    using SceneTest::Id;

    template <typename T> T V(std::uint32_t value = 1) {
        return T::Create(value).Value();
    }

    inline Assets::AssetTypeId SceneType() {
        return Assets::AssetTypeId::Parse("core.scene").Value();
    }

    inline RuntimeSceneDefinition Definition(std::uint64_t id = 7, std::uint64_t revision = 3) {
        SceneDefinitionBuilder builder{SceneDefinitionId{id}, SceneDefinitionRevision{revision}};
        for (const std::uint64_t object : {10U, 20U}) {
            RuntimeEntityDefinition entity;
            entity.object = SceneObjectId{object};
            builder.Add(std::move(entity));
        }
        return std::move(builder).Build().Value();
    }

    /** @brief Actual admitted test-host decoder for its two-byte fixture format, not a shipping engine decoder. */
    class Decoder final : public ISavedSceneBaselineDecoder {
    public:
        std::size_t calls{};
        bool unsupported{};
        std::optional<Assets::AssetId> observed;

        Result<RuntimeSceneDefinition> DecodeBaseline(const SavedSceneBaselineDecodeInput &input) override {
            ++calls;
            observed = input.Artifact().id;
            if (unsupported || input.Artifact().payload.size() != 2)
                return Result<RuntimeSceneDefinition>::Failure(MakeError(SaveErrors::RestoreAdapterContractInvalid));
            return Result<RuntimeSceneDefinition>::Success(Definition(input.Artifact().payload[0], input.Artifact().payload[1]));
        }
    };

    /** @brief Real cooked provider, immutable reader container and installation owner used by bootstrap integration tests. */
    struct Fixture final {
        SavedSceneBootstrapDescriptor descriptor{.world = Id<SaveWorldId>(1),
                                                 .baseScene = Id<SaveBaseSceneId>(2),
                                                 .expectedAssetType = SceneType(),
                                                 .definition = SceneDefinitionId{7},
                                                 .revision = SceneDefinitionRevision{3},
                                                 .contentDigest = {},
                                                 .spawnAnchor = SceneObjectId{20},
                                                 .transition = {.slot = Id<SaveGameSlotId>(5), .generation = Id<SlotGenerationId>(6)}};
        std::shared_ptr<const Assets::AssetArchiveProvider> provider;
        std::unique_ptr<InstalledSaveContent> installed;
        ImmutableSaveArchive archive;
        SaveContentProjectPolicy policy;
        CancellationSource cancellation;
        Decoder decoder;

        explicit Fixture(bool mounted = true, Assets::AssetTypeId type = SceneType(), std::vector<std::uint8_t> payload = {7, 3},
                         std::optional<Assets::AssetId> replacement = {}) {
            auto source = CookSource(std::move(type), std::move(payload));
            MountSource(mounted, std::move(source), replacement);
            WriteSourceArchive();
            // This fixture deliberately exercises the explicitly trusted semantic-schema1 migration path.
            policy.legacy = SaveLegacyContentPolicy::ValidateBaselineOnly;
            policy.compatibility = {.archiveVersions = {{V<ArchiveFormatVersion>(), V<ArchiveFormatVersion>()}, {}},
                                    .saveSchemaVersions = {{V<SaveSchemaVersion>(), V<SaveSchemaVersion>()}, {}},
                                    .productVersions = {{V<ProductSaveCompatibilityVersion>(), V<ProductSaveCompatibilityVersion>()}, {}}};
        }

        struct CookedSource final {
            Assets::AssetCookArtifact artifact;
            std::vector<std::uint8_t> bytes;
        };

        /** @brief Builds the actual baseline envelope and freezes its logical expected digest. */
        CookedSource CookSource(Assets::AssetTypeId type, std::vector<std::uint8_t> payload) {
            const auto asset = Assets::AssetId::FromBytes(descriptor.baseScene.Bytes());
            const auto target = AssetCookTargetId::Parse("headless-null").Value();
            Assets::AssetCookArtifact artifact;
            artifact.id = asset;
            artifact.type = std::move(type);
            artifact.target = target;
            artifact.payload = std::move(payload);
            artifact.payloadDigest = ComputeSha256(std::as_bytes(std::span{artifact.payload}));
            auto cooked = Assets::EncodeCookedArtifact(artifact);
            REQUIRE(cooked.HasValue());
            descriptor.contentDigest = ComputeSha256(std::as_bytes(std::span{cooked.Value()}));
            return {std::move(artifact), std::move(cooked).Value()};
        }

        /** @brief Produces independent core cooked bytes used by the actual selected chunk dependency. */
        Assets::AssetArchiveInput CookCoreAsset(const Assets::AssetId coreAsset, const AssetCookTargetId &target) const {
            Assets::AssetCookArtifact coreArtifact;
            coreArtifact.id = coreAsset;
            coreArtifact.type = Assets::AssetTypeId::Parse("core.mesh").Value();
            coreArtifact.target = target;
            coreArtifact.payload = {1};
            coreArtifact.payloadDigest = ComputeSha256(std::as_bytes(std::span{coreArtifact.payload}));
            auto coreBytes = Assets::EncodeCookedArtifact(coreArtifact);
            REQUIRE(coreBytes.HasValue());
            return {coreAsset, std::move(coreBytes).Value()};
        }

        /** @brief Mounts the real selected cooked chunks, including an explicit compatible physical substitution. */
        void MountSource(const bool mounted, CookedSource source, const std::optional<Assets::AssetId> replacement) {
            const auto asset = source.artifact.id;
            const auto target = source.artifact.target;
            const auto core = Assets::AssetChunkId::Parse("core").Value();
            const auto optional = Assets::AssetChunkId::Parse("scene").Value();
            const auto coreAsset = Assets::AssetId::FromBytes(Id<SaveBaseSceneId>(99).Bytes());
            auto coreInput = CookCoreAsset(coreAsset, target);
            std::vector coreAssets{coreAsset};
            std::optional<std::vector<std::uint8_t>> replacementBytes;
            if (replacement) {
                auto replacementArtifact = source.artifact;
                replacementArtifact.id = *replacement;
                auto encoded = Assets::EncodeCookedArtifact(replacementArtifact);
                REQUIRE(encoded.HasValue());
                replacementBytes = std::move(encoded).Value();
                policy.substitutions.push_back(
                    {asset, *replacement, SceneType(), ComputeSha256(std::as_bytes(std::span{*replacementBytes}))});
                coreAssets.push_back(*replacement);
                std::ranges::sort(coreAssets);
            }
            const std::array chunks{Assets::AssetChunkDefinition{.id = core, .assets = coreAssets},
                                    Assets::AssetChunkDefinition{.id = optional,
                                                                 .kind = Assets::AssetChunkKind::Optional,
                                                                 .assets = {asset},
                                                                 .dependencies = {core}}};
            auto plan = Assets::AssetChunkPlan::Create(chunks);
            REQUIRE(plan.HasValue());
            std::vector inputs{Assets::AssetArchiveInput{asset, std::move(source.bytes)}, std::move(coreInput)};
            if (replacement)
                inputs.push_back({*replacement, std::move(*replacementBytes)});
            auto package = Assets::BuildAssetArchive(plan.Value(), target, inputs);
            REQUIRE(package.HasValue());
            Sha256Digest base;
            base.bytes[0] = 1;
            const std::vector selected = mounted ? std::vector{core, optional} : std::vector{core};
            auto opened = Assets::AssetArchiveProvider::OpenSelected(package.Value(), target, plan.Value(), selected, base);
            REQUIRE(opened.HasValue());
            provider = std::make_shared<const Assets::AssetArchiveProvider>(std::move(opened).Value());
            auto owner = InstalledSaveContent::Create(provider, {});
            REQUIRE(owner.HasValue());
            installed = std::move(owner).Value();
        }

        /** @brief Writes the declared baseline through the production writer and independent reader-admitted archive ownership. */
        void WriteSourceArchive() {
            const auto asset = Assets::AssetId::FromBytes(descriptor.baseScene.Bytes());
            const auto participant = SaveContentRequirementsParticipant();
            const SaveContentRequirement requirement{participant, SaveContentNecessity::Required,
                                                     SaveAssetContentRequirement{asset, SceneType(), descriptor.contentDigest}};
            auto encoded = EncodeSaveContentRequirements(std::array{requirement});
            REQUIRE(encoded.HasValue());
            const auto bytes = encoded.Value().Bytes();
            const SaveArchiveHeader header{.slot = descriptor.transition.slot,
                                           .slotGeneration = descriptor.transition.generation,
                                           .productCompatibility = V<ProductSaveCompatibilityVersion>(),
                                           .product = Id<ProductStorageId>(3),
                                           .environment = Id<EnvironmentStorageId>(4),
                                           .user = Id<LocalUserStorageId>(5),
                                           .profile = Id<GameProfileId>(6),
                                           .project = Id<SaveProjectId>(7),
                                           .world = descriptor.world,
                                           .baseScene = descriptor.baseScene,
                                           .capturedAtUnixMilliseconds = 1,
                                           .engineVersion = "fixture-1",
                                           .projectBuildId = "fixture-1"};
            const SaveGameManifest manifest{V<SaveSchemaVersion>(),
                                            ComputeCanonicalStateHash(bytes),
                                            {{participant, V<ParticipantSchemaVersion>(), true, {SaveContentRequirementsRecord()}}}};
            const PreservedSaveChunk chunk{{.record = SaveContentRequirementsRecord(),
                                            .owner = participant,
                                            .storedByteLength = bytes.size(),
                                            .decodedByteLength = bytes.size(),
                                            .decodedHash = ComputeSha256(bytes)},
                                           {bytes.begin(), bytes.end()}};
            auto written = SaveArchiveContainerWriter::Write(header, manifest, std::array{chunk}, V<ArchiveFormatVersion>());
            if (written.HasError()) {
                INFO("initial content fixture writer: " << written.ErrorValue().code.Value() << ": " << written.ErrorValue().message);
                std::string diagnostics;
                for (const auto &diagnostic : written.ErrorValue().diagnostics)
                    diagnostics += diagnostic.path + ": " + diagnostic.message + "; ";
                INFO("writer diagnostics: " << diagnostics);
                REQUIRE(written.HasValue());
            }
            REQUIRE(written.HasValue());
            const auto finalized = written.Value().Bytes();
            archive.bytes = std::make_shared<const std::vector<std::byte>>(finalized.begin(), finalized.end());
        }

        Result<ReconciledSaveContent> Reconcile() {
            return ReconciledSaveContent::Prepare(*installed, archive, policy, cancellation.Token());
        }

        Result<PreparedSavedSceneBootstrap> Prepare(SavedSceneBootstrapDescriptor requested, Assets::AssetTypeId type = SceneType()) {
            auto proof = Reconcile();
            if (proof.HasError())
                return Result<PreparedSavedSceneBootstrap>::Failure(proof.ErrorValue());
            return PrepareSavedSceneBootstrap(std::move(requested), type, std::move(proof).Value(), &decoder);
        }

        Result<PreparedSavedSceneBootstrap> Prepare() {
            return Prepare(descriptor);
        }
    };
}  // namespace Horo::Runtime::SceneContentTest
