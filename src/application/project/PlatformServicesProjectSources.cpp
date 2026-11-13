#include "Horo/Application/PlatformServicesProjectSources.h"

#include <utility>

namespace Horo::Application {
    using namespace PlatformServices;

    namespace {
        constexpr std::array<std::string_view, 6> Paths{".horo/project.json",
                                                        PlatformServicesDocumentPaths::Ledger,
                                                        PlatformServicesDocumentPaths::Achievements,
                                                        PlatformServicesDocumentPaths::Stats,
                                                        PlatformServicesDocumentPaths::Leaderboards,
                                                        PlatformServicesDocumentPaths::Presence};

        /** @brief Preserves safe logical source context without disclosing native paths or input values. */
        Error SourceError(const ErrorCodeDescriptor &descriptor, const std::size_t index) {
            auto error = MakeError(descriptor);
            error.diagnostics.push_back({.code = DiagnosticCode{descriptor.code.Value()},
                                         .message = std::string{descriptor.remediationHint},
                                         .location = {.source = std::string{Paths[index]}}});
            return error;
        }

        /** @brief Applies the existing bounded physically-contained capture contract to one fixed source. */
        Result<Assets::AssetSourceCapture> Capture(const std::filesystem::path &root, const std::size_t index,
                                                   const CancellationToken cancellation) {
            return Assets::CaptureAssetSource(root, root / Paths[index],
                                              index == 0 ? 1024U * 1024U : PlatformStableIdRegistryMaximumDocumentBytes, cancellation);
        }

        /** @brief Borrows admitted bytes only while their owning captures remain alive. */
        std::string_view Text(const Assets::AssetSourceCapture &source) {
            return source.bytes.empty() ? std::string_view{}
                                        : std::string_view{reinterpret_cast<const char *>(source.bytes.data()), source.bytes.size()};
        }

        /** @brief Hashes fixed-order document digests under a distinct domain and explicit NUL separator. */
        Sha256Digest Generation(const std::array<Assets::AssetSourceIdentity, 6> &sources) {
            constexpr std::string_view domain = "horo.platform-services.project-source.v1";
            const std::array<std::byte, 1> separator{};
            std::array<std::span<const std::byte>, 8> fragments;
            fragments[0] = std::as_bytes(std::span{domain.data(), domain.size()});
            fragments[1] = separator;
            for (std::size_t index = 0; index < sources.size(); ++index)
                fragments[index + 2] = std::as_bytes(std::span{sources[index].digest.bytes});
            return ComputeSha256Fragments(fragments);
        }
    }  // namespace

    /** @copydoc CapturePlatformServicesProjectSources */
    Result<PlatformServicesProjectSourceSnapshot> CapturePlatformServicesProjectSources(
        const std::filesystem::path &projectRoot, const ProjectCompatibilitySnapshot &admission,
        const PlatformServicesProjectValidationContext &context, const CancellationToken cancellation) {
        using Output = Result<PlatformServicesProjectSourceSnapshot>;
        if ((admission.status != ProjectCompatibilityStatus::Current &&
             admission.status != ProjectCompatibilityStatus::CompatibleReleaseLine) ||
            admission.markerUpdateRequired || !admission.metadata || !admission.sourceFingerprint)
            return Output::Failure(SourceError(Assets::AssetSourceErrors::Authority, 0));
        const auto &expectedProjectId = admission.metadata->projectId;
        if (expectedProjectId.empty() || expectedProjectId.size() > PlatformServicesMaximumProjectIdBytes)
            return Output::Failure(SourceError(Assets::AssetSourceErrors::Conflict, 0));
        std::array<Assets::AssetSourceCapture, 6> sources;
        std::array<Assets::AssetSourceIdentity, 6> identities;
        const auto read = [&](const std::size_t index) -> Result<void> {
            auto captured = Capture(projectRoot, index, cancellation);
            if (captured.HasError())
                return Result<void>::Failure(std::move(captured).ErrorValue());
            if (!captured.Value().identity.exists)
                return Result<void>::Failure(SourceError(Assets::AssetSourceErrors::ReadFailed, index));
            identities[index] = captured.Value().identity;
            sources[index] = std::move(captured).Value();
            return Result<void>::Success();
        };
        if (auto captured = read(0); captured.HasError())
            return Output::Failure(std::move(captured).ErrorValue());
        if (identities[0].digest != *admission.sourceFingerprint)
            return Output::Failure(SourceError(Assets::AssetSourceErrors::Conflict, 0));
        auto root = DecodeProjectSourceDocument(Text(sources[0]));
        if (root.HasError())
            return Output::Failure(std::move(root).ErrorValue());
        if (root.Value().metadata.projectId != expectedProjectId)
            return Output::Failure(SourceError(Assets::AssetSourceErrors::Conflict, 0));
        auto salt = ResolveProjectPlatformServicesSalt(root.Value());
        if (salt.HasError())
            return Output::Failure(std::move(salt).ErrorValue());
        for (std::size_t index = 1; index < Paths.size(); ++index)
            if (auto captured = read(index); captured.HasError())
                return Output::Failure(std::move(captured).ErrorValue());
        auto ids = DecodePlatformServicesLedger(Text(sources[1]), expectedProjectId, salt.Value());
        if (ids.HasError())
            return Output::Failure(std::move(ids).ErrorValue());
        auto policy = ResolveProjectPlatformServicesConfiguration(root.Value(), context.profile, context.contributions,
                                                                  context.trustedModules, context.policyLimits);
        if (policy.HasError())
            return Output::Failure(std::move(policy).ErrorValue());
        auto achievements = DecodePlatformAchievementDefinitions(Text(sources[2]), ids.Value(), context.achievementLimits);
        if (achievements.HasError())
            return Output::Failure(std::move(achievements).ErrorValue());
        auto stats = DecodePlatformStatDefinitions(Text(sources[3]), ids.Value(), context.definitionLimits);
        if (stats.HasError())
            return Output::Failure(std::move(stats).ErrorValue());
        auto leaderboards = DecodePlatformLeaderboardDefinitions(Text(sources[4]), ids.Value(), stats.Value(), context.definitionLimits);
        if (leaderboards.HasError())
            return Output::Failure(std::move(leaderboards).ErrorValue());
        auto presence = DecodePlatformPresenceDefinitions(Text(sources[5]), ids.Value(), context.definitionLimits);
        if (presence.HasError())
            return Output::Failure(std::move(presence).ErrorValue());
        PlatformServicesProjectSourceSnapshot snapshot{projectRoot,
                                                       identities,
                                                       std::move(ids).Value(),
                                                       std::move(achievements).Value(),
                                                       std::move(stats).Value(),
                                                       std::move(leaderboards).Value(),
                                                       std::move(presence).Value(),
                                                       std::move(policy).Value()};
        auto current = RevalidatePlatformServicesProjectSources(snapshot, cancellation);
        if (current.HasError())
            return Output::Failure(std::move(current).ErrorValue());
        return Output::Success(std::move(snapshot));
    }

    /** @copydoc RevalidatePlatformServicesProjectSources */
    Result<void> RevalidatePlatformServicesProjectSources(const PlatformServicesProjectSourceSnapshot &snapshot,
                                                          const CancellationToken cancellation) {
        for (std::size_t index = 0; index < Paths.size(); ++index) {
            auto source = Capture(snapshot.root_, index, cancellation);
            if (source.HasError())
                return Result<void>::Failure(std::move(source).ErrorValue());
            if (source.Value().identity != snapshot.sources_[index])
                return Result<void>::Failure(SourceError(Assets::AssetSourceErrors::Conflict, index));
        }
        return Result<void>::Success();
    }

    /** @copydoc PlatformServicesProjectSourceSnapshot::PlatformServicesProjectSourceSnapshot */
    PlatformServicesProjectSourceSnapshot::PlatformServicesProjectSourceSnapshot(
        std::filesystem::path root, std::array<Assets::AssetSourceIdentity, 6> sources, PlatformStableIdRegistry ids,
        AchievementDefinitionRegistry achievements, StatDefinitionRegistry stats, LeaderboardDefinitionRegistry leaderboards,
        PresenceDefinitionRegistry presence, PlatformProjectConfiguration configuration)
        : root_(std::move(root)), sources_(std::move(sources)), fingerprint_(Generation(sources_)), ids_(std::move(ids)),
          achievements_(std::move(achievements)), stats_(std::move(stats)), leaderboards_(std::move(leaderboards)),
          presence_(std::move(presence)), configuration_(std::move(configuration)) {}
}  // namespace Horo::Application
