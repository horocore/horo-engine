#pragma once

/**
 * @file ExtensionModuleResolution.h
 * @brief Pure deterministic resolution for modules inside one extension package.
 */

#include "Horo/Extensions/ExtensionManifest.h"
#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Extensions {
    /** @brief Selects which presentation authorities are available to a package activation attempt. */
    enum class ExtensionHostProfile : std::uint8_t {
        Headless,
        Interactive,
    };

    /** @brief Exact compatibility facts supplied by the host composition root. */
    struct ExtensionHostEnvironment {
        ExtensionHostProfile profile{ExtensionHostProfile::Interactive};          /**< Available presentation shape. */
        ExtensionHostPlatform platform{ExtensionHostPlatform::Count};             /**< Current operating system. */
        ExtensionHostArchitecture architecture{ExtensionHostArchitecture::Count}; /**< Current CPU architecture. */
        ExtensionBuildProfile buildProfile{ExtensionBuildProfile::Count};         /**< Current binary profile. */
        std::string_view engineVersion;                                           /**< Canonical running engine version. */
        std::uint32_t abiMajor{};                                                 /**< Generic extension ABI major version. */
        std::uint32_t abiMinor{};                                                 /**< Generic extension ABI minor version. */
        std::span<const std::string_view> capabilities; /**< Stable capability identities granted by composition. */
    };

    /** @brief Typed result of compatibility evaluation before executable loading. */
    enum class ExtensionCompatibilityStatus : std::uint8_t {
        Compatible,
        EngineVersionUnsupported,
        HostPlatformUnsupported,
        HostArchitectureUnsupported,
        BuildProfileUnsupported,
        AbiUnsupported,
        CapabilityUnavailable,
    };

    /** @brief Pure compatibility decision for one module and one explicit host. */
    struct ExtensionModuleCompatibility {
        ExtensionCompatibilityStatus status{ExtensionCompatibilityStatus::Compatible}; /**< Typed admission outcome. */
        std::string moduleId;                                                          /**< Module whose constraint was evaluated. */
        std::string requirement;   /**< Canonical unsupported requirement, when rejected. */
        std::string selectedEntry; /**< Selected package-relative entry when compatible. */

        /** @brief Tests whether every compatibility constraint was admitted. @return True only for a compatible module. */
        [[nodiscard]] bool IsCompatible() const noexcept {
            return status == ExtensionCompatibilityStatus::Compatible;
        }
    };

    /** @brief Explicit resolution state for one declared cross-module service import. */
    enum class ExtensionServiceImportStatus : std::uint8_t {
        Bound,
        Unavailable,
        Incompatible,
    };

    struct ExtensionModulePlan;

    /** @brief Unforgeable immutable service resolution selected from one validated package module graph. */
    class ResolvedExtensionServiceImport final {
    public:
        ResolvedExtensionServiceImport(const ResolvedExtensionServiceImport &) = default;
        ResolvedExtensionServiceImport &operator=(const ResolvedExtensionServiceImport &) = default;
        ResolvedExtensionServiceImport(ResolvedExtensionServiceImport &&) noexcept = default;
        ResolvedExtensionServiceImport &operator=(ResolvedExtensionServiceImport &&) noexcept = default;

        /** @brief Returns the package/extension that owns the consumer module. */
        [[nodiscard]] const std::string &ConsumerExtensionId() const noexcept;

        /** @brief Returns the module that declared and exclusively owns this import. */
        [[nodiscard]] const std::string &ConsumerModuleId() const noexcept;

        /** @brief Returns the stable import identity local to the consumer module. */
        [[nodiscard]] const std::string &ImportId() const noexcept;

        /** @brief Returns the exact exported service identity. */
        [[nodiscard]] const std::string &ServiceId() const noexcept;

        /** @brief Returns the exact typed callable contract identity. */
        [[nodiscard]] const std::string &ContractId() const noexcept;

        /** @brief Returns the selected provider module, or an empty string when unavailable. */
        [[nodiscard]] const std::string &ProviderModuleId() const noexcept;

        /** @brief Returns the selected canonical service API version, or an empty string when unavailable. */
        [[nodiscard]] const std::string &ProviderVersion() const noexcept;

        /** @brief Returns the explicit deterministic resolution outcome. */
        [[nodiscard]] ExtensionServiceImportStatus Status() const noexcept;

        /** @brief Returns whether a non-bound outcome rejects package composition. */
        [[nodiscard]] bool IsRequired() const noexcept;

    private:
        friend Result<ExtensionModulePlan> ResolveExtensionModules(const ExtensionManifest &manifest, const ExtensionHostEnvironment &host,
                                                                   std::span<const std::string> selectedModules);

        /** @brief Complete construction payload produced only by the validated module resolver. */
        struct Fields {
            std::string consumerExtensionId;
            std::string consumerModuleId;
            std::string importId;
            std::string serviceId;
            std::string contractId;
            std::string providerModuleId;
            std::string providerVersion;
            ExtensionServiceImportStatus status;
            bool required;
        };

        explicit ResolvedExtensionServiceImport(Fields fields);

        std::string consumerExtensionId_;
        std::string consumerModuleId_;
        std::string importId_;
        std::string serviceId_;
        std::string contractId_;
        std::string providerModuleId_;
        std::string providerVersion_;
        ExtensionServiceImportStatus status_{ExtensionServiceImportStatus::Unavailable};
        bool required_{true};
    };

    /** @brief Immutable deterministic module order and contribution ownership for one activation attempt. */
    struct ExtensionModulePlan {
        std::vector<std::string> moduleIds;                         /**< Dependency-first module identities. */
        std::vector<ExtensionContributionManifest> contributions;   /**< Contributions owned by selected modules. */
        std::vector<std::string> selectedEntries;                   /**< Entry selected for each module ID at the same index. */
        std::vector<ResolvedExtensionServiceImport> serviceImports; /**< Stable consumer/import ordered binding snapshot. */
    };

    /**
     * @brief Evaluates one module against explicit immutable host facts without loading code or mutating ambient state.
     * @param manifest Package metadata owning engine compatibility and legacy platform declarations.
     * @param module Module metadata whose ABI, capabilities and entry variants are evaluated.
     * @param host Exact host facts supplied by the application composition root.
     * @return Typed compatibility outcome naming the rejected constraint or selected entry.
     */
    [[nodiscard]] ExtensionModuleCompatibility EvaluateExtensionModuleCompatibility(const ExtensionManifest &manifest,
                                                                                    const ExtensionModuleManifest &module,
                                                                                    const ExtensionHostEnvironment &host);

    /**
     * @brief Resolves a fully parsed package manifest without loading code or mutating ambient state.
     * @param manifest Owned parsed package metadata whose module graph is resolved.
     * @param host Explicit host shape and compatibility facts; headless resolution excludes presentation-only modules.
     * @param selectedModules Optional explicit role roots; compatibility is checked only for their dependency closure. Empty selects all.
     * @return A stable dependency-first plan, or a typed extension error naming every involved module.
     */
    [[nodiscard]] Result<ExtensionModulePlan> ResolveExtensionModules(const ExtensionManifest &manifest,
                                                                      const ExtensionHostEnvironment &host,
                                                                      std::span<const std::string> selectedModules = {});
}  // namespace Horo::Extensions
