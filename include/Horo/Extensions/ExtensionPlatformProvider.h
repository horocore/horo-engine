#pragma once

/**
 * @file ExtensionPlatformProvider.h
 * @brief Copied platform-provider ABI candidate handed from ExtensionHost to application composition.
 */

#include "Horo/Extensions/ExtensionAbi.h"
#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Horo::Extensions {
    /** @brief Inert copied claim and private factory table from one verified loaded module. */
    struct ExtensionPlatformProviderCandidate final {
        std::string extensionId;
        std::string moduleId;
        std::string providerKey;
        std::uint64_t providerId{};
        std::uint32_t platformMask{};
        std::uint32_t profileMask{};
        std::uint32_t serviceMask{};
        std::uint32_t interfaceMajor{};
        std::uint32_t interfaceMinor{};
        std::uint32_t contractMajor{};
        std::uint32_t contractMinor{};
        std::uint32_t contractPatch{};
        std::vector<std::string> permissions;
        void *factoryContext{};
        HoroPlatformProviderCreateFunc createCandidate{};
        HoroPlatformProviderRetireFunc retireCandidate{};
        HoroPlatformProviderDestroyFunc destroyCandidate{};
        HoroPlatformProviderOperations operations{}; /**< Empty for legacy factory-only profile. */
        std::shared_ptr<void> moduleCodeLease;       /**< Holds native code through candidate retirement. */
    };

    /** @brief Host-owned publication revoked before the manager releases the module lease. */
    class IExtensionPlatformProviderPublication {
    public:
        virtual ~IExtensionPlatformProviderPublication() = default;
        virtual void Revoke() noexcept = 0;
    };

    /** @brief Deleter that makes publication rollback automatic on every unwind path. */
    struct ExtensionPlatformProviderPublicationDeleter final {
        void operator()(IExtensionPlatformProviderPublication *publication) const noexcept {
            if (publication != nullptr) {
                publication->Revoke();
                delete publication;
            }
        }
    };

    using ExtensionPlatformProviderPublication =
        std::unique_ptr<IExtensionPlatformProviderPublication, ExtensionPlatformProviderPublicationDeleter>;

    /** @brief Composition callback that validates and publishes one staged provider generation. */
    using ExtensionPlatformProviderCommit = std::function<Result<ExtensionPlatformProviderPublication>(ExtensionPlatformProviderCandidate)>;
}  // namespace Horo::Extensions
