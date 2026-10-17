#include "Horo/Editor/ProjectCollisionSettingsStore.h"

#include "CollisionSourceInventory.h"

#include <new>

namespace Horo::Editor {
    namespace {
        const ErrorCodeDescriptor Invalid{.domain = ErrorDomainId{"horo.project.collision"},
                                          .code = ErrorCode{"collision.save.conflict"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "The collision settings save is stale or invalid.",
                                          .remediationHint = "Reload the exact project document and review the edit before saving.",
                                          .userActionable = true};

        /** @brief Revalidates the complete reviewed inventory and physical source identities while the caller holds the project lease. */
        Result<void> ValidateReviewedSources(const std::filesystem::path &root, const Assets::IAssetRegistrySource &source,
                                             const std::optional<CollisionPublicationSourceFence> &reviewed) {
            if (reviewed) {
                if (auto physical = ValidateCollisionSourceInventory(source, reviewed->inventory); physical.HasError())
                    return Result<void>::Failure(physical.ErrorValue());
            }
            const std::span<const CollisionSourceFence> sources = reviewed ? reviewed->sources : std::span<const CollisionSourceFence>{};
            for (const auto &fence : sources) {
                const auto current = Assets::CaptureAssetSource(root, fence.path, 16 * 1024 * 1024);
                if (current.HasError())
                    return Result<void>::Failure(current.ErrorValue());
                if (current.Value().identity != fence.identity)
                    return Result<void>::Failure(MakeError(Invalid, "A reviewed collision reference source changed before save."));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ProjectCollisionSettingsStore::ProjectCollisionSettingsStore */
    ProjectCollisionSettingsStore::ProjectCollisionSettingsStore(DurableFileSystem &files, ProjectMutationCoordinator &mutations,
                                                                 std::filesystem::path root, std::string projectId,
                                                                 const Assets::IAssetRegistrySource &source)
        : files_(files), mutations_(mutations), root_(std::move(root)), projectId_(std::move(projectId)), source_(source) {}

    /** @copydoc ProjectCollisionSettingsStore::Load */
    Result<Application::ProjectCollisionFileSnapshot> ProjectCollisionSettingsStore::Load() const {
        return Application::LoadProjectCollisionSettings(root_, projectId_);
    }

    /** @copydoc ProjectCollisionSettingsStore::Save */
    Result<ProjectCollisionPublication> ProjectCollisionSettingsStore::Save(
        const std::optional<Application::ProjectCollisionFileSnapshot> &expected, const Application::ProjectCollisionSettings &candidate,
        const std::optional<CollisionPublicationSourceFence> &reviewed) const {
        try {
            using Return = Result<ProjectCollisionPublication>;
            if (candidate.Input().projectId != projectId_ ||
                (expected && (!expected->identity.exists || !expected->settings || expected->settings->Input().projectId != projectId_ ||
                              expected->settings->Input().revision == UINT64_MAX)) ||
                candidate.Input().revision != (expected ? expected->settings->Input().revision + 1 : 1))
                return Return::Failure(MakeError(Invalid, "Project identity and consecutive revision must match the accepted source."));
            if (auto projectLease = mutations_.TryAcquire({root_, ProjectMutationOwner::Save, "collision.settings"});
                projectLease.HasError())
                return Return::Failure(projectLease.ErrorValue());
            if (auto validated = ValidateReviewedSources(root_, source_, reviewed); validated.HasError())
                return Return::Failure(validated.ErrorValue());
            const auto path = root_ / ".horo/collision.json";
            auto lockPath = Assets::AssetSourcePublicationLockPath(root_, path);
            if (lockPath.HasError())
                return Return::Failure(lockPath.ErrorValue());
            auto lease = files_.TryAcquireExclusive(lockPath.Value(), "horo.collision.settings");
            if (lease.HasError())
                return Return::Failure(lease.ErrorValue());
            auto text = Application::EncodeProjectCollisionSettings(candidate);
            if (text.HasError())
                return Return::Failure(text.ErrorValue());
            auto settings = std::make_shared<const Application::ProjectCollisionSettings>(candidate);
            auto uncertain =
                MakeError(Invalid, "Replacement occurred without confirmed directory durability; reconcile before claiming saved.");
            auto publication =
                Assets::PublishAssetSource(root_, path, expected ? expected->identity : Assets::AssetSourceIdentity{},
                                           std::as_bytes(std::span{text.Value().data(), text.Value().size()}), lease.Value(), files_);
            if (publication.HasError())
                return Return::Failure(publication.ErrorValue());
            auto receipt = std::move(publication).Value();
            if (receipt.status != Assets::AssetSourcePublicationStatus::Durable && !receipt.durabilityError)
                receipt.durabilityError = std::move(uncertain);
            return Return::Success({{std::move(settings), receipt.identity}, std::move(receipt.durabilityError)});
        } catch (const std::bad_alloc &) {
            return Result<ProjectCollisionPublication>::Failure(MakeError(Invalid, "Collision save capacity is unavailable."));
        }
    }
}  // namespace Horo::Editor
