#pragma once
#include "Horo/Extensions/EditorPageHost.h"
#include "Horo/Platform/ConfigurationFileStore.h"

namespace Horo::Editor {
    /** @brief Actual editor composition adapter to the canonical durable configuration store; providers never receive this object. */
    class EditorPageFileStore final : public Extensions::IEditorPageStore {
    public:
        /** @brief Borrows application-owned durable files and captures the explicit resolved user configuration directory. */
        EditorPageFileStore(DurableFileSystem &files, std::filesystem::path userRoot);
        /** @brief Changes the actual project/workspace root and fences every admitted draft, even when returning to the same project later.
         */
        void SetProject(std::filesystem::path root);
        /** @copydoc Extensions::IEditorPageStore::ContextRevision */
        std::uint64_t ContextRevision(SettingScope scope) const noexcept override;
        /** @copydoc Extensions::IEditorPageStore::Read */
        Result<std::optional<std::string>> Read(const Extensions::EditorSurfaceProviderIdentity &provider, std::string_view id,
                                                SettingScope scope) override;
        /** @copydoc Extensions::IEditorPageStore::Write */
        Result<void> Write(const Extensions::EditorSurfaceProviderIdentity &provider, std::string_view id, SettingScope scope,
                           std::uint64_t revision, const ConfigurationSnapshot &snapshot) override;

    private:
        /** @brief Derives one stable collision-resistant filename under an explicit scoped host root, never a provider-supplied path. */
        Result<std::filesystem::path> Path(const Extensions::EditorSurfaceProviderIdentity &provider, std::string_view id,
                                           SettingScope scope) const;
        ConfigurationFileStore store_;
        std::filesystem::path userRoot_;
        std::filesystem::path projectRoot_;
        std::uint64_t projectRevision_{1};
    };
}  // namespace Horo::Editor
