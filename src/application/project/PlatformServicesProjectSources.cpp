#include "Horo/Application/PlatformServicesProjectSources.h"

#include "PlatformServicesPublicationFiles.h"

#include <type_traits>
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
                                         .location = {.source = std::string{Paths.at(index)}}});
            return error;
        }

        /** @brief Applies the existing bounded physically-contained capture contract to one fixed source. */
        Result<Assets::AssetSourceCapture> Capture(const std::filesystem::path &root, const std::size_t index,
                                                   const CancellationToken cancellation) {
            return Assets::CaptureAssetSource(root, root / Paths.at(index),
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
                fragments.at(index + 2) = std::as_bytes(std::span{sources.at(index).digest.bytes});
            return ComputeSha256Fragments(fragments);
        }

        /** @brief Retains strict root admission and byte witnesses before semantic sidecar projection. */
        struct CapturedDocuments final {
            std::array<Assets::AssetSourceCapture, Paths.size()> documents;
            std::array<Assets::AssetSourceIdentity, Paths.size()> identities;
            ProjectSourceDocument root;
            PlatformServicesIdSalt salt;
        };

        /** @brief Reads the root first, preserving identity/salt diagnostic precedence before sidecar reads. */
        Result<CapturedDocuments> ReadDocuments(const std::filesystem::path &projectRoot, const std::string_view projectId,
                                                const Sha256Digest &expectedRoot, const CancellationToken cancellation) {
            using Output = Result<CapturedDocuments>;
            CapturedDocuments result;
            auto captured = Capture(projectRoot, 0, cancellation);
            if (captured.HasError())
                return Output::Failure(std::move(captured).ErrorValue());
            if (!captured.Value().identity.exists)
                return Output::Failure(SourceError(Assets::AssetSourceErrors::ReadFailed, 0));
            if (captured.Value().identity.digest != expectedRoot)
                return Output::Failure(SourceError(Assets::AssetSourceErrors::Conflict, 0));
            result.identities.front() = captured.Value().identity;
            result.documents.front() = std::move(captured).Value();
            auto root = DecodeProjectSourceDocument(Text(result.documents.front()));
            if (root.HasError())
                return Output::Failure(std::move(root).ErrorValue());
            if (root.Value().metadata.projectId != projectId)
                return Output::Failure(SourceError(Assets::AssetSourceErrors::Conflict, 0));
            auto salt = ResolveProjectPlatformServicesSalt(root.Value());
            if (salt.HasError())
                return Output::Failure(std::move(salt).ErrorValue());
            result.root = std::move(root).Value();
            result.salt = std::move(salt).Value();
            for (std::size_t index = 1; index < result.documents.size(); ++index) {
                auto source = Capture(projectRoot, index, cancellation);
                if (source.HasError())
                    return Output::Failure(std::move(source).ErrorValue());
                if (!source.Value().identity.exists)
                    return Output::Failure(SourceError(Assets::AssetSourceErrors::ReadFailed, index));
                result.identities.at(index) = source.Value().identity;
                result.documents.at(index) = std::move(source).Value();
            }
            return Output::Success(std::move(result));
        }

        /** @brief Complete definitions retain their shared ledger generation and cross-reference order. */
        struct ServiceDefinitions final {
            AchievementDefinitionRegistry achievements;
            StatDefinitionRegistry stats;
            LeaderboardDefinitionRegistry leaderboards;
            PresenceDefinitionRegistry presence;
        };

        /** @brief Projects all four documents in the existing achievement/stat/leaderboard/presence diagnostic order. */
        Result<ServiceDefinitions> DecodeDefinitions(const CapturedDocuments &sources, const PlatformStableIdRegistry &ids,
                                                     const PlatformServicesProjectValidationContext &context) {
            using Output = Result<ServiceDefinitions>;
            auto achievements = DecodePlatformAchievementDefinitions(Text(sources.documents.at(2)), ids, context.achievementLimits);
            if (achievements.HasError())
                return Output::Failure(std::move(achievements).ErrorValue());
            auto stats = DecodePlatformStatDefinitions(Text(sources.documents.at(3)), ids, context.definitionLimits);
            if (stats.HasError())
                return Output::Failure(std::move(stats).ErrorValue());
            auto leaderboards =
                DecodePlatformLeaderboardDefinitions(Text(sources.documents.at(4)), ids, stats.Value(), context.definitionLimits);
            if (leaderboards.HasError())
                return Output::Failure(std::move(leaderboards).ErrorValue());
            auto presence = DecodePlatformPresenceDefinitions(Text(sources.documents.at(5)), ids, context.definitionLimits);
            if (presence.HasError())
                return Output::Failure(std::move(presence).ErrorValue());
            return Output::Success(
                {std::move(achievements).Value(), std::move(stats).Value(), std::move(leaderboards).Value(), std::move(presence).Value()});
        }
    }  // namespace

    /** @copydoc PublishPlatformServicesProjectPolicy */
    Result<PlatformServicesProjectPolicyPublication> PublishPlatformServicesProjectPolicy(
        const PlatformServicesProjectSourceSnapshot &sources, const PlatformProjectConfiguration &configuration,
        const ExclusiveFileLock &lease, DurableFileSystem &files, std::function<Result<void>()> checkAvailable,
        const CancellationToken cancellation) {
        using Output = Result<PlatformServicesProjectPolicyPublication>;
        if (!checkAvailable || configuration.ProjectId() != sources.Configuration().ProjectId() ||
            configuration.Profile() != sources.Configuration().Profile())
            return Output::Failure(SourceError(Assets::AssetSourceErrors::Authority, 0));
        const auto destination = sources.ProjectRoot() / Paths.front();
        auto lockPath = Assets::AssetSourcePublicationLockPath(sources.ProjectRoot(), destination);
        if (lockPath.HasError())
            return Output::Failure(std::move(lockPath).ErrorValue());
        if (!lease.ProtectsPath(lockPath.Value()))
            return Output::Failure(SourceError(Assets::AssetSourceErrors::Authority, 0));
        const auto validate = [&]() -> Result<void> {
            auto available = checkAvailable();
            if (available.HasError())
                return available;
            return RevalidatePlatformServicesProjectSources(sources, cancellation);
        };
        if (auto current = validate(); current.HasError())
            return Output::Failure(std::move(current).ErrorValue());
        auto root = Capture(sources.ProjectRoot(), 0, cancellation);
        if (root.HasError())
            return Output::Failure(std::move(root).ErrorValue());
        if (root.Value().identity != sources.RootIdentity())
            return Output::Failure(SourceError(Assets::AssetSourceErrors::Conflict, 0));
        auto encoded = PreparePlatformServicesProjectPolicyEdit(Text(root.Value()), configuration);
        if (encoded.HasError())
            return Output::Failure(std::move(encoded).ErrorValue());
        const auto bytes = std::as_bytes(std::span{encoded.Value().data(), encoded.Value().size()});
        auto identities = sources.sources_;
        identities.front() = {.exists = true, .bytes = bytes.size(), .digest = ComputeSha256(bytes)};
        std::shared_ptr<const PlatformServicesProjectSourceSnapshot> next{
            new PlatformServicesProjectSourceSnapshot{sources.root_, identities, sources.ids_, sources.achievements_, sources.stats_,
                                                      sources.leaderboards_, sources.presence_, configuration}};
        PlatformServicesDetail::PublicationFiles fenced{files, validate};
        auto published =
            Assets::PublishAssetSource(sources.ProjectRoot(), destination, sources.RootIdentity(), bytes, lease, fenced, cancellation);
        if (published.HasError())
            return Output::Failure(std::move(published).ErrorValue());
        static_assert(std::is_nothrow_move_constructible_v<PlatformServicesProjectPolicyPublication>);
        return Output::Success({std::move(next), std::move(published).Value()});
    }

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
        auto captured = ReadDocuments(projectRoot, expectedProjectId, *admission.sourceFingerprint, cancellation);
        if (captured.HasError())
            return Output::Failure(std::move(captured).ErrorValue());
        auto sources = std::move(captured).Value();
        auto ids = DecodePlatformServicesLedger(Text(sources.documents.at(1)), expectedProjectId, sources.salt);
        if (ids.HasError())
            return Output::Failure(std::move(ids).ErrorValue());
        auto policy = ResolveProjectPlatformServicesConfiguration(sources.root, context.profile, context.contributions,
                                                                  context.trustedModules, context.policyLimits);
        if (policy.HasError())
            return Output::Failure(std::move(policy).ErrorValue());
        auto definitions = DecodeDefinitions(sources, ids.Value(), context);
        if (definitions.HasError())
            return Output::Failure(std::move(definitions).ErrorValue());
        auto models = std::move(definitions).Value();
        PlatformServicesProjectSourceSnapshot snapshot{projectRoot,
                                                       sources.identities,
                                                       std::move(ids).Value(),
                                                       std::move(models.achievements),
                                                       std::move(models.stats),
                                                       std::move(models.leaderboards),
                                                       std::move(models.presence),
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
            if (source.Value().identity != snapshot.sources_.at(index))
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
