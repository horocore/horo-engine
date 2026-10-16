#include "Horo/Application/NavigationProjectSettingsStore.h"

#include <limits>
#include <new>

namespace Horo::Application {
    namespace {
        /** @brief Stable exact-publication rejection. */
        Error Reject(const Navigation::NavigationSettingsFailure reason) {
            return MakeError(Navigation::NavigationSettingsError(reason));
        }
    }  // namespace

    /** @copydoc NavigationProjectSettingsStore::NavigationProjectSettingsStore */
    NavigationProjectSettingsStore::NavigationProjectSettingsStore(DurableFileSystem &files, std::filesystem::path projectRoot,
                                                                   std::filesystem::path path)
        : files_(files), root_(std::move(projectRoot)), path_(std::move(path)) {}

    /** @copydoc NavigationProjectSettingsStore::Load */
    Result<NavigationSettingsFileSnapshot> NavigationProjectSettingsStore::Load() const {
        try {
            if (!root_.is_absolute() || root_ != root_.lexically_normal() || path_ != root_ / ".horo/navigation.json")
                return Result<NavigationSettingsFileSnapshot>::Failure(Reject(Navigation::NavigationSettingsFailure::Invalid));
            auto source = Assets::CaptureAssetSource(root_, path_, 64 * 1024);
            if (source.HasError())
                return Result<NavigationSettingsFileSnapshot>::Failure(source.ErrorValue());
            if (!source.Value().identity.exists)
                return Result<NavigationSettingsFileSnapshot>::Failure(Reject(Navigation::NavigationSettingsFailure::Invalid));
            const auto &bytes = source.Value().bytes;
            auto policy = Application::DecodeNavigationProjectSettings({reinterpret_cast<const char *>(bytes.data()), bytes.size()});
            if (policy.HasError())
                return Result<NavigationSettingsFileSnapshot>::Failure(policy.ErrorValue());
            return Result<NavigationSettingsFileSnapshot>::Success(
                {std::make_shared<const Application::NavigationProjectSettings>(std::move(policy).Value()), source.Value().identity});
        } catch (const std::bad_alloc &) {
            return Result<NavigationSettingsFileSnapshot>::Failure(Reject(Navigation::NavigationSettingsFailure::Capacity));
        }
    }

    /** @copydoc NavigationProjectSettingsStore::Save */
    Result<NavigationSettingsPublication> NavigationProjectSettingsStore::Save(
        const std::optional<NavigationSettingsFileSnapshot> &expected, const Application::NavigationProjectSettings &candidate) {
        try {
            using Return = Result<NavigationSettingsPublication>;
            if (!root_.is_absolute() || root_ != root_.lexically_normal() || path_ != root_ / ".horo/navigation.json")
                return Return::Failure(Reject(Navigation::NavigationSettingsFailure::Invalid));
            if ((expected && (!expected->settings || expected->settings->Input().revision == UINT64_MAX)) ||
                candidate.Input().revision != (expected ? expected->settings->Input().revision + 1 : 1))
                return Return::Failure(Reject(Navigation::NavigationSettingsFailure::Invalid));
            if (expected) {
                const auto successor =
                    Navigation::NavigationProjectProfile::Replace(expected->settings->RuntimeProfile(), candidate.Input().runtimeProfile);
                if (successor.HasError())
                    return Return::Failure(successor.ErrorValue());
            }
            auto lockPath = Assets::AssetSourcePublicationLockPath(root_, path_);
            if (lockPath.HasError())
                return Return::Failure(lockPath.ErrorValue());
            auto lock = files_.TryAcquireExclusive(lockPath.Value(), "horo.navigation.settings");
            if (lock.HasError())
                return Return::Failure(lock.ErrorValue());
            const auto text = Application::EncodeNavigationProjectSettings(candidate);
            // Reserve the entire receipt before the irreversible native rename; uncertain durability is explicit.
            auto settings = std::make_shared<const Application::NavigationProjectSettings>(candidate);
            auto published = Assets::PublishAssetSource(root_, path_, expected ? expected->identity : Assets::AssetSourceIdentity{},
                                                        std::as_bytes(std::span{text.data(), text.size()}), lock.Value(), files_);
            if (published.HasError())
                return Return::Failure(published.ErrorValue());
            auto receipt = std::move(published).Value();
            return Return::Success({{std::move(settings), receipt.identity}, std::move(receipt.durabilityError)});
        } catch (const std::bad_alloc &) {
            return Result<NavigationSettingsPublication>::Failure(Reject(Navigation::NavigationSettingsFailure::Capacity));
        }
    }
}  // namespace Horo::Application
