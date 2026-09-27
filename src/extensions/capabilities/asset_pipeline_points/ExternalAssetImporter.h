#pragma once

#include "Horo/Assets/AssetImporter.h"
#include "Horo/Extensions/ExtensionAbi.h"
#include "Horo/Extensions/ExtensionManifest.h"
#include "Horo/Extensions/ExtensionPlatformProvider.h"
#include "Horo/Platform/DynamicLibrary.h"

#include <memory>
#include <string>
#include <vector>

namespace Horo::Extensions {
    /** @brief Shared library/module lease retained by every external contribution adapter. */
    struct ExtensionModuleLifetime final {
        ExtensionModuleLifetime() = default;
        ~ExtensionModuleLifetime();
        ExtensionModuleLifetime(const ExtensionModuleLifetime &) = delete;
        ExtensionModuleLifetime &operator=(const ExtensionModuleLifetime &) = delete;

        ExtensionModuleLifetime(ExtensionModuleLifetime &&other) noexcept
            : library(std::move(other.library)), moduleId(std::move(other.moduleId)), moduleApi(std::exchange(other.moduleApi, {})),
              unload(std::exchange(other.unload, nullptr)), loaded(std::exchange(other.loaded, false)) {}

        ExtensionModuleLifetime &operator=(ExtensionModuleLifetime &&other) noexcept {
            if (this != &other) {
                library = std::move(other.library);
                moduleId = std::move(other.moduleId);
                moduleApi = std::exchange(other.moduleApi, {});
                unload = std::exchange(other.unload, nullptr);
                loaded = std::exchange(other.loaded, false);
            }
            return *this;
        }

        /**
         * @brief Invokes the module unload callback at most once.
         * @return True when no callback was required or the callback completed without throwing.
         */
        [[nodiscard]] bool UnloadNow() noexcept;

        std::shared_ptr<Platform::DynamicLibrary> library;
        std::string moduleId;
        HoroExtensionModuleApi moduleApi{};
        HoroExtensionUnloadFunc unload{};
        bool loaded{};
    };

    /** @brief Host-side state used only during one module load transaction. */
    struct AssetImporterRegistrationSession final {
        const ExtensionManifest *manifest{};
        const ExtensionModuleManifest *extensionModule{};
        std::shared_ptr<ExtensionModuleLifetime> lifetime;
        std::vector<Assets::AssetImporterContribution> contributions;
        std::vector<ExtensionPlatformProviderCandidate> platformProviders;
        Error error;
        bool failed{};
    };

    /**
     * @brief Copies and validates one C ABI importer descriptor into a load transaction.
     * @param hostContext Pointer to AssetImporterRegistrationSession.
     * @param descriptor Borrowed module-owned descriptor.
     * @return C ABI status; failure leaves the live host catalog untouched.
     */
    HoroExtensionStatus RegisterExternalAssetImporter(void *hostContext, const HoroAssetImporterDescriptor *descriptor) noexcept;
}  // namespace Horo::Extensions
