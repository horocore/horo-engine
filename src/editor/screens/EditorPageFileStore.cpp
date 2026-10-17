#include "EditorPageFileStore.h"

#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Platform/PlatformErrors.h"

#include <format>
#include <limits>

namespace Horo::Editor {
    namespace {
        /** @brief Rejects filesystem redirects and aliases below the already selected host-owned root; absence remains a distinct read
         * result. */
        Result<bool> ExistingFile(const std::filesystem::path &path) {
            std::error_code error;
            auto current = path.root_path();
            for (const auto &component : path.relative_path()) {
                current /= component;
                const auto status = std::filesystem::symlink_status(current, error);
                if (error == std::errc::no_such_file_or_directory)
                    return Result<bool>::Success(false);
                if (error || std::filesystem::is_symlink(status))
                    return Result<bool>::Failure(MakeError(PlatformErrors::ConfigurationReadFailed,
                                                           "Extension configuration contains an unreadable or redirected path."));
            }
            const bool regular = std::filesystem::is_regular_file(path, error);
            if (!regular || error)
                return Result<bool>::Failure(
                    MakeError(PlatformErrors::ConfigurationReadFailed, "Extension configuration must be one private regular file."));
            const auto links = std::filesystem::hard_link_count(path, error);
            if (links != 1 || error)
                return Result<bool>::Failure(
                    MakeError(PlatformErrors::ConfigurationReadFailed, "Extension configuration must be one private regular file."));
            return Result<bool>::Success(true);
        }
    }  // namespace

    /** @copydoc EditorPageFileStore::EditorPageFileStore */
    EditorPageFileStore::EditorPageFileStore(DurableFileSystem &files, std::filesystem::path root)
        : store_(files), userRoot_(std::move(root)) {}

    /** @copydoc EditorPageFileStore::SetProject */
    void EditorPageFileStore::SetProject(std::filesystem::path root) {
        if (root == projectRoot_)
            return;
        projectRoot_ = std::move(root);
        if (projectRevision_ == std::numeric_limits<std::uint64_t>::max())
            projectRevision_ = 0;
        else if (projectRevision_ != 0)
            ++projectRevision_;
    }

    /** @copydoc EditorPageFileStore::ContextRevision */
    std::uint64_t EditorPageFileStore::ContextRevision(SettingScope scope) const noexcept {
        using enum SettingScope;
        if (scope == User)
            return userRoot_.empty() ? 0 : 1;
        if (scope == Project || scope == Workspace)
            return projectRoot_.empty() ? 0 : projectRevision_;
        return 0;
    }

    /** @copydoc EditorPageFileStore::Path */
    Result<std::filesystem::path> EditorPageFileStore::Path(const Extensions::EditorSurfaceProviderIdentity &provider, std::string_view id,
                                                            SettingScope scope) const {
        if (ContextRevision(scope) == 0 || provider.extensionId.empty() || provider.moduleId.empty() || id.empty())
            return Result<std::filesystem::path>::Failure(MakeError(Extensions::ExtensionErrors::CapabilityUnavailable));
        const std::string identity = std::format("{}:{}{}:{}{}:{}", provider.extensionId.size(), provider.extensionId,
                                                 provider.moduleId.size(), provider.moduleId, id.size(), id);
        const auto filename = FormatSha256(ComputeSha256(std::as_bytes(std::span{identity}))).substr(7) + ".json";
        auto root = userRoot_;
        if (scope == SettingScope::Project)
            root = projectRoot_ / ".horo/settings";
        else if (scope != SettingScope::User)
            root = projectRoot_ / ".horo/local";
        if (!root.is_absolute())
            return Result<std::filesystem::path>::Failure(MakeError(Extensions::ExtensionErrors::CapabilityUnavailable));
        return Result<std::filesystem::path>::Success(root / "extensions" / filename);
    }

    /** @copydoc EditorPageFileStore::Read */
    Result<std::optional<std::string>> EditorPageFileStore::Read(const Extensions::EditorSurfaceProviderIdentity &provider,
                                                                 std::string_view id, SettingScope scope) {
        auto path = Path(provider, id, scope);
        if (path.HasError())
            return Result<std::optional<std::string>>::Failure(path.ErrorValue());
        auto exists = ExistingFile(path.Value());
        if (exists.HasError())
            return Result<std::optional<std::string>>::Failure(exists.ErrorValue());
        if (!exists.Value())
            return Result<std::optional<std::string>>::Success({});
        auto document = store_.Read(path.Value(), {.maximumDocumentBytes = 128 * 1024});
        if (document.HasError())
            return Result<std::optional<std::string>>::Failure(document.ErrorValue());
        return Result<std::optional<std::string>>::Success(std::move(document).Value());
    }

    /** @copydoc EditorPageFileStore::Write */
    Result<void> EditorPageFileStore::Write(const Extensions::EditorSurfaceProviderIdentity &provider, std::string_view id,
                                            SettingScope scope, std::uint64_t revision, const ConfigurationSnapshot &snapshot) {
        if (revision == 0 || revision != ContextRevision(scope))
            return Result<void>::Failure(MakeError(Extensions::ExtensionErrors::CapabilityRevoked));
        auto path = Path(provider, id, scope);
        if (path.HasError())
            return Result<void>::Failure(path.ErrorValue());
        if (auto exists = ExistingFile(path.Value()); exists.HasError())
            return Result<void>::Failure(exists.ErrorValue());
        std::error_code error;
        std::filesystem::create_directories(path.Value().parent_path(), error);
        if (error)
            return Result<void>::Failure(MakeError(PlatformErrors::ConfigurationWriteFailed,
                                                   "Cannot create the selected extension configuration directory: " + error.message()));
        // Recheck the completed path before the canonical durable store acquires its lock and publishes.
        if (const auto checked = ExistingFile(path.Value()); checked.HasError())
            return Result<void>::Failure(checked.ErrorValue());
        return store_.Write(path.Value(), snapshot, {.maximumDocumentBytes = 128 * 1024});
    }
}  // namespace Horo::Editor
