#pragma once

#include "Horo/Assets/AssetImporter.h"
#include "Horo/Extensions/ExtensionAbi.h"
#include "Horo/Extensions/ExtensionManifest.h"
#include "Horo/Extensions/ExtensionPlatformProvider.h"
#include "Horo/Extensions/ExtensionRetirement.h"
#include "Horo/Platform/DynamicLibrary.h"

#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace Horo::Extensions {
    struct ExtensionModuleLifetime;
    struct EditorActivitySession;
    class EditorActivityHost;

    /** @brief Preallocated native closure retained if module teardown fails; never allocated during destruction. */
    struct ExtensionNativeClosure final {
        std::vector<std::shared_ptr<ExtensionModuleLifetime>> dependencies;
        std::shared_ptr<Platform::DynamicLibrary> library;
    };

    /** @brief Shared library/module lease retained by every external contribution adapter. */
    struct ExtensionModuleLifetime final {
        ExtensionModuleLifetime() = default;
        ~ExtensionModuleLifetime();
        ExtensionModuleLifetime(const ExtensionModuleLifetime &) = delete;
        ExtensionModuleLifetime &operator=(const ExtensionModuleLifetime &) = delete;

        ExtensionModuleLifetime(ExtensionModuleLifetime &&) = delete;
        ExtensionModuleLifetime &operator=(ExtensionModuleLifetime &&) = delete;

        /**
         * @brief Invokes the module unload callback at most once.
         * @return True when no callback was required or the callback completed without throwing.
         * A prior teardown failure remains false on every subsequent call; native closure retention is restart-only.
         */
        [[nodiscard]] bool UnloadNow() noexcept;

        std::unique_ptr<ExtensionNativeClosure> code{std::make_unique<ExtensionNativeClosure>()};
        std::string moduleId;
        HoroExtensionModuleApi moduleApi{};
        HoroExtensionUnloadFunc unload{};
        bool loaded{};
        bool teardownFailed{};
        std::thread::id ownerThread{std::this_thread::get_id()};
        std::weak_ptr<ExtensionRetirement> retirement;
        std::vector<std::shared_ptr<EditorActivitySession>> editorActivities;
    };

    /** @brief Host-side state used only during one module load transaction. */
    struct AssetImporterRegistrationSession final {
        const ExtensionManifest *manifest{};
        const ExtensionModuleManifest *extensionModule{};
        std::shared_ptr<ExtensionModuleLifetime> lifetime;
        std::shared_ptr<ExtensionRetirement> retirement;
        std::vector<Assets::AssetImporterContribution> contributions;
        std::vector<ExtensionPlatformProviderCandidate> platformProviders;
        Error error;
        bool failed{};
        std::shared_ptr<EditorActivityHost> editorHost;
        std::uint64_t editorGeneration{};
    };

    /**
     * @brief Copies and validates one C ABI importer descriptor into a load transaction.
     * @param hostContext Pointer to AssetImporterRegistrationSession.
     * @param descriptor Borrowed module-owned descriptor.
     * @return C ABI status; failure leaves the live host catalog untouched.
     */
    HoroExtensionStatus RegisterExternalAssetImporter(void *hostContext, const HoroAssetImporterDescriptor *descriptor) noexcept;
}  // namespace Horo::Extensions
