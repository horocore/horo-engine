#pragma once

#include "Horo/Extensions/ExtensionManifest.h"
#include "Horo/Extensions/ExtensionModuleResolution.h"
#include "Horo/Extensions/ExtensionPlatformProvider.h"
#include "Horo/Extensions/ExtensionRetirement.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/TransparentString.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "Horo/Security/ArtifactSignature.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Horo::Assets {
    class AssetImporterCatalog;
}

namespace Horo::Extensions {
    struct ExtensionModuleLifetime;
    class EditorActivityHost;

    /** @brief Represents a loaded extension instance. */
    struct LoadedExtension {
        ExtensionManifest manifest;
        std::vector<std::shared_ptr<ExtensionModuleLifetime>> lifetimes;
        std::vector<std::string> moduleIds;
        ExtensionPlatformProviderPublication platformProvider;
        std::shared_ptr<ExtensionRetirement> retirement;
        std::vector<std::string> providerExtensions;
        std::vector<std::shared_ptr<ExtensionExecutableLease>> providerLeases;
        bool retiring{};
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
         * @param editorActivityHost Explicit editor surface authority; omitted hosts reject ABI editor publication.
         */
        explicit ExtensionManager(Assets::AssetImporterCatalog *importerCatalog = nullptr,
                                  ExtensionHostProfile hostProfile = ExtensionHostProfile::Interactive,
                                  std::vector<std::string> hostCapabilities = {},
                                  std::shared_ptr<const Security::NativeArtifactGate> artifactGate = {},
                                  NativeLibraryLoader libraryLoader = {}, ExtensionPlatformProviderCommit platformProviderCommit = {},
                                  std::shared_ptr<EditorActivityHost> editorActivityHost = {});
        /** @brief Withdraws publications on the owner lane; busy native closures require process restart. */
        ~ExtensionManager();
        ExtensionManager(const ExtensionManager &) = delete;
        ExtensionManager &operator=(const ExtensionManager &) = delete;
        ExtensionManager(ExtensionManager &&) = delete;
        ExtensionManager &operator=(ExtensionManager &&) = delete;

        /**
         * @brief Loads an extension from the given directory path.
         * @param extensionDir Path to the extension's root directory.
         * @param providerExtensions Package-authority-resolved provider extensions, already activated by this host.
         * @return Result containing a reference to the loaded extension ID or an error.
         */
        Result<std::string> LoadExtension(const std::string &extensionDir, std::span<const std::string> providerExtensions = {});

        /**
         * @brief Begins terminal retirement of a specific extension; never forcibly unloads held code.
         * @param extensionId The ID of the extension to unload.
         */
        void UnloadExtension(const std::string &extensionId);

        /**
         * @brief Closes admission, withdraws contributions and releases manager leases in reverse dependency order.
         * @param extensionId Exact loaded or retiring extension identity.
         * @return Attributed shutdown result; Draining requires releasing outstanding owners or restarting.
         */
        [[nodiscard]] ExtensionRetirementReport RetireExtension(const std::string &extensionId);

        /**
         * @brief Returns the retirement owner for explicit host contribution/work registration.
         * @param extensionId Exact extension identity.
         * @return Shared retirement owner, or null for an unknown extension.
         * @details Host composition must register callback, job, resource, surface and service ownership here
         * before exposing module code outside the native ABI's built-in importer/provider adapters.
         */
        [[nodiscard]] std::shared_ptr<ExtensionRetirement> Retirement(const std::string &extensionId) const;

        /**
         * @brief Finalizes drained modules on the same host owner lane used for load and retirement.
         * @details Worker-thread last-lease release never invokes native unload. Releasing the manager with
         * outstanding work retains native records for process lifetime and requires restart. The activation
         * limit is per manager, not a global quarantine budget; do not recreate failed hosts to bypass restart.
         */
        void FinalizeRetirements();

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
        std::shared_ptr<EditorActivityHost> m_editorActivityHost;
        TransparentStringMap<std::unique_ptr<LoadedExtension>> m_loadedExtensions;
        std::vector<std::string> m_activationOrder;
        /** @brief Reports owner-lane activation capacity; retained records count against the same per-manager bound. */
        [[nodiscard]] bool HasActivationCapacity(std::size_t providerCount) const noexcept;
        [[nodiscard]] Result<void> AcquireProviderDependencies(std::span<const std::string> providers, std::string_view extensionId,
                                                               std::vector<std::shared_ptr<ExtensionModuleLifetime>> &dependencies,
                                                               std::vector<std::shared_ptr<ExtensionExecutableLease>> &leases) const;
        void RetireRecord(const std::string &extensionId);
        void CloseForDestruction() noexcept;
    };

}  // namespace Horo::Extensions
