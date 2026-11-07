#pragma once

/**
 * @file ExtensionManifest.h
 * @brief Bounded extension manifest parsing and validated package metadata.
 */

#include "Horo/Foundation/Result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Extensions {
    /** @brief Resource limits applied before an untrusted manifest can reach extension resolution. */
    struct ExtensionManifestLimits {
        std::size_t maximumDocumentBytes{64U * 1024U}; /**< Maximum encoded JSON document size. */
        std::size_t maximumNestingDepth{16};           /**< Maximum number of nested JSON containers. */
        std::size_t maximumObjectMembers{512};         /**< Maximum object members across the document. */
        std::size_t maximumArrayElements{512};         /**< Maximum array elements across the document. */
        std::size_t maximumStringBytes{4U * 1024U};    /**< Maximum decoded bytes in one general string. */
        std::size_t maximumIdentifierBytes{256};       /**< Maximum decoded bytes in one stable identity. */
        std::size_t maximumModules{64};                /**< Maximum modules declared by one package. */
        std::size_t maximumContributions{256};         /**< Maximum contributions declared by one package. */
        std::size_t maximumPlatforms{32};              /**< Maximum compatibility platforms. */
    };

    /** @brief Explicit authority granted to one extension module. */
    enum class ExtensionModuleRole : std::uint8_t {
        BackendCapability,
        EditorPresentation,
        HeadlessTooling,
        ScriptProvider,
        RuntimeParticipant,
    };

    /** @brief Host operating systems addressable by a native extension entry. */
    enum class ExtensionHostPlatform : std::uint8_t {
        Windows,
        MacOS,
        Linux,
        Count,
    };

    /** @brief CPU architectures addressable by a native extension entry. */
    enum class ExtensionHostArchitecture : std::uint8_t {
        X86_64,
        Arm64,
        Count,
    };

    /** @brief Binary compatibility profiles addressable by a native extension entry. */
    enum class ExtensionBuildProfile : std::uint8_t {
        Debug,
        Release,
        Count,
    };

    /** @brief One exact native artifact selector owned by an extension module. */
    struct ExtensionNativeEntryManifest {
        ExtensionHostPlatform platform{ExtensionHostPlatform::Count};             /**< Required host operating system. */
        ExtensionHostArchitecture architecture{ExtensionHostArchitecture::Count}; /**< Required host CPU architecture. */
        ExtensionBuildProfile buildProfile{ExtensionBuildProfile::Count};         /**< Required host binary profile. */
        std::string entry;                                                        /**< Safe package-relative native entry name. */
    };

    /** @brief Generic host ABI requirement declared by one native extension module. */
    struct ExtensionAbiRequirement {
        std::uint32_t major{1};       /**< Required ABI major version. */
        std::uint32_t minimumMinor{}; /**< Minimum append-only ABI minor version. */
    };

    /** @brief One module-owned, backend-neutral service contract export. */
    struct ExtensionServiceExportManifest {
        std::string id;       /**< Stable service export identity. */
        std::string contract; /**< Stable callable contract identity. */
        std::string version;  /**< Canonical semantic API version. */
    };

    /** @brief One service contract required by a module. */
    struct ExtensionServiceImportManifest {
        std::string id;             /**< Stable import identity local to the module. */
        std::string service;        /**< Required service export identity. */
        std::string contract;       /**< Required callable contract identity. */
        std::string minimumVersion; /**< Minimum compatible semantic API version. */
        bool required{true};        /**< Whether absence or incompatibility rejects module composition. */
    };

    /** @brief One versioned native or declarative module exported by an extension package. */
    struct ExtensionModuleManifest {
        std::string id;                                      /**< Stable module identity. */
        std::string version;                                 /**< Canonical semantic module version. */
        std::string kind;                                    /**< Host-defined module kind. */
        std::string entry;                                   /**< Optional package-relative native entry name. */
        std::vector<ExtensionModuleRole> roles;              /**< Explicit module authorities; empty only for decoded legacy input. */
        std::vector<std::string> dependencies;               /**< Required local module identities. */
        std::vector<ExtensionServiceExportManifest> exports; /**< Services owned by this module. */
        std::vector<ExtensionServiceImportManifest> imports; /**< Services consumed by this module. */
        std::optional<ExtensionAbiRequirement> abi;          /**< Required generic host ABI range; absent for legacy compatibility input. */
        std::vector<ExtensionNativeEntryManifest> entries;   /**< Exact native artifact variants; mutually exclusive with legacy entry. */
        std::vector<std::string> requiredCapabilities;       /**< Stable host capability identities required before library load. */
    };

    /** @brief One manifest-declared contribution bound to a module in the same package. */
    struct ExtensionContributionManifest {
        std::string type;         /**< Typed extension point, for example `asset.importer`. */
        std::string id;           /**< Stable globally unique contribution identity. */
        std::string owningModule; /**< Owning module identity from this manifest. */
        std::string resource;     /**< Package-relative JSON resource; required only for editor.theme contributions. */
    };

    /** @brief Represents a parsed extension.json manifest. */
    struct ExtensionManifest {
        std::uint32_t schemaVersion{1};
        std::string id;
        std::string version;
        std::string kind;
        std::string displayName;
        std::string description;
        std::string author;

        std::string engineMin;
        std::string engineMax;
        std::string sdkAbi;

        std::vector<std::string> platforms;
        std::vector<ExtensionModuleManifest> modules;
        std::vector<ExtensionContributionManifest> contributions;

        /** @brief The path to the directory containing this manifest. */
        std::string rootPath;
    };

    /**
     * @brief Parses and validates an untrusted extension.json document without loading extension code.
     * @param jsonContent Encoded JSON content; the returned manifest owns every decoded value.
     * @param limits Resource limits enforced during syntax and schema decoding.
     * @return Validated manifest, or an error whose diagnostic owns the exact JSON field path in `Diagnostic::path`.
     * @details Diagnostic paths use dot notation for identifier keys, bracket-quoted JSON strings for all other keys,
     *          and numeric brackets for array elements.
     */
    [[nodiscard]] Result<ExtensionManifest> ParseExtensionManifest(std::string_view jsonContent,
                                                                   const ExtensionManifestLimits &limits = {});

}  // namespace Horo::Extensions
