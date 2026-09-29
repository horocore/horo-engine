#pragma once

/**
 * @file EnvironmentQuerySchema.h
 * @brief Versioned, bounded EQS authoring schema and inert descriptor admission.
 */

#include "Horo/AI/AIIdentity.h"
#include "Horo/Foundation/Result.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Horo::AI {
    struct QueryIdentityTag;
    struct QueryStageIdentityTag;
    struct QueryItemTypeIdentityTag;
    struct QueryContextIdentityTag;
    struct QueryGeneratorIdentityTag;
    struct QueryTestIdentityTag;
    struct QueryPropertyIdentityTag;
    struct QueryProviderIdentityTag;
    struct QueryResultSchemaIdentityTag;

    /** @brief Stable authored EQS asset identity; AssetRegistry binds it to an AssetId outside HoroAI. */
    using QueryId = AiStableIdentity<QueryIdentityTag>;
    /** @brief Stable authored stage identity, independent of label or array position. */
    using QueryStageId = AiStableIdentity<QueryStageIdentityTag>;
    /** @brief Stable item payload type identity. */
    using QueryItemTypeId = AiStableIdentity<QueryItemTypeIdentityTag>;
    /** @brief Stable context type identity. */
    using QueryContextId = AiStableIdentity<QueryContextIdentityTag>;
    /** @brief Stable generator descriptor identity. */
    using QueryGeneratorId = AiStableIdentity<QueryGeneratorIdentityTag>;
    /** @brief Stable test descriptor identity. */
    using QueryTestId = AiStableIdentity<QueryTestIdentityTag>;
    /** @brief Stable property identity within one stage descriptor. */
    using QueryPropertyId = AiStableIdentity<QueryPropertyIdentityTag>;
    /** @brief Stable identity of a native, script, or package descriptor provider. */
    using QueryProviderId = AiStableIdentity<QueryProviderIdentityTag>;
    /** @brief Stable identity of an authored query result schema. */
    using QueryResultSchemaId = AiStableIdentity<QueryResultSchemaIdentityTag>;

    /** @brief Current authored EQS schema version; cooked artifact versions are separate. */
    inline constexpr std::uint32_t CurrentEnvironmentQuerySchemaVersion = 1;

    /** @brief Inclusive non-zero descriptor or payload version interval. */
    struct QueryVersionRange final {
        std::uint32_t minimum{1};
        std::uint32_t maximum{std::numeric_limits<std::uint32_t>::max()};

        /** @brief Checks non-zero ordered bounds. @return True for a usable interval. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return minimum != 0 && minimum <= maximum;
        }

        /** @brief Tests one schema version. @param version Candidate version. @return True when admitted. */
        [[nodiscard]] constexpr bool Contains(std::uint32_t version) const noexcept {
            return IsValid() && version >= minimum && version <= maximum;
        }

        constexpr auto operator<=>(const QueryVersionRange &) const noexcept = default;
    };

    /** @brief Typed item representation; custom bytes use a canonical payload schema, not native object layout. */
    enum class QueryItemKind : std::uint8_t {
        Point,
        Actor,
        DirectionalRay,
        Custom,
        Count
    };
    /** @brief Authored stage family; Unknown retains a future contribution but is never executable. */
    enum class QueryStageKind : std::uint8_t {
        Generator,
        Test,
        Unknown,
        Count
    };
    /** @brief Canonical authored property type. */
    enum class QueryPropertyKind : std::uint8_t {
        Boolean,
        Signed64,
        Unsigned64,
        Float64,
        CanonicalBytes,
        Count
    };
    /** @brief Origin family of an inert descriptor contribution. */
    enum class QueryDescriptorSourceKind : std::uint8_t {
        Native,
        Script,
        Package,
        Count
    };

    /** @brief Descriptor owner and exact non-zero schema version, never registration precedence. */
    struct QueryDescriptorOrigin final {
        QueryDescriptorSourceKind kind{QueryDescriptorSourceKind::Native};
        QueryProviderId provider;
        std::uint32_t version{1};

        constexpr auto operator<=>(const QueryDescriptorOrigin &) const noexcept = default;
    };

    /** @brief Immutable canonical property value; byte payloads are validated and bounded before capture. */
    using QueryPropertyData = std::variant<bool, std::int64_t, std::uint64_t, double, std::vector<std::byte>>;

    /** @brief One authored property keyed only by its stable identity. */
    struct QueryPropertyValue final {
        QueryPropertyId id;
        QueryPropertyData value;

        bool operator==(const QueryPropertyValue &) const = default;
    };

    /** @brief Descriptor-side property contract, independent of display names and serialized order. */
    struct QueryPropertyDescriptor final {
        QueryPropertyId id;
        QueryPropertyKind kind{QueryPropertyKind::Boolean};
        bool required{};
        std::size_t maximumBytes{64}; /**< Used only by CanonicalBytes, at most 64. */

        constexpr auto operator<=>(const QueryPropertyDescriptor &) const noexcept = default;
    };

    /** @brief Versioned item type with bounded canonical custom payload. */
    struct QueryItemTypeDescriptor final {
        QueryItemTypeId id;
        QueryDescriptorOrigin origin;
        QueryItemKind kind{QueryItemKind::Point};
        std::size_t maximumPayloadBytes{}; /**< Zero for built-ins; 1–64 for Custom. */
    };

    /** @brief Versioned context type with bounded canonical captured payload. */
    struct QueryContextDescriptor final {
        QueryContextId id;
        QueryDescriptorOrigin origin;
        std::size_t maximumPayloadBytes{64};
    };

    /** @brief Exact context dependency and compatible schema versions for one stage descriptor. */
    struct QueryContextRequirement final {
        QueryContextId id;
        QueryVersionRange version;

        constexpr auto operator<=>(const QueryContextRequirement &) const noexcept = default;
    };

    /** @brief Inert generator metadata; execution callbacks belong to a later host-composed provider seam. */
    struct QueryGeneratorDescriptor final {
        QueryGeneratorId id;
        QueryDescriptorOrigin origin;
        QueryItemTypeId outputItemType;
        QueryVersionRange outputItemVersion;
        std::vector<QueryContextRequirement> contexts;
        std::vector<QueryPropertyDescriptor> properties;
    };

    /** @brief Inert test metadata over one typed candidate item. */
    struct QueryTestDescriptor final {
        QueryTestId id;
        QueryDescriptorOrigin origin;
        QueryItemTypeId inputItemType;
        QueryVersionRange inputItemVersion;
        std::vector<QueryContextRequirement> contexts;
        std::vector<QueryPropertyDescriptor> properties;
    };

    /** @brief Hard ceilings for untrusted EQS metadata, independent of product profile settings. */
    struct EnvironmentQuerySchemaLimits final {
        static constexpr std::size_t ItemTypes = 64;
        static constexpr std::size_t Contexts = 64;
        static constexpr std::size_t Generators = 128;
        static constexpr std::size_t Tests = 128;
        static constexpr std::size_t Stages = 64;
        static constexpr std::size_t PropertiesPerStage = 32;
        static constexpr std::size_t DescriptorProperties = 32;
        static constexpr std::size_t DescriptorContexts = 16;
        static constexpr std::size_t CanonicalBytes = 64;
        static constexpr std::size_t UnknownStageBytes = 4'096;
        static constexpr std::size_t DisplayNameBytes = 128;
        static constexpr std::uint32_t MaximumResults = 4'096;
    };

    /** @brief Borrowed descriptor contributions synchronously captured as inert owned metadata. */
    struct QueryDescriptorContributions final {
        std::span<const QueryItemTypeDescriptor> items;
        std::span<const QueryContextDescriptor> contexts;
        std::span<const QueryGeneratorDescriptor> generators;
        std::span<const QueryTestDescriptor> tests;
    };

    /** @brief Immutable validated descriptor snapshot; never starts or discovers a provider. */
    class QuerySchemaRegistry final {
    public:
        /**
         * @brief Copies and validates bounded native, script, or package descriptors.
         * @param contributions Borrowed contributions from explicit host composition.
         * @return Owned registry or a typed invalid, conflict, missing, version, or limit failure.
         */
        [[nodiscard]] static Result<QuerySchemaRegistry> Capture(const QueryDescriptorContributions &contributions);

        /** @brief Resolves one item type by stable identity. @param id Exact ID. @return Descriptor or null. */
        [[nodiscard]] const QueryItemTypeDescriptor *Find(QueryItemTypeId id) const noexcept;
        /** @brief Resolves one context type by stable identity. @param id Exact ID. @return Descriptor or null. */
        [[nodiscard]] const QueryContextDescriptor *Find(QueryContextId id) const noexcept;
        /** @brief Resolves one generator by stable identity. @param id Exact ID. @return Descriptor or null. */
        [[nodiscard]] const QueryGeneratorDescriptor *Find(QueryGeneratorId id) const noexcept;
        /** @brief Resolves one test by stable identity. @param id Exact ID. @return Descriptor or null. */
        [[nodiscard]] const QueryTestDescriptor *Find(QueryTestId id) const noexcept;

    private:
        std::vector<QueryItemTypeDescriptor> items_;
        std::vector<QueryContextDescriptor> contexts_;
        std::vector<QueryGeneratorDescriptor> generators_;
        std::vector<QueryTestDescriptor> tests_;
    };

    /** @brief Versioned result item contract; ranking and candidate values are later runtime concerns. */
    struct QueryResultSchema final {
        QueryResultSchemaId id;
        std::uint32_t version{1};
        QueryItemTypeId itemType;
        QueryVersionRange itemVersion;
        std::uint32_t maximumResults{EnvironmentQuerySchemaLimits::MaximumResults};

        constexpr auto operator<=>(const QueryResultSchema &) const noexcept = default;
    };

    /** @brief Authored stage with explicit semantic order and opaque bytes retained for unavailable contributions. */
    struct QueryAssetStage final {
        QueryStageId id;
        QueryStageKind kind{QueryStageKind::Unknown};
        std::uint32_t executionOrder{}; /**< Semantic execution order; vector/editor order is not meaningful. */
        std::string displayName;        /**< Presentation only; never persistent identity or plan input. */
        QueryGeneratorId generator;
        QueryTestId test;
        QueryVersionRange descriptorVersion;
        std::vector<QueryPropertyValue> properties;
        std::uint64_t unknownTypeId{};        /**< Future stage type ID retained only when kind is Unknown. */
        std::vector<std::byte> opaquePayload; /**< Canonical unknown bytes retained but never passed to an executor. */
    };

    /** @brief Borrowed authoring source; AssetRegistry mapping and cooker identity live downstream. */
    struct QueryAssetSource final {
        QueryId id;
        std::uint32_t schemaVersion{CurrentEnvironmentQuerySchemaVersion};
        std::string displayName;
        QueryResultSchema result;
        std::vector<QueryAssetStage> stages;
    };

    /** @brief Owned bounded authoring asset, including any unknown stage data for editor round-trip. */
    class EnvironmentQueryAsset final {
    public:
        /**
         * @brief Validates structural bounds and captures source bytes without resolving contributed descriptors.
         * @param source Borrowed authoring source.
         * @return Owned asset or a typed invalid, version, duplicate, or limit failure.
         * @post Unknown stage kinds and missing descriptors remain preserved but cannot compile.
         */
        [[nodiscard]] static Result<EnvironmentQueryAsset> Capture(const QueryAssetSource &source);

        /** @brief Returns stable query identity. @return Authored identity. */
        [[nodiscard]] QueryId Id() const noexcept;
        /** @brief Returns the owned result contract. @return Read-only result schema. */
        [[nodiscard]] const QueryResultSchema &ResultSchema() const noexcept;
        /** @brief Returns owned authoring stages, including unknown stages. @return Read-only stages in source order. */
        [[nodiscard]] std::span<const QueryAssetStage> Stages() const noexcept;
        /** @brief Returns the authored display label. @return Presentation-only text. */
        [[nodiscard]] const std::string &DisplayName() const noexcept;

    private:
        explicit EnvironmentQueryAsset(QueryAssetSource source) noexcept;
        QueryAssetSource source_;
    };

    /** @brief One descriptor-admitted stage without editor labels or unknown opaque payloads. */
    struct QueryPlanStage final {
        QueryStageId id;
        QueryStageKind kind{QueryStageKind::Unknown};
        std::uint32_t executionOrder{};
        QueryGeneratorId generator;
        QueryTestId test;
        std::vector<QueryContextRequirement> contexts;
        std::vector<QueryPropertyValue> properties;

        bool operator==(const QueryPlanStage &) const = default;
    };

    /** @brief Immutable typed plan admitted only when every stage and property is known and compatible. */
    class EnvironmentQueryPlan final {
    public:
        /**
         * @brief Resolves every authored stage against an inert descriptor snapshot before any execution.
         * @param asset Structurally captured authoring asset.
         * @param registry Explicit descriptor snapshot.
         * @return Admitted plan or typed unavailable, unsupported, or incompatible failure.
         * @post Failure never drops an unknown stage or produces a partial executable plan.
         */
        [[nodiscard]] static Result<EnvironmentQueryPlan> Compile(const EnvironmentQueryAsset &asset, const QuerySchemaRegistry &registry);

        /** @brief Returns stable asset identity. @return Authored query ID. */
        [[nodiscard]] QueryId Id() const noexcept;
        /** @brief Returns the admitted result contract. @return Immutable schema. */
        [[nodiscard]] const QueryResultSchema &ResultSchema() const noexcept;
        /** @brief Returns stages in explicit semantic execution order. @return Read-only admitted stages. */
        [[nodiscard]] std::span<const QueryPlanStage> Stages() const noexcept;
        /** @brief Returns the deduplicated context dependencies of every admitted stage. @return Stable ID-ordered requirements. */
        [[nodiscard]] std::span<const QueryContextRequirement> RequiredContexts() const noexcept;

    private:
        EnvironmentQueryPlan(QueryId id, const QueryResultSchema &result, std::vector<QueryPlanStage> stages);
        QueryId id_;
        QueryResultSchema result_;
        std::vector<QueryPlanStage> stages_;
        std::vector<QueryContextRequirement> requiredContexts_;
    };
}  // namespace Horo::AI
