#pragma once

/**
 * @file ScriptExportDescriptor.h
 * @brief Inert language-neutral script API declarations and immutable generations.
 */

#include "Horo/Extensions/ExtensionCapabilityAdmission.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Extensions {
    /** @brief Semantic version of one script API or API member. */
    struct ScriptExportVersion final {
        std::uint32_t major{}; /**< Breaking contract generation. */
        std::uint32_t minor{}; /**< Additive contract generation. */
        std::uint32_t patch{}; /**< Behaviour-compatible correction generation. */

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return *this != ScriptExportVersion{};
        }

        constexpr auto operator<=>(const ScriptExportVersion &) const noexcept = default;
    };

    /** @brief Closed same-major version interval accepted by one script API generation. */
    struct ScriptExportVersionRange final {
        ScriptExportVersion minimum{}; /**< Oldest compatible API generation. */
        ScriptExportVersion maximum{}; /**< Newest compatible API generation. */

        constexpr auto operator<=>(const ScriptExportVersionRange &) const noexcept = default;
    };

    /** @brief Primitive values available without a runtime-specific type declaration. */
    enum class ScriptExportPrimitiveKind : std::uint8_t {
        Boolean,
        SignedInteger,
        UnsignedInteger,
        Number,
        String,
        Bytes,
        Handle,
        Count,
    };

    /** @brief Nullability policy carried by every script-visible value reference. */
    enum class ScriptExportNullability : std::uint8_t {
        NonNull,
        Nullable,
        Count,
    };

    /** @brief Closed set of named language-neutral type declarations. */
    enum class ScriptExportNamedTypeKind : std::uint8_t {
        Enum,
        Struct,
        Array,
        Map,
        Count,
    };

    /** @brief Requiredness policy for a function parameter or struct field. */
    enum class ScriptExportParameterRequirement : std::uint8_t {
        Required,
        Optional,
        Count,
    };

    /** @brief Host-owned scheduling policy exposed to a runtime adapter. */
    enum class ScriptExportInvocationMode : std::uint8_t {
        Synchronous,
        Asynchronous,
        Count,
    };

    /** @brief Bounded, presentation-neutral documentation metadata. */
    struct ScriptExportDocumentation final {
        std::string summary;       /**< Short human-readable summary. */
        std::string description;   /**< Optional longer description. */
        std::string replacementId; /**< Optional stable replacement identity for deprecated declarations. */
        bool deprecated{};         /**< Whether consumers should migrate away from this declaration. */

        bool operator==(const ScriptExportDocumentation &) const noexcept = default;
    };

    /**
     * @brief Language-neutral reference to a primitive or named type.
     *
     * A named type is resolved only against the immutable descriptor generation. This value
     * contains no runtime VM object, native pointer, C++ layout, or executable callback.
     */
    struct ScriptExportTypeReference final {
        ScriptExportPrimitiveKind primitive{ScriptExportPrimitiveKind::Count}; /**< Primitive kind, or Count for a named type. */
        std::string namedType;                                                 /**< Stable named type identity when primitive is Count. */
        ScriptExportNullability nullability{ScriptExportNullability::NonNull}; /**< Whether the referenced value may be null. */

        bool operator==(const ScriptExportTypeReference &) const noexcept = default;
    };

    /** @brief One stable field in a language-neutral struct declaration. */
    struct ScriptExportFieldDescriptor final {
        std::string id;                          /**< Stable field identity scoped by its struct. */
        ScriptExportTypeReference type;          /**< Field value type and nullability. */
        ScriptExportVersion introducedVersion{}; /**< First API version containing this field. */
        ScriptExportParameterRequirement requirement{ScriptExportParameterRequirement::Required}; /**< Presence policy. */
        std::optional<std::vector<std::byte>> canonicalDefault; /**< Bounded codec-owned default for optional fields. */
        ScriptExportDocumentation documentation;                /**< Non-authoritative documentation metadata. */

        bool operator==(const ScriptExportFieldDescriptor &) const noexcept = default;
    };

    /** @brief One stable enumerator identity and canonical numeric value. */
    struct ScriptExportEnumValueDescriptor final {
        std::string id;                          /**< Stable enumerator identity scoped by its enum. */
        std::int64_t value{};                    /**< Canonical numeric value exposed to every runtime. */
        ScriptExportVersion introducedVersion{}; /**< First API version containing this enumerator. */
        ScriptExportDocumentation documentation; /**< Non-authoritative documentation metadata. */

        bool operator==(const ScriptExportEnumValueDescriptor &) const noexcept = default;
    };

    /** @brief One named enum, struct, array, or map declaration. */
    struct ScriptExportTypeDescriptor final {
        std::string id;                                                   /**< Stable named type identity. */
        ScriptExportNamedTypeKind kind{ScriptExportNamedTypeKind::Count}; /**< Closed declaration kind. */
        ScriptExportVersion introducedVersion{};                          /**< First API version containing this type. */
        ScriptExportTypeReference elementType;                            /**< Element type for an Array declaration. */
        ScriptExportTypeReference keyType;                                /**< Key type for a Map declaration. */
        ScriptExportTypeReference valueType;                              /**< Value type for a Map declaration. */
        std::size_t maximumElements{};                                    /**< Host-enforced bound for Array and Map values. */
        std::vector<ScriptExportFieldDescriptor> fields;                  /**< Struct fields, otherwise empty. */
        std::vector<ScriptExportEnumValueDescriptor> enumValues;          /**< Enum values, otherwise empty. */
        ScriptExportDocumentation documentation;                          /**< Non-authoritative documentation metadata. */

        bool operator==(const ScriptExportTypeDescriptor &) const noexcept = default;
    };

    /** @brief One stable function parameter with a bounded optional default. */
    struct ScriptExportParameterDescriptor final {
        std::string id;                          /**< Stable parameter identity scoped by the function. */
        ScriptExportTypeReference type;          /**< Parameter value type and nullability. */
        ScriptExportVersion introducedVersion{}; /**< First API version containing this parameter. */
        ScriptExportParameterRequirement requirement{ScriptExportParameterRequirement::Required}; /**< Presence policy. */
        std::optional<std::vector<std::byte>> canonicalDefault; /**< Bounded codec-owned default for optional parameters. */
        ScriptExportDocumentation documentation;                /**< Non-authoritative documentation metadata. */

        bool operator==(const ScriptExportParameterDescriptor &) const noexcept = default;
    };

    /** @brief One stable function result. Results are named to avoid runtime-specific tuple rules. */
    struct ScriptExportResultDescriptor final {
        std::string id;                          /**< Stable result identity scoped by the function. */
        ScriptExportTypeReference type;          /**< Result value type and nullability. */
        ScriptExportVersion introducedVersion{}; /**< First API version containing this result. */
        ScriptExportDocumentation documentation; /**< Non-authoritative documentation metadata. */

        bool operator==(const ScriptExportResultDescriptor &) const noexcept = default;
    };

    /** @brief One inert function declaration mapped to a host-owned backend service operation. */
    struct ScriptExportFunctionDescriptor final {
        std::string id;                                                           /**< Stable function identity. */
        ScriptExportVersion introducedVersion{};                                  /**< First API version containing this function. */
        ScriptExportInvocationMode invocation{ScriptExportInvocationMode::Count}; /**< Host scheduling policy. */
        std::vector<ScriptExportParameterDescriptor> parameters;                  /**< Stable parameter declarations. */
        std::vector<ScriptExportResultDescriptor> results;                        /**< Stable result declarations; empty means no result. */
        std::vector<std::string> errorIds;                                        /**< Stable errors this operation may return. */
        ScriptExportDocumentation documentation;                                  /**< Non-authoritative documentation metadata. */

        bool operator==(const ScriptExportFunctionDescriptor &) const noexcept = default;
    };

    /** @brief One stable semantic error that a script operation may preserve across runtimes. */
    struct ScriptExportErrorDescriptor final {
        std::string id;                          /**< Stable error identity. */
        ScriptExportVersion introducedVersion{}; /**< First API version containing this error. */
        bool retryable{};                        /**< Whether retry may be meaningful to the caller. */
        ScriptExportDocumentation documentation; /**< Non-authoritative documentation metadata. */

        bool operator==(const ScriptExportErrorDescriptor &) const noexcept = default;
    };

    /** @brief One typed constant with an owned canonical value encoding. */
    struct ScriptExportConstantDescriptor final {
        std::string id;                          /**< Stable constant identity. */
        ScriptExportTypeReference type;          /**< Constant value type and nullability. */
        ScriptExportVersion introducedVersion{}; /**< First API version containing this constant. */
        std::vector<std::byte> canonicalValue;   /**< Canonical bounded value bytes; never a native object representation. */
        ScriptExportDocumentation documentation; /**< Non-authoritative documentation metadata. */

        bool operator==(const ScriptExportConstantDescriptor &) const noexcept = default;
    };

    /** @brief Exact backend capability and service contract referenced by one script export. */
    struct ScriptExportServiceReference final {
        ExtensionCapabilityId capability;     /**< Host capability required before binding. */
        std::string serviceId;                /**< Stable backend service export identity. */
        std::string contractId;               /**< Stable callable service contract identity. */
        ScriptExportVersion minimumVersion{}; /**< Minimum compatible service contract version. */

        bool operator==(const ScriptExportServiceReference &) const noexcept = default;
    };

    /**
     * @brief Complete inert language-neutral API declaration for one extension module.
     *
     * The descriptor identifies a backend service but never embeds a provider pointer,
     * function table, VM object, runtime-specific type, or lifecycle callback. Provider
     * selection, trust, permissions, binding creation, and activation remain host-owned.
     */
    struct ScriptExportDescriptor final {
        std::string moduleId;                                  /**< Stable owning extension module identity. */
        std::string id;                                        /**< Stable script API identity. */
        std::string nameSpace;                                 /**< Stable namespace projected by every supported runtime. */
        ScriptExportVersion version{};                         /**< Current API declaration version. */
        ScriptExportVersionRange compatibility;                /**< Explicit same-major client compatibility interval. */
        ScriptExportServiceReference service;                  /**< Backend capability/service contract reference. */
        std::vector<ScriptExportTypeDescriptor> types;         /**< Named structs, enums, arrays, and maps. */
        std::vector<ScriptExportConstantDescriptor> constants; /**< Typed constants. */
        std::vector<ScriptExportErrorDescriptor> errors;       /**< Stable semantic errors. */
        std::vector<ScriptExportFunctionDescriptor> functions; /**< Callable declarations. */
        std::vector<std::string> tombstonedSymbols;            /**< Retired identities that may never be reused. */
        ScriptExportDocumentation documentation;               /**< Non-authoritative API documentation metadata. */

        bool operator==(const ScriptExportDescriptor &) const noexcept = default;
    };

    /** @brief Finite host bounds applied before a descriptor can reach a runtime adapter. */
    struct ScriptExportDescriptorLimits final {
        std::size_t maximumDescriptors{128};                    /**< Maximum APIs in one immutable generation. */
        std::size_t maximumIdentityBytes{256};                  /**< Maximum module, API, namespace, and symbol identity bytes. */
        std::size_t maximumDocumentationBytes{4U * 1024U};      /**< Maximum bytes in one documentation string. */
        std::size_t maximumFunctions{256};                      /**< Maximum functions in one API. */
        std::size_t maximumParametersPerFunction{64};           /**< Maximum parameters in one function. */
        std::size_t maximumResultsPerFunction{32};              /**< Maximum results in one function. */
        std::size_t maximumErrorsPerFunction{64};               /**< Maximum referenced errors in one function. */
        std::size_t maximumTypes{256};                          /**< Maximum named types in one API. */
        std::size_t maximumFieldsPerStruct{128};                /**< Maximum fields in one struct. */
        std::size_t maximumEnumValues{256};                     /**< Maximum values in one enum. */
        std::size_t maximumConstants{256};                      /**< Maximum constants in one API. */
        std::size_t maximumErrors{256};                         /**< Maximum semantic errors in one API. */
        std::size_t maximumTombstones{1024};                    /**< Maximum retired identities in one API. */
        std::size_t maximumSymbols{4096};                       /**< Maximum active declarations plus retired identities in one API. */
        std::size_t maximumContainerElements{1U << 20};         /**< Maximum elements in one declared array or map value. */
        std::size_t maximumDefaultBytes{64U * 1024U};           /**< Maximum canonical default encoding. */
        std::size_t maximumConstantBytes{64U * 1024U};          /**< Maximum canonical constant encoding. */
        std::size_t maximumTypeNestingDepth{32};                /**< Maximum acyclic named-type reference depth. */
        std::size_t maximumDescriptorBytes{4U * 1024U * 1024U}; /**< Maximum aggregate owned text/value bytes. */
    };

    /**
     * @brief Validates one complete inert script export without activation or provider lookup.
     * @param descriptor Candidate language-neutral API declaration.
     * @param limits Explicit resource and nesting bounds owned by the host.
     * @return Success or a typed invalid, conflict, capacity, or compatibility-independent error.
     * @post No runtime, service registry, VM, callback, native handle, or ambient state is touched.
     */
    [[nodiscard]] Result<void> ValidateScriptExportDescriptor(const ScriptExportDescriptor &descriptor,
                                                              const ScriptExportDescriptorLimits &limits = {});

    /** @brief Immutable identity-sorted generation of validated script export declarations. */
    class ScriptExportDescriptorSnapshot final {
    private:
        struct ConstructionKey final {};

        [[nodiscard]] static Result<std::shared_ptr<const ScriptExportDescriptorSnapshot>> BuildFromCandidate(
            Result<std::vector<ScriptExportDescriptor>> candidate, std::uint64_t generation);

    public:
        /** @brief Returns all declarations in ascending stable API identity order. */
        [[nodiscard]] std::span<const ScriptExportDescriptor> Descriptors() const noexcept;

        /**
         * @brief Finds one exact API identity in this generation.
         * @param id Stable API identity to find.
         * @return Borrowed declaration or nullptr when the identity is absent.
         */
        [[nodiscard]] const ScriptExportDescriptor *Find(std::string_view id) const noexcept;

        /** @brief Returns the canonical digest of logical descriptor contents. */
        [[nodiscard]] const Sha256Digest &Fingerprint() const noexcept;

        /** @brief Returns the monotonically increasing immutable descriptor generation. */
        [[nodiscard]] std::uint64_t Generation() const noexcept;

        /** @brief Constructs canonical storage using the private builder key. */
        ScriptExportDescriptorSnapshot(ConstructionKey, std::vector<ScriptExportDescriptor> descriptors, const Sha256Digest &fingerprint,
                                       std::uint64_t generation);

    private:
        friend Result<std::shared_ptr<const ScriptExportDescriptorSnapshot>> BuildScriptExportDescriptorSnapshot(
            std::span<const ScriptExportDescriptor>, const ScriptExportDescriptorLimits &);
        friend Result<std::shared_ptr<const ScriptExportDescriptorSnapshot>> BuildScriptExportDescriptorReplacement(
            const std::shared_ptr<const ScriptExportDescriptorSnapshot> &, std::span<const ScriptExportDescriptor>,
            const ScriptExportDescriptorLimits &);

        std::vector<ScriptExportDescriptor> descriptors_;
        Sha256Digest fingerprint_;
        std::uint64_t generation_{};
    };

    /** @brief Shared immutable handle used by runtime adapters and import contexts. */
    using ScriptExportDescriptorSnapshotPtr = std::shared_ptr<const ScriptExportDescriptorSnapshot>;

    /**
     * @brief Builds the initial validated script export generation.
     * @param descriptors Complete inert declarations; the input order is not significant.
     * @param limits Explicit finite construction bounds.
     * @return Immutable generation or a typed validation failure; failure publishes nothing.
     */
    [[nodiscard]] Result<ScriptExportDescriptorSnapshotPtr> BuildScriptExportDescriptorSnapshot(
        std::span<const ScriptExportDescriptor> descriptors, const ScriptExportDescriptorLimits &limits = {});

    /**
     * @brief Builds a compatible replacement while retaining the prior generation on failure.
     * @param previous Current immutable generation; it must not be null.
     * @param descriptors Complete replacement declarations.
     * @param limits Explicit finite construction bounds.
     * @return New generation or an incompatible/invalid failure; @p previous is never modified.
     */
    [[nodiscard]] Result<ScriptExportDescriptorSnapshotPtr> BuildScriptExportDescriptorReplacement(
        const ScriptExportDescriptorSnapshotPtr &previous, std::span<const ScriptExportDescriptor> descriptors,
        const ScriptExportDescriptorLimits &limits = {});

    /** @brief ADR-055 vocabulary alias for the language-neutral script API declaration. */
    using ExtensionScriptApiDescriptorV1 = ScriptExportDescriptor;
    /** @brief ADR-055 vocabulary alias for script API descriptor bounds. */
    using ExtensionScriptApiDescriptorLimitsV1 = ScriptExportDescriptorLimits;
}  // namespace Horo::Extensions
