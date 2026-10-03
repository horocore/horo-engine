#pragma once

/** @file NavigationLinkValidation.h
 * @brief Bounded authored traversal validation, optional suggestions and fenced cooked-link conversion.
 */

#include "Horo/Navigation/NavMeshData.h"
#include "Horo/Navigation/NavigationBackend.h"
#include "Horo/Navigation/NavigationBakeInput.h"
#include "Horo/Navigation/NavigationLinkTypes.h"

#include <optional>
#include <span>
#include <vector>

namespace Horo::Navigation {
    struct NavigationLinkAnchorIdentityTag;
    /** @brief Stable bake-local identity of one generation anchor, never an authored link identity. */
    using NavigationLinkAnchorId = NavigationIdentity<NavigationLinkAnchorIdentityTag>;

    /** @brief Ordered canonical-metre endpoint pinned to an exact authored surface. */
    struct NavigationBakeLinkEndpoint final {
        SurfaceId surface;
        Math::Vec3 position{};
        float connectionRadiusMeters{0.5F};

        [[nodiscard]] constexpr bool operator==(const NavigationBakeLinkEndpoint &) const noexcept = default;
    };

    /** @brief One enabled authored link projected by the Scene adapter into a single profile partition. */
    struct NavigationBakeLinkInput final {
        NavigationLinkId id;
        NavigationAgentProfileId profile;
        NavigationBakeLinkEndpoint start;
        NavigationBakeLinkEndpoint end;
        NavigationLinkKind kind{NavigationLinkKind::Jump};
        NavigationLinkDirection direction{NavigationLinkDirection::StartToEnd};
        float traversalCost{1.0F}; /**< Must be representable by the selected traversal area/filter policy. */
    };

    /** @brief Available host-composed traversal descriptor for one exact grounded profile and kind. */
    struct NavigationTraversalDescriptor final {
        NavigationLinkKind kind{NavigationLinkKind::Jump};
        NavigationAgentProfileId profile;
        NavigationAreaId area;
        bool supportsBidirectional{};
        float maximumDistanceMeters{5.0F};
        float maximumRiseMeters{1.0F};
        float maximumDropMeters{1.0F};
    };

    /**
     * @brief Collision-owner evidence for the free corridor between one ordered projected endpoint pair.
     * @details Evidence must match the exact bake fingerprint and profile. Radius/height are measured free clearance,
     * not requested agent dimensions. Missing evidence fails closed; navigation never assumes a gap is unobstructed.
     */
    struct NavigationLinkClearanceEvidence final {
        Sha256Digest bakeFingerprint{};
        NavigationAgentProfileId profile;
        NavigationBakeLinkEndpoint start;
        NavigationBakeLinkEndpoint end;
        float radiusMeters{};
        float heightMeters{};
    };

    /** @brief Stable endpoint offered for optional deterministic pair generation. */
    struct NavigationLinkGenerationAnchor final {
        NavigationLinkAnchorId id;
        NavigationBakeLinkEndpoint endpoint;
    };

    /** @brief Exact polygon-range-to-authored-surface binding for automatic grounded boundary-anchor extraction. */
    struct NavigationLinkGenerationSurfaceBinding final {
        NavMeshTileKey tile;
        NavMeshTableRange polygons; /**< Global polygon range inside the exact tile; ranges must not overlap. */
        SurfaceId surface;
    };

    /**
     * @brief Extracts boundary-edge midpoint anchors from validated neutral polygons in stable global polygon/edge order.
     * @param mesh Owned immutable candidate topology; it need not contain links or be published.
     * @param bindings Exact authored surface for each selected polygon range; overlapping/missing bindings fail.
     * @param maximumAnchors Positive caller ceiling no greater than the qualified 256-anchor maximum.
     * @param maximumWorkUnits Positive caller work ceiling no greater than the qualified work maximum.
     * @param cancellation Cooperative cancellation observed for each polygon edge.
     * @return Owned canonical-metre anchors with bake-local polygon/edge identities, or typed invalid/capacity/cancelled error.
     * @note Anchor radii use the mesh's validated grounded profile. Internal polygon edges are omitted. No suggestions are accepted here.
     */
    [[nodiscard]] Result<std::vector<NavigationLinkGenerationAnchor>> CaptureNavigationLinkBoundaryAnchors(
        const NavMeshData &mesh, std::span<const NavigationLinkGenerationSurfaceBinding> bindings, std::uint32_t maximumAnchors,
        std::uint64_t maximumWorkUnits, const CancellationToken &cancellation);

    /** @brief Explicit optional generation policy; ordered pairs preserve one-way direction semantics. */
    struct NavigationLinkGenerationPolicy final {
        NavigationLinkKind kind{NavigationLinkKind::Jump};
        NavigationLinkDirection direction{NavigationLinkDirection::StartToEnd};
        float maximumDistanceMeters{5.0F};
        std::span<const NavigationLinkGenerationAnchor> anchors;
    };

    /** @brief Closed validation failures reported with both ordered endpoints. */
    enum class NavigationLinkValidationRule : std::uint8_t {
        Malformed,
        DuplicateIdentity,
        ProfileMismatch,
        DescriptorUnavailable,
        TraversalCost,
        Direction,
        StartProjection,
        EndProjection,
        Clearance,
        CoincidentEndpoints,
        DuplicateTraversal,
    };

    /** @brief One owned rejection row; provider errors are retained when projection fails. */
    struct NavigationLinkDiagnostic final {
        NavigationLinkId authoredLink;
        NavigationLinkAnchorId startAnchor;
        NavigationLinkAnchorId endAnchor;
        NavigationBakeLinkEndpoint start;
        NavigationBakeLinkEndpoint end;
        NavigationLinkValidationRule rule{NavigationLinkValidationRule::Malformed};
        std::optional<Error> cause;
    };

    /** @brief Immutable inspection row for an accepted link, deliberately distinct from cooked link data. */
    struct NavigationValidatedLink final {
        NavigationLinkId authoredLink;
        NavigationLinkAnchorId startAnchor;
        NavigationLinkAnchorId endAnchor;
        NavigationSurfaceHit start;
        NavigationSurfaceHit end;
        NavigationLinkKind kind{NavigationLinkKind::Jump};
        NavigationLinkDirection direction{NavigationLinkDirection::StartToEnd};
        NavigationAreaId area;
        float radiusMeters{};
    };

    /** @brief Cook rows plus their complete semantic key, including the explicit suggestion policy. */
    struct NavigationCookedLinkSet final {
        std::vector<NavMeshOffMeshLink> links;
        Sha256Digest fingerprint{};
    };

    /** @brief Hard ceilings across admission, projection, pair generation, duplicate checks and retained results. */
    struct NavigationLinkValidationLimits final {
        static constexpr std::uint32_t MaximumAuthoredLinks = 4'096;
        static constexpr std::uint32_t MaximumAnchors = 256;
        static constexpr std::uint32_t MaximumPairAttempts = 16'384;
        static constexpr std::uint32_t MaximumSuggestions = 1'024;
        static constexpr std::uint32_t MaximumClearanceRows = 20'480;
        static constexpr std::uint64_t MaximumWorkUnits = 64ULL * 1024ULL * 1024ULL;
        static constexpr std::uint64_t MaximumOwnedBytes = 32ULL * 1024ULL * 1024ULL;

        std::uint32_t maximumAuthoredLinks{MaximumAuthoredLinks};
        std::uint32_t maximumAnchors{MaximumAnchors};
        std::uint32_t maximumPairAttempts{MaximumPairAttempts};
        std::uint32_t maximumSuggestions{MaximumSuggestions};
        std::uint32_t maximumClearanceRows{MaximumClearanceRows};
        std::uint64_t maximumWorkUnits{MaximumWorkUnits};
        std::uint64_t maximumOwnedBytes{MaximumOwnedBytes};
    };

    /** @brief Exact ephemeral query binding to the profile-specific topology used for endpoint validation. */
    struct NavigationLinkProjectionContext final {
        NavigationAgentProfileId profile;
        NavigationWorldId world;
        NavigationGeneration topology;
        NavigationQueryRequirement requirement{.query = NavigationQueryKind::NearestPoint};
    };

    /**
     * @brief Complete immutable synchronous validation request; all borrows are pinned by the host through Validate.
     * @details The backend serves the exact profile-specific built topology. The registry belongs to the captured area
     * revision; authored endpoints are already canonical metres, and clearance observations match projected endpoints.
     */
    struct NavigationLinkValidationRequest final {
        const NavigationBakeInputSnapshot &input; /**< Canonical input and exact source/revision fence. */
        const NavigationAreaRegistry &areas;      /**< Registry pinned to input.Revisions().areaRegistry. */
        const INavigationQueryBackend &backend;   /**< Immutable provider for the selected profile's exact built topology. */
        NavigationLinkProjectionContext context;
        std::span<const NavigationBakeLinkInput> authored;          /**< Enabled links in canonical metres; capture sorts by identity. */
        std::span<const NavigationTraversalDescriptor> descriptors; /**< One available descriptor per kind and selected profile. */
        std::span<const NavigationLinkClearanceEvidence> clearance; /**< Measurements for exact projected pairs and bake fingerprint. */
        std::optional<NavigationLinkGenerationPolicy> generation;   /**< Omission performs no automatic generation. */
        NavigationLinkValidationLimits limits;                      /**< Positive qualified count, work and owned-byte ceilings. */
    };

    /** @brief Explicit authorization for generated links at cooked conversion; no implicit/default acceptance exists. */
    enum class NavigationGeneratedLinkCookPolicy : std::uint8_t {
        AuthoredOnly,
        IncludeValidatedSuggestions,
        Count,
    };

    /**
     * @brief Owned move-only validation result with separate authored acceptance and generated suggestions.
     * @details No Scene, backend, registry, input, or evidence pointers survive capture. This synchronous bake kernel
     * owns no jobs or publication. The host pins the profile-specific provider for the duration of Validate only.
     */
    class NavigationLinkValidationSnapshot final {
    public:
        NavigationLinkValidationSnapshot(const NavigationLinkValidationSnapshot &) = delete;
        NavigationLinkValidationSnapshot &operator=(const NavigationLinkValidationSnapshot &) = delete;
        /** @brief Transfers result ownership and invalidates cooked conversion on the source. @param other Owned result to move. */
        NavigationLinkValidationSnapshot(NavigationLinkValidationSnapshot &&other) noexcept;
        NavigationLinkValidationSnapshot &operator=(NavigationLinkValidationSnapshot &&) = delete;

        /**
         * @brief Validates authored links and optionally generates candidates in stable anchor identity order.
         * @param request Complete host-pinned canonical capture, projection, traversal, clearance and generation policy.
         * @param cancellation Cooperative cancellation, checked around each bounded provider call and work unit.
         * @return Owned acceptance/suggestion/diagnostic snapshot, or typed invalid, capacity, stale or cancelled error.
         */
        [[nodiscard]] static Result<NavigationLinkValidationSnapshot> Validate(const NavigationLinkValidationRequest &request,
                                                                               const CancellationToken &cancellation);

        /** @brief Returns accepted authored rows in link identity order. @return Owned immutable inspection view. */
        [[nodiscard]] std::span<const NavigationValidatedLink> Authored() const noexcept;
        /** @brief Returns proposals in stable ordered anchor-pair order. @return Owned immutable suggestion view. */
        [[nodiscard]] std::span<const NavigationValidatedLink> Suggestions() const noexcept;
        /** @brief Returns rejected rows with both endpoints and failing rule. @return Owned immutable diagnostics. */
        [[nodiscard]] std::span<const NavigationLinkDiagnostic> Diagnostics() const noexcept;

        /**
         * @brief Converts validated links to existing neutral cooked rows only under an explicit suggestion policy.
         * @param policy Declared authored-only or suggestion-inclusive cook decision.
         * @param input Exact bake input whose fingerprint and revisions must match this result.
         * @param currentRevisions Current authoritative bake revisions, including latest request generation.
         * @param currentSources Complete current source observations for the existing publication fence.
         * @param currentContext Current profile/world/topology identity; replacement rejects old polygon evidence.
         * @param state Current publication lifecycle; cancelled, failed, superseded and shutdown states fail closed.
         * @param cancellation Cooperative cancellation checked before and after conversion.
         * @return Complete cooked rows, or typed invalid, stale, cancellation or authored-rejection failure; never partial rows.
         */
        [[nodiscard]] Result<NavigationCookedLinkSet> PrepareCookedLinks(NavigationGeneratedLinkCookPolicy policy,
                                                                         const NavigationBakeInputSnapshot &input,
                                                                         const NavigationBakeInputRevisions &currentRevisions,
                                                                         std::span<const NavigationSourceObservation> currentSources,
                                                                         const NavigationLinkProjectionContext &currentContext,
                                                                         NavigationBakePublicationState state,
                                                                         const CancellationToken &cancellation) const;

    private:
        NavigationLinkValidationSnapshot(const NavigationBakeInputSnapshot &input, const NavigationLinkProjectionContext &context);
        bool valid_{true};
        NavigationBakeInputRevisions revisions_;
        Sha256Digest fingerprint_{};
        NavigationLinkProjectionContext context_;
        std::vector<NavigationValidatedLink> authored_;
        std::vector<NavigationValidatedLink> suggestions_;
        std::vector<NavigationLinkDiagnostic> diagnostics_;
    };
}  // namespace Horo::Navigation
