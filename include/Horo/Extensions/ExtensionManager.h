#pragma once

#include "Horo/Extensions/ExtensionManifest.h"
#include "Horo/Extensions/ExtensionModuleResolution.h"
#include "Horo/Extensions/ExtensionPlatformProvider.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/TransparentString.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "Horo/Security/ArtifactSignature.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Horo::Assets {
    class AssetImporterCatalog;
}

namespace Horo::Extensions {
    struct ExtensionModuleLifetime;

    /** @brief Represents a loaded extension instance. */
    struct LoadedExtension {
        ExtensionManifest manifest;
        std::vector<std::shared_ptr<ExtensionModuleLifetime>> lifetimes;
        std::vector<std::string> moduleIds;
        ExtensionPlatformProviderPublication platformProvider;
    };

    /**
     * @brief Manages loading and lifecycle of extensions.
     */
    class ExtensionManager {
    public:
        using NativeLibraryLoader = std::function<Result<std::unique_ptr<Platform::DynamicLibrary>>(const std::string &)>;

        /**
         * @brief Creates an extension manager bound to an optional unsealed importer catalog.
         * @param importerCatalog Host-owned candidate catalog receiving transactional asset.importer registrations.
         * @param hostProfile Explicit presentation shape available to extension modules.
         * @param hostCapabilities Stable capability identities granted by the host composition root. Invalid, duplicate, or excess
         *                         identities are omitted so module requirements fail closed.
         * @param artifactGate Mandatory host-composed integrity, trust, and signature gate. Null composition fails closed before native
         * load.
         * @param libraryLoader Explicit native loader boundary; empty uses the platform loader after security verification.
         * @param platformProviderCommit Optional host-owned commit for one staged provider-only extension.
         */
        explicit ExtensionManager(Assets::AssetImporterCatalog *importerCatalog = nullptr,
                                  ExtensionHostProfile hostProfile = ExtensionHostProfile::Interactive,
                                  std::vector<std::string> hostCapabilities = {},
                                  std::shared_ptr<const Security::NativeArtifactGate> artifactGate = {},
                                  NativeLibraryLoader libraryLoader = {}, ExtensionPlatformProviderCommit platformProviderCommit = {});
        ~ExtensionManager();
        ExtensionManager(const ExtensionManager &) = delete;
        ExtensionManager &operator=(const ExtensionManager &) = delete;
        ExtensionManager(ExtensionManager &&) noexcept;
        ExtensionManager &operator=(ExtensionManager &&) noexcept;

        /**
         * @brief Loads an extension from the given directory path.
         * @param extensionDir Path to the extension's root directory.
         * @return Result containing a reference to the loaded extension ID or an error.
         */
        Result<std::string> LoadExtension(const std::string &extensionDir);

        /**
         * @brief Unloads a specific extension by its ID.
         * @param extensionId The ID of the extension to unload.
         */
        void UnloadExtension(const std::string &extensionId);

        /**
         * @brief Unloads all currently loaded extensions.
         */
        void UnloadAll();

        /**
         * @brief Gets a list of all currently loaded extension IDs.
         * @return Vector of extension IDs.
         */
        std::vector<std::string> GetLoadedExtensionIds() const;

    private:
        Assets::AssetImporterCatalog *m_importerCatalog{};
        ExtensionHostProfile m_hostProfile{ExtensionHostProfile::Interactive};
        std::vector<std::string> m_hostCapabilities;
        std::shared_ptr<const Security::NativeArtifactGate> m_artifactGate;
        NativeLibraryLoader m_libraryLoader;
        ExtensionPlatformProviderCommit m_platformProviderCommit;
        TransparentStringMap<std::unique_ptr<LoadedExtension>> m_loadedExtensions;
    };

}  // namespace Horo::Extensions
