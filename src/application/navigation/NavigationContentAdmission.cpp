#include "Horo/Navigation/NavigationErrors.h"
#include "NavigationContentInternal.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Horo::Application {
    namespace {
        using Json = nlohmann::ordered_json;
        constexpr std::size_t MaximumExtensionBytes = 16'384U;

        /** @brief Integrity evidence remains subordinate to the package host's signature and product authority. */
        [[nodiscard]] bool MatchesInventory(const std::span<const std::uint8_t> archive, const Release::ReleaseArtifactManifest &manifest,
                                            const std::string_view path, const Release::DistributionProductKind product) {
            if (manifest.Data().product.kind != product)
                return false;
            const auto found = std::ranges::find(manifest.Artifacts(), path, &Release::ReleaseArtifactRecord::path);
            return found != manifest.Artifacts().end() && found->role == Release::ReleaseArtifactRole::AssetArchive &&
                   found->size == archive.size() && found->digest == ComputeSha256(std::as_bytes(archive));
        }

        /** @brief Select one required version with no unknown-version or duplicate fallback. */
        [[nodiscard]] const Release::ReleaseManifestExtension *FindExtension(const Release::ReleaseArtifactManifest &manifest) {
            const Release::ReleaseManifestExtension *result = nullptr;
            for (const auto &extension : manifest.Data().extensions) {
                if (extension.name != "horo.navigation.content")
                    continue;
                if (result || extension.version != 1 || extension.canonicalJson.size() > MaximumExtensionBytes)
                    return nullptr;
                result = &extension;
            }
            return result;
        }

        /** @brief Recover validated owned policy exclusively from an exactly pinned immutable envelope. */
        [[nodiscard]] Result<Navigation::NavMeshAssetContentExpectation> ReadPolicy(const Assets::AssetArchiveProvider &provider,
                                                                                    const Assets::AssetId id, const Sha256Digest &digest,
                                                                                    const AssetCookTargetId &target,
                                                                                    const Assets::AssetArchiveLimits &limits) {
            const auto bytes = provider.Load(id, {});
            if (bytes.HasError())
                return Result<Navigation::NavMeshAssetContentExpectation>::Failure(bytes.ErrorValue());
            if (ComputeSha256(std::as_bytes(std::span{bytes.Value()})) != digest)
                return Result<Navigation::NavMeshAssetContentExpectation>::Failure(
                    MakeError(Navigation::NavigationErrors::NavMeshArtifactCorrupt));
            Assets::AssetCookLimits cook;
            cook.maximumArtifactBytes = std::min(cook.maximumArtifactBytes, limits.maximumAssetBytes);
            const auto envelope = Assets::DecodeCookedArtifact(bytes.Value(), cook);
            if (envelope.HasError())
                return Result<Navigation::NavMeshAssetContentExpectation>::Failure(envelope.ErrorValue());
            if (envelope.Value().id != id || envelope.Value().type.Value() != Assets::NavMeshAssetTypeName ||
                envelope.Value().target != target)
                return Result<Navigation::NavMeshAssetContentExpectation>::Failure(
                    MakeError(Navigation::NavigationErrors::NavMeshArtifactCorrupt));
            const auto set = Navigation::DecodeNavigationCookedTileSet(envelope.Value().payload, limits.maximumAssetBytes);
            if (set.HasError())
                return Result<Navigation::NavMeshAssetContentExpectation>::Failure(set.ErrorValue());
            if (!set.Value().provenance || !set.Value().provenance->projectProfile ||
                set.Value().inputFingerprint != envelope.Value().sourceDigest)
                return Result<Navigation::NavMeshAssetContentExpectation>::Failure(
                    MakeError(Navigation::NavigationErrors::NavMeshArtifactCorrupt));
            return Result<Navigation::NavMeshAssetContentExpectation>::Success(
                {id, digest, set.Value().provenance->compatibility, *set.Value().provenance->projectProfile});
        }

        /** @brief Require exact navigation evidence for parser-validated visible members, preserving unrelated asset types. */
        [[nodiscard]] bool MatchesNavigationClosure(const AdmittedNavigationReleaseContent &content,
                                                    const AssetCookTargetId &target) noexcept {
            if (content.provider.Target() != target)
                return false;
            std::size_t index{};
            for (const auto &member : content.provider.Members()) {
                if (member.type.Value() != Assets::NavMeshAssetTypeName)
                    continue;
                if (index == content.expectations.size() || content.expectations[index].id != member.id)
                    return false;
                ++index;
            }
            return index == content.expectations.size();
        }

        /** @brief Strict bounded canonical extension shape; unknown fields cannot acquire authority. */
        [[nodiscard]] bool MatchesExtension(const Json &json, const Release::ReleaseManifestExtension &extension,
                                            const AssetCookTargetId &target, const Release::DistributionProductKind product,
                                            const std::span<const std::uint8_t> archive, const Assets::AssetArchiveLimits &limits) {
            return json.is_object() && json.size() == 4 && json.contains("product") && json.contains("target") &&
                   json.contains("archive") && json.contains("assets") && json["product"].is_string() && json["target"].is_string() &&
                   json["archive"].is_string() && json["assets"].is_array() && json["assets"].size() <= limits.maximumAssets &&
                   json.dump() == extension.canonicalJson && json["target"] == target.Value() &&
                   json["product"] ==
                       (product == Release::DistributionProductKind::GameRuntime ? "game-runtime" : "game-dedicated-server") &&
                   json["archive"] == FormatSha256(ComputeSha256(std::as_bytes(archive)));
        }
    }  // namespace

    /** @copydoc NavigationContentDetail::ValidateContent */
    Result<AdmittedNavigationReleaseContent> NavigationContentDetail::ValidateContent(
        const std::span<const std::uint8_t> archive, const Release::ReleaseArtifactManifest &manifest, const std::string_view archivePath,
        const AssetCookTargetId &target, const Release::DistributionProductKind product, const Assets::AssetArchiveLimits &limits) {
        if ((product != Release::DistributionProductKind::GameRuntime &&
             product != Release::DistributionProductKind::GameDedicatedServer) ||
            !MatchesInventory(archive, manifest, archivePath, product))
            return Result<AdmittedNavigationReleaseContent>::Failure(MakeError(Navigation::NavigationErrors::NavMeshArtifactCorrupt));
        const auto *extension = FindExtension(manifest);
        if (!extension)
            return Result<AdmittedNavigationReleaseContent>::Failure(MakeError(Navigation::NavigationErrors::UnsupportedCookedVersion));
        try {
            // Reject recursive hostile extension shapes before JSON DOM allocation grows beyond the finite schema depth.
            const auto json = Json::parse(extension->canonicalJson, [](const int depth, const Json::parse_event_t, Json &) {
                if (depth > 4)
                    throw std::invalid_argument("navigation extension nesting");
                return true;
            });
            if (!MatchesExtension(json, *extension, target, product, archive, limits))
                return Result<AdmittedNavigationReleaseContent>::Failure(MakeError(Navigation::NavigationErrors::NavMeshArtifactCorrupt));
            auto opened = Assets::AssetArchiveProvider::Open(archive, target, limits);
            if (opened.HasError())
                return Result<AdmittedNavigationReleaseContent>::Failure(opened.ErrorValue());
            AdmittedNavigationReleaseContent result{std::move(opened).Value(), {}};
            result.expectations.reserve(json["assets"].size());
            for (const auto &entry : json["assets"]) {
                if (!entry.is_object() || entry.size() != 6 || !entry.contains("asset") || !entry.contains("envelope") ||
                    !entry["asset"].is_string() || !entry["envelope"].is_string() || !entry.contains("provider") ||
                    !entry.contains("schemas") || !entry.contains("settings") || !entry.contains("profile") ||
                    !entry["provider"].is_string() || !entry["schemas"].is_string() || !entry["settings"].is_string() ||
                    !entry["profile"].is_object() || entry["profile"].size() != 3 || !entry["profile"].contains("id") ||
                    !entry["profile"].contains("revision") || !entry["profile"].contains("fingerprint") ||
                    !entry["profile"]["id"].is_number_unsigned() || !entry["profile"]["revision"].is_number_unsigned() ||
                    !entry["profile"]["fingerprint"].is_number_unsigned())
                    return Result<AdmittedNavigationReleaseContent>::Failure(
                        MakeError(Navigation::NavigationErrors::NavMeshArtifactCorrupt));
                const auto id = Assets::AssetId::Parse(entry["asset"].get_ref<const std::string &>());
                const auto digest = ParseSha256(entry["envelope"].get_ref<const std::string &>());
                if (id.HasError() || digest.HasError() || (!result.expectations.empty() && id.Value() <= result.expectations.back().id))
                    return Result<AdmittedNavigationReleaseContent>::Failure(
                        MakeError(Navigation::NavigationErrors::NavMeshArtifactCorrupt));
                auto policy = ReadPolicy(result.provider, id.Value(), digest.Value(), target, limits);
                if (policy.HasError())
                    return Result<AdmittedNavigationReleaseContent>::Failure(policy.ErrorValue());
                if (entry != NavigationContentDetail::ContentRecord(policy.Value()))
                    return Result<AdmittedNavigationReleaseContent>::Failure(
                        MakeError(Navigation::NavigationErrors::NavMeshArtifactCorrupt));
                result.expectations.push_back(std::move(policy).Value());
            }
            if (!MatchesNavigationClosure(result, target))
                return Result<AdmittedNavigationReleaseContent>::Failure(MakeError(Navigation::NavigationErrors::NavMeshArtifactCorrupt));
            return Result<AdmittedNavigationReleaseContent>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Result<AdmittedNavigationReleaseContent>::Failure(MakeError(Navigation::NavigationErrors::CapacityExceeded));
        } catch (const std::invalid_argument &) {
            return Result<AdmittedNavigationReleaseContent>::Failure(MakeError(Navigation::NavigationErrors::NavMeshArtifactCorrupt));
        } catch (const Json::exception &) {
            return Result<AdmittedNavigationReleaseContent>::Failure(MakeError(Navigation::NavigationErrors::NavMeshArtifactCorrupt));
        }
    }

    /** @copydoc AdmitNavigationReleaseContent */
    Result<AdmittedNavigationReleaseContent> AdmitNavigationReleaseContent(
        const std::span<const std::uint8_t> archive, const Release::ReleaseArtifactManifest &manifest,
        const Release::VerifiedReleaseCandidate &verified, const std::string_view archivePath, const AssetCookTargetId &target,
        const Release::DistributionProductKind product, const Assets::AssetArchiveLimits &limits) {
        if (verified.ManifestDigest() != manifest.Digest() || verified.Candidate() != manifest.Data().candidate)
            return Result<AdmittedNavigationReleaseContent>::Failure(MakeError(Navigation::NavigationErrors::StaleSnapshot));
        return NavigationContentDetail::ValidateContent(archive, manifest, archivePath, target, product, limits);
    }
}  // namespace Horo::Application
