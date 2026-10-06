#include "Horo/Navigation/NavigationErrors.h"
#include "NavigationContentInternal.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace Horo::Application {
    namespace {
        using Json = nlohmann::ordered_json;
        constexpr std::size_t MaximumExtensionBytes = 16'384U;

        /** @brief Premeasure exact canonical ASCII fields with checked count arithmetic before projection/archive allocation. */
        [[nodiscard]] bool FitsEvidence(const std::size_t entries, const AssetCookTargetId &target,
                                        const Release::DistributionProductKind product) noexcept {
            constexpr std::string_view EmptyEvidence{R"({"product":"","target":"","archive":"","assets":[]})"};
            constexpr std::string_view EmptyEntry{
                R"({"asset":"","envelope":"","provider":"","schemas":"","settings":"","profile":{"id":,"revision":,"fingerprint":}})"};
            constexpr std::size_t DigestTextBytes = 71;
            constexpr std::size_t AssetIdTextBytes = 36;
            // The minimum-width lower bound rejects impossible counts before allocating policy projections.
            constexpr std::size_t EntryBytes = EmptyEntry.size() + 4 * DigestTextBytes + AssetIdTextBytes + 3;
            const std::string_view productText =
                product == Release::DistributionProductKind::GameRuntime ? "game-runtime" : "game-dedicated-server";
            const auto fixed = EmptyEvidence.size() + productText.size() + DigestTextBytes;
            if (!target.IsValid() || target.Value().size() > MaximumExtensionBytes - fixed)
                return false;
            const auto base = fixed + target.Value().size();
            if (entries == 0)
                return true;
            return entries <= (MaximumExtensionBytes - base + 1) / (EntryBytes + 1);
        }

        /** @brief Count canonical unsigned decimal bytes without formatting or allocating. */
        [[nodiscard]] std::size_t DecimalBytes(std::uint64_t value) noexcept {
            std::size_t bytes = 1;
            while (value >= 10) {
                value /= 10;
                ++bytes;
            }
            return bytes;
        }

        /** @brief Check exact actual profile widths before building JSON or any archive output. */
        [[nodiscard]] bool FitsActualEvidence(const std::span<const Navigation::NavMeshAssetContentExpectation> entries,
                                              const AssetCookTargetId &target, const Release::DistributionProductKind product) noexcept {
            constexpr std::string_view EmptyEvidence{R"({"product":"","target":"","archive":"","assets":[]})"};
            constexpr std::string_view EmptyEntry{
                R"({"asset":"","envelope":"","provider":"","schemas":"","settings":"","profile":{"id":,"revision":,"fingerprint":}})"};
            const std::string_view productText =
                product == Release::DistributionProductKind::GameRuntime ? "game-runtime" : "game-dedicated-server";
            std::size_t size = EmptyEvidence.size() + productText.size() + target.Value().size() + 71;
            bool first = true;
            for (const auto &entry : entries) {
                const auto addition = EmptyEntry.size() + 4 * 71 + 36 + DecimalBytes(entry.projectProfile.Id().Value()) +
                                      DecimalBytes(entry.projectProfile.Revision().Value()) +
                                      DecimalBytes(entry.projectProfile.Fingerprint().Value()) + (first ? 0 : 1);
                if (size > MaximumExtensionBytes || addition > MaximumExtensionBytes - size)
                    return false;
                size += addition;
                first = false;
            }
            return size <= MaximumExtensionBytes;
        }

        /** @brief Construct evidence from decoded promoted bytes, never from a fabricated source snapshot. */
        [[nodiscard]] Result<Navigation::NavMeshAssetContentExpectation> ReadExpectation(const Assets::AssetCookManifestEntry &entry,
                                                                                         const std::span<const std::uint8_t> bytes,
                                                                                         const AssetCookTargetId &target,
                                                                                         const Assets::AssetCookLimits &limits) {
            const auto envelope = Assets::DecodeCookedArtifact(bytes, limits);
            if (envelope.HasError())
                return Result<Navigation::NavMeshAssetContentExpectation>::Failure(envelope.ErrorValue());
            if (envelope.Value().id != entry.assetId || envelope.Value().type != entry.assetType || envelope.Value().target != target)
                return Result<Navigation::NavMeshAssetContentExpectation>::Failure(
                    MakeError(Navigation::NavigationErrors::NavMeshArtifactCorrupt));
            const auto set = Navigation::DecodeNavigationCookedTileSet(envelope.Value().payload, limits.maximumArtifactBytes);
            if (set.HasError())
                return Result<Navigation::NavMeshAssetContentExpectation>::Failure(set.ErrorValue());
            if (set.Value().inputFingerprint != envelope.Value().sourceDigest || !set.Value().provenance ||
                !set.Value().provenance->projectProfile)
                return Result<Navigation::NavMeshAssetContentExpectation>::Failure(
                    MakeError(Navigation::NavigationErrors::NavMeshArtifactCorrupt));
            return Result<Navigation::NavMeshAssetContentExpectation>::Success(
                {entry.assetId, entry.artifactHash, set.Value().provenance->compatibility, *set.Value().provenance->projectProfile});
        }

        /** @brief Project all promoted members into owned archive inputs without rebuilding any artifact. */
        [[nodiscard]] Result<std::vector<Assets::AssetArchiveInput>> PrepareArchiveInputs(Assets::AssetCookGenerationContents &contents,
                                                                                          PreparedNavigationReleaseContent &result,
                                                                                          const AssetCookTargetId &target,
                                                                                          const Assets::AssetCookLimits &cookLimits) {
            std::vector<Assets::AssetArchiveInput> inputs;
            inputs.reserve(contents.entries.size());
            for (std::size_t index = 0; index < contents.entries.size(); ++index) {
                const auto &entry = contents.entries[index];
                if (entry.assetType.Value() == Assets::NavMeshAssetTypeName) {
                    auto expectation = ReadExpectation(entry, contents.artifacts[index], target, cookLimits);
                    if (expectation.HasError())
                        return Result<std::vector<Assets::AssetArchiveInput>>::Failure(expectation.ErrorValue());
                    result.expectations.push_back(std::move(expectation).Value());
                }
                inputs.push_back({entry.assetId, std::move(contents.artifacts[index])});
            }
            return Result<std::vector<Assets::AssetArchiveInput>>::Success(std::move(inputs));
        }

        /** @brief Canonical minimal manifest closure pins full policy through exact immutable envelope hashes. */
        [[nodiscard]] Release::ReleaseManifestExtension Evidence(const PreparedNavigationReleaseContent &content,
                                                                 const AssetCookTargetId &target,
                                                                 const Release::DistributionProductKind product) {
            Json entries = Json::array();
            for (const auto &expected : content.expectations)
                entries.push_back(NavigationContentDetail::ContentRecord(expected));
            Json value{{"product", product == Release::DistributionProductKind::GameRuntime ? "game-runtime" : "game-dedicated-server"},
                       {"target", target.Value()},
                       {"archive", FormatSha256(ComputeSha256(std::as_bytes(std::span{content.archive})))},
                       {"assets", std::move(entries)}};
            return {"horo.navigation.content", 1, value.dump()};
        }
    }  // namespace

    /** @copydoc NavigationContentDetail::ContentRecord */
    nlohmann::ordered_json NavigationContentDetail::ContentRecord(const Navigation::NavMeshAssetContentExpectation &expected) {
        return {{"asset", expected.id.ToString()},
                {"envelope", FormatSha256(expected.cookedContentDigest)},
                {"provider", FormatSha256(expected.compatibility.provider)},
                {"schemas", FormatSha256(expected.compatibility.schemas)},
                {"settings", FormatSha256(expected.compatibility.settings)},
                {"profile",
                 {{"id", expected.projectProfile.Id().Value()},
                  {"revision", expected.projectProfile.Revision().Value()},
                  {"fingerprint", expected.projectProfile.Fingerprint().Value()}}}};
    }

    /** @copydoc PrepareNavigationReleaseContent */
    Result<PreparedNavigationReleaseContent> PrepareNavigationReleaseContent(const Assets::AssetCookGeneration &generation,
                                                                             const Assets::AssetChunkPlan &plan,
                                                                             const Release::DistributionProductKind product,
                                                                             const Assets::AssetArchiveLimits &limits) {
        if ((product != Release::DistributionProductKind::GameRuntime &&
             product != Release::DistributionProductKind::GameDedicatedServer) ||
            (product == Release::DistributionProductKind::GameDedicatedServer && std::ranges::none_of(plan.Chunks(), [](const auto &chunk) {
            return chunk.kind == Assets::AssetChunkKind::DedicatedServer;
        })))
            return Result<PreparedNavigationReleaseContent>::Failure(MakeError(Navigation::NavigationErrors::NavMeshArtifactInvalid));
        if (!FitsEvidence(0, generation.target, product))
            return Result<PreparedNavigationReleaseContent>::Failure(MakeError(Navigation::NavigationErrors::CapacityExceeded));
        try {
            Assets::AssetCookLimits cookLimits;
            // Cook-generation admission has its own legal ceilings; archive policy can tighten but cannot raise them.
            cookLimits.maximumArtifactBytes = std::min(cookLimits.maximumArtifactBytes, limits.maximumAssetBytes);
            cookLimits.maximumAssets = std::min(cookLimits.maximumAssets, limits.maximumAssets);
            auto read = Assets::ReadCookGenerationContents(generation, limits.maximumArchiveBytes, cookLimits);
            if (read.HasError())
                return Result<PreparedNavigationReleaseContent>::Failure(read.ErrorValue());
            auto contents = std::move(read).Value();
            const auto navigationCount = static_cast<std::size_t>(std::ranges::count_if(contents.entries, [](const auto &entry) {
                return entry.assetType.Value() == Assets::NavMeshAssetTypeName;
            }));
            if (!FitsEvidence(navigationCount, generation.target, product))
                return Result<PreparedNavigationReleaseContent>::Failure(MakeError(Navigation::NavigationErrors::CapacityExceeded));
            PreparedNavigationReleaseContent result;
            result.expectations.reserve(navigationCount);
            auto inputs = PrepareArchiveInputs(contents, result, generation.target, cookLimits);
            if (inputs.HasError())
                return Result<PreparedNavigationReleaseContent>::Failure(inputs.ErrorValue());
            if (!FitsActualEvidence(result.expectations, generation.target, product))
                return Result<PreparedNavigationReleaseContent>::Failure(MakeError(Navigation::NavigationErrors::CapacityExceeded));
            auto archive = Assets::BuildAssetArchive(plan, generation.target, inputs.Value(), limits);
            if (archive.HasError())
                return Result<PreparedNavigationReleaseContent>::Failure(archive.ErrorValue());
            result.archive = std::move(archive).Value();
            result.extension = Evidence(result, generation.target, product);
            if (result.extension.canonicalJson.size() > MaximumExtensionBytes)
                return Result<PreparedNavigationReleaseContent>::Failure(MakeError(Navigation::NavigationErrors::CapacityExceeded));
            return Result<PreparedNavigationReleaseContent>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Result<PreparedNavigationReleaseContent>::Failure(MakeError(Navigation::NavigationErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Application
