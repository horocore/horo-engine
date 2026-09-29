#pragma once

/**
 * @file EnvironmentQueryContexts.h
 * @brief Scene-validated, immutable environment-query context capture at submission.
 */

#include "Horo/AI/AISceneComponents.h"
#include "Horo/AI/EnvironmentQuerySchema.h"
#include "Horo/Math/WorldCoordinate64.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace Horo::AI {
    /** @brief Reserved built-in context identities, independent of authored display names. */
    enum class BuiltinQueryContext : std::uint8_t {
        Querier,
        Target,
        QuerierLocation,
        TargetLocation,
        WorldOrigin,
        Group,
        Count,
    };

    /** @brief Returns the reserved non-zero identity of a built-in context. @param kind Built-in kind. @return ID or invalid ID. */
    [[nodiscard]] QueryContextId BuiltinQueryContextId(BuiltinQueryContext kind);

    /** @brief Supplies inert built-in descriptors for explicit schema composition. @return Six stable version-one descriptors. */
    [[nodiscard]] std::array<QueryContextDescriptor, 6> BuiltinQueryContextDescriptors();

    /** @brief Bounded canonical bytes, never native object memory or a borrowed view. */
    struct QueryCanonicalContext final {
        std::uint32_t schemaVersion{1};
        std::array<std::byte, EnvironmentQuerySchemaLimits::CanonicalBytes> bytes{};
        std::size_t size{};
    };

    /** @brief Owned entity, global location, ordered group, or canonical provider value. */
    using QueryContextValue =
        std::variant<Runtime::EntityRef, Math::WorldCoordinate64, std::vector<Runtime::EntityRef>, QueryCanonicalContext>;

    /** @brief One owned typed context value with its admitted descriptor identity. */
    struct QueryCapturedContext final {
        QueryContextId id;
        QueryContextValue value;
    };

    /** @brief Immutable owned values fixed for one query execution revision. */
    class QueryContextSnapshot final {
    public:
        /** @brief Returns the source scene incarnation. @return Exact generation-checked runtime ID. */
        [[nodiscard]] Runtime::SceneRuntimeId Scene() const noexcept;
        /** @brief Returns the submission execution revision. @return Non-zero revision. */
        [[nodiscard]] std::uint64_t Revision() const noexcept;
        /** @brief Returns the complete captured value set. @return Borrowed read-only records. */
        [[nodiscard]] std::span<const QueryCapturedContext> Values() const noexcept;
        /** @brief Finds one captured value. @param id Stable context ID. @return Read-only record or null. */
        [[nodiscard]] const QueryCapturedContext *Find(QueryContextId id) const noexcept;

    private:
        friend class QueryContextCapture;
        Runtime::SceneRuntimeId scene_;
        std::uint64_t revision_{};
        std::vector<QueryCapturedContext> values_;
    };

    /** @brief Borrowed custom provider callback; it must not retain the scene view. */
    using QueryContextCaptureFunction = Result<QueryContextValue> (*)(void *state, const Runtime::RuntimeSceneView &scene);

    /** @brief Explicit host-composed capture provider; state remains caller-owned for the registry lifetime. */
    struct QueryContextProviderRegistration final {
        QueryContextId id;
        std::uint32_t schemaVersion{1};
        AiCapabilitySet requiredCapabilities;
        void *state{};
        QueryContextCaptureFunction capture{};
    };

    /** @brief Owned bounded registration table; inert until Capture invokes one required provider. */
    class QueryContextProviderRegistry final {
    public:
        /** @brief Validates and copies explicit registrations. @param providers Borrowed registrations. @param schema Admitted inert
         * descriptors. @return Registry or typed descriptor failure. */
        [[nodiscard]] static Result<QueryContextProviderRegistry> Capture(std::span<const QueryContextProviderRegistration> providers,
                                                                          const QuerySchemaRegistry &schema);
        /** @brief Finds an explicitly registered provider. @param id Exact context ID. @return Borrowed registration or null. */
        [[nodiscard]] const QueryContextProviderRegistration *Find(QueryContextId id) const noexcept;

    private:
        std::vector<QueryContextProviderRegistration> providers_;
    };

    /** @brief Borrowed submission values; global locations are host-captured, never rebased float origins. */
    struct QueryContextCaptureSource final {
        Runtime::EntityRef querier;
        Math::WorldCoordinate64 querierLocation;
        std::optional<Runtime::EntityRef> target;
        std::optional<Math::WorldCoordinate64> targetLocation;
        std::optional<std::span<const Runtime::EntityRef>> group;
        AiCapabilitySet availableCapabilities;
        std::uint64_t executionRevision{};
    };

    /**
     * @brief Owner-thread capture seam for one admitted query and scene incarnation.
     * @details One instance is bound to one plan. Repeated reads of a revision return the same immutable allocation;
     *          advancing the revision captures anew. Old snapshots stay valid while consumers retain them.
     */
    class QueryContextCapture final {
    public:
        /**
         * @brief Resolves every plan dependency against a current scene view without partial publication.
         * @param plan Immutable plan whose context requirements were admitted by the schema.
         * @param schema Inert descriptor registry used to compile the plan.
         * @param providers Explicit host-composed custom provider table.
         * @param scene Current owner-thread view of the scene.
         * @param source Borrowed submission values and non-zero execution revision.
         * @return Shared immutable snapshot or typed missing, stale, capability, or invalid failure.
         */
        [[nodiscard]] Result<std::shared_ptr<const QueryContextSnapshot>> Capture(const EnvironmentQueryPlan &plan,
                                                                                  const QuerySchemaRegistry &schema,
                                                                                  const QueryContextProviderRegistry &providers,
                                                                                  const Runtime::RuntimeSceneView &scene,
                                                                                  const QueryContextCaptureSource &source);

    private:
        QueryId plan_;
        std::uint64_t latestRevision_{};
        std::shared_ptr<const QueryContextSnapshot> latest_;
    };
}  // namespace Horo::AI
