#pragma once

/** @file AnimationGraph.h
 * @brief Versioned animation graph authoring and bounded immutable compilation.
 */
#include "Horo/Animation/AnimationClip.h"
#include "Horo/Animation/AnimationComponents.h"
#include "Horo/Foundation/CancellationToken.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Horo::Animation {
    /** @brief Current authoring schema; earlier versions require explicit
     * migration. */
    inline constexpr std::uint32_t CurrentAnimationGraphSchemaVersion = 6;

    /** @brief Exact graph semantic contract, independent of source-envelope
     * migration. */
    struct AnimationGraphContractVersion final {
        std::uint16_t major{};
        std::uint16_t minor{};
        std::uint16_t patch{};
        auto operator<=>(const AnimationGraphContractVersion &) const = default;
    };

    /** @brief Graph semantic contract implemented by this compiler. */
    inline constexpr AnimationGraphContractVersion CurrentAnimationGraphContractVersion{1, 4, 0};

    struct GraphDefinitionTag;
    struct GraphNodeTag;
    struct GraphPinTag;
    struct GraphParameterTag;
    struct GraphInterfaceTag;
    struct GraphStateTag;
    struct GraphTransitionTag;
    /** @brief Stable graph-local definition identity. */
    using GraphDefinitionId = Foundation::Detail::NonZeroId64<GraphDefinitionTag, AnimationErrors::IdentityInvalid>;
    /** @brief Stable definition-local node identity. */
    using GraphNodeId = Foundation::Detail::NonZeroId64<GraphNodeTag, AnimationErrors::IdentityInvalid>;
    /** @brief Stable node-local pin identity. */
    using GraphPinId = Foundation::Detail::NonZeroId64<GraphPinTag, AnimationErrors::IdentityInvalid>;
    /** @brief Stable asset-local parameter identity. */
    using GraphParameterId = Foundation::Detail::NonZeroId64<GraphParameterTag, AnimationErrors::IdentityInvalid>;
    /** @brief Stable definition-local input identity shared by calls and input
     * nodes. */
    using GraphInterfaceId = Foundation::Detail::NonZeroId64<GraphInterfaceTag, AnimationErrors::IdentityInvalid>;
    /** @brief Stable state identity local to one state-machine node. */
    using GraphStateId = Foundation::Detail::NonZeroId64<GraphStateTag, AnimationErrors::IdentityInvalid>;
    /** @brief Stable transition identity local to one state-machine node. */
    using GraphTransitionId = Foundation::Detail::NonZeroId64<GraphTransitionTag, AnimationErrors::IdentityInvalid>;

    /** @brief Pin transport vocabulary; Unspecified is admitted only by version-1
     * migration. */
    enum class GraphValueType : std::uint8_t {
        Pose,
        Float,
        Boolean,
        Integer,
        Trigger,
        Unspecified
    };
    /** @brief Static parameter defaults; trigger defaults must be false. */
    using GraphParameterValue = std::variant<float, bool, std::int32_t>;

    /** @brief Immutable parameter declaration, never a gameplay parameter store. */
    struct GraphParameter final {
        GraphParameterId id{};
        std::string name = std::string(std::size_t{0}, char{}); /**< Bounded ASCII identifier for display/binding;
                               identity remains id. */
        GraphValueType type{GraphValueType::Float};
        GraphParameterValue defaultValue{0.0F};
        bool operator==(const GraphParameter &) const = default;
    };

    /** @brief Stable typed input to one reusable subgraph definition. */
    struct GraphInterfaceInput final {
        GraphInterfaceId id{};
        GraphValueType type{GraphValueType::Pose};
        bool operator==(const GraphInterfaceInput &) const = default;
    };
    /** @brief Semantic pin roles; blend inputs use FirstPose, SecondPose and
     * Weight. */
    enum class GraphPinRole : std::uint8_t {
        Value,
        FirstPose,
        SecondPose,
        Weight,
        Interface,
        Result,
        ReferencePose,
        Count
    };

    /** @brief A stable input or output pin; only Interface roles carry interfaceId.
     */
    struct GraphPin final {
        GraphPinId id{};
        GraphPinRole role{GraphPinRole::Result};
        GraphValueType type{GraphValueType::Pose};
        bool output{true};
        GraphInterfaceId interfaceId{};
        bool operator==(const GraphPin &) const = default;
    };

    /** @brief Clip sampling reference; the instance owner resolves its skeleton and
     * generation. */
    struct GraphClipNode final {
        AnimationClipId clip{};
        std::optional<AnimationReferencePoseId> referencePose{}; /**< Additive reference identity supplied by the
                                                                    ReferencePose operand. */
        bool operator==(const GraphClipNode &) const = default;
    };

    /** @brief Normalized interpolation or reference-relative rotation delta
     * application space. */
    enum class GraphBlendMode : std::uint8_t {
        Normalized,
        AdditiveLocal,
        AdditiveParent,
        Count
    };
    /** @brief Completion policy for joints without an authored overlay
     * contribution. */
    enum class GraphMissingJointPolicy : std::uint8_t {
        SkeletonReference,
        PreserveBase,
        Count
    };

    /** @brief Stable skeleton-local mask entry; weights must be finite in [0,1]. */
    struct GraphBoneWeight final {
        JointId joint{};
        float weight{1.0F};
        bool operator==(const GraphBoneWeight &) const = default;
    };

    /** @brief Bounded sparse mask resolved to exact skeleton order during
     * compilation. */
    struct GraphBoneMask final {
        float defaultWeight{1.0F}; /**< Applies to joints not explicitly listed. */
        std::vector<GraphBoneWeight> joints = std::vector<GraphBoneWeight>(std::size_t{0});
        bool operator==(const GraphBoneMask &) const = default;
    };

    /** @brief Ordered pose layer; additive modes require an explicit ReferencePose
     * operand.
     * @details FirstPose is the base, SecondPose the overlay. Finite Weight is
     * clamped to [0,1], then multiplied by the mask. Normalized weights sum to one
     * per joint. AdditiveLocal postmultiplies a reference-relative rotation delta;
     * AdditiveParent premultiplies it. Translation and scale remain local in both
     * modes.
     */
    struct GraphBlendNode final {
        GraphBlendMode mode{GraphBlendMode::Normalized};
        GraphBoneMask mask{};
        GraphMissingJointPolicy missingJoint{GraphMissingJointPolicy::SkeletonReference};
        bool operator==(const GraphBlendNode &) const = default;
    };

    struct GraphBlendSampleTag;
    struct GraphSyncGroupTag;
    /** @brief Stable tree-local member identity; never a player index. */
    using GraphBlendSampleId = Foundation::Detail::NonZeroId64<GraphBlendSampleTag, AnimationErrors::IdentityInvalid>;
    /** @brief Explicit instance-wide sync namespace, shared by calls; absent
     * identity creates an occurrence-local group. */
    using GraphSyncGroupId = Foundation::Detail::NonZeroId64<GraphSyncGroupTag, AnimationErrors::IdentityInvalid>;

    /** @brief Blend-space coordinate dimensionality. */
    enum class GraphBlendDimension : std::uint8_t {
        One,
        Two,
        Count
    };
    /** @brief Explicit outside-domain behavior; nearest projects to an authored
     * triangle edge in 2D. */
    enum class GraphBlendOutside : std::uint8_t {
        ClampToDomain,
        Reject,
        Count
    };
    /** @brief Group synchronization uses exact clip phases; marker mode falls back
     * to normalized phase when sequences differ. */
    enum class GraphSyncMode : std::uint8_t {
        Independent,
        NormalizedPhase,
        Markers,
        Count
    };
    /** @brief Eligibility exported for the clip-event producer; this policy does
     * not manufacture clip notifies. */
    enum class GraphSyncEventPolicy : std::uint8_t {
        LeaderOnly,
        AllActive,
        Suppress,
        Count
    };

    /** @brief Explicit local-space leader scoring; generic outer pose-layer weights
     * and masks are not inferred. */
    enum class GraphSyncLeaderPolicy : std::uint8_t {
        LocalSampleWeightWithStateContribution,
        Count
    };

    /** @brief Group policy must agree across all nodes carrying the same explicit
     * group identity. */
    struct GraphSyncPolicy final {
        GraphSyncMode mode{GraphSyncMode::NormalizedPhase};
        GraphSyncEventPolicy events{GraphSyncEventPolicy::LeaderOnly};
        GraphSyncLeaderPolicy leader{GraphSyncLeaderPolicy::LocalSampleWeightWithStateContribution};
        bool operator==(const GraphSyncPolicy &) const = default;
    };

    /** @brief One immutable clip member at a finite authored coordinate; clips must
     * be absolute. */
    struct GraphBlendSample final {
        GraphBlendSampleId id{};
        AnimationClipId clip{};
        float x{};
        float y{};
        bool canLead{true}; /**< Followers are eligible only when no active leading
                               member exists. */
        bool operator==(const GraphBlendSample &) const = default;
    };

    /** @brief Authored nondegenerate triangle in stable sample identities;
     * authoring order breaks equal-distance hull ties. */
    struct GraphBlendTriangle final {
        std::array<GraphBlendSampleId, 3> samples{};
        bool operator==(const GraphBlendTriangle &) const = default;
    };

    /** @brief Direct multi-clip pose writer with bounded cold triangulation and
     * instance-scoped synchronization.
     * @details Samples are canonicalized by identity. 1D x coordinates must be
     * distinct; 2D triangles must form a nonoverlapping simplicial complex.
     * Coordinate parameters are exact Float declarations. Zero-weight members do
     * not advance or emit events. The highest-weight eligible member leads
     * (including active state-transition contributions), with canonical
     * occurrence/sample ties. Generic outer layers do not add implicit group
     * weights. Explicit group identities join across reusable definitions in this
     * instance only.
     */
    struct GraphBlendTreeNode final {
        GraphBlendDimension dimension{GraphBlendDimension::One};
        GraphParameterId x{};
        GraphParameterId y{}; /**< Absent for 1D. */
        GraphBlendOutside outside{GraphBlendOutside::ClampToDomain};
        std::vector<GraphBlendSample> samples = std::vector<GraphBlendSample>(std::size_t{0});
        std::vector<GraphBlendTriangle> triangles = std::vector<GraphBlendTriangle>(std::size_t{0});
        GraphSyncGroupId group{};
        GraphSyncPolicy sync{};
        bool operator==(const GraphBlendTreeNode &) const = default;
    };

    /** @brief Typed parameter read by stable identity. */
    struct GraphParameterNode final {
        GraphParameterId parameter{};
        bool operator==(const GraphParameterNode &) const = default;
    };

    /** @brief Final pose sink; exactly one per definition. */
    struct GraphOutputNode final {
        bool operator==(const GraphOutputNode &) const = default;
    };

    /** @brief Subgraph input read by stable interface identity. */
    struct GraphInputNode final {
        GraphInterfaceId input{};
        bool operator==(const GraphInputNode &) const = default;
    };

    /** @brief Call to an asset-local definition; external graph discovery is
     * forbidden. */
    struct GraphCallNode final {
        GraphDefinitionId definition{};
        bool operator==(const GraphCallNode &) const = default;
    };

    /** @brief Typed condition vocabulary; floats use separate enter/release
     * thresholds. */
    enum class GraphStatePredicate : std::uint8_t {
        Above,
        Below,
        Equal,
        NotEqual,
        Trigger
    };
    /** @brief Permitted higher-priority interruption origins of an active
     * transition. */
    enum class GraphStateInterruption : std::uint8_t {
        None,
        Source,
        Destination,
        Either
    };
    /** @brief Finite scalar transition-weight curves; pose composition remains a
     * separate kernel. */
    enum class GraphStateBlendCurve : std::uint8_t {
        Linear,
        SmoothStep,
        EaseIn,
        EaseOut
    };

    /** @brief One parameter condition; immutable authoring data, never a runtime
     * latch. */
    struct GraphStateCondition final {
        GraphParameterId parameter{};
        GraphStatePredicate predicate{};
        GraphParameterValue threshold{0.0F};
        float releaseThreshold{};
        bool operator==(const GraphStateCondition &) const = default;
    };

    /** @brief A state references an exact clip or an asset-local definition without
     * external interface inputs. */
    struct GraphState final {
        GraphStateId id{};
        std::variant<AnimationClipId, GraphDefinitionId> source{};
        std::optional<AnimationDeltaTime> loopPeriod{}; /**< Explicit subgraph cycle; direct clips derive their
                                                           cycle from the clip descriptor. */
        bool operator==(const GraphState &) const = default;
    };

    /** @brief Typed transition; stable identity breaks priority ties and survives
     * canonicalization. */
    struct GraphStateTransition final {
        GraphTransitionId id{};
        GraphStateId source{};
        GraphStateId destination{};
        std::int32_t priority{};
        AnimationDeltaTime duration{};
        std::optional<AnimationTime> exitTime{}; /**< Nonnegative authored elapsed time since entry, including
                                                    loops. */
        GraphStateInterruption interruption{GraphStateInterruption::None};
        GraphStateBlendCurve curve{GraphStateBlendCurve::Linear};
        std::vector<GraphStateCondition> conditions = std::vector<GraphStateCondition>(std::size_t{0});
        bool operator==(const GraphStateTransition &) const = default;
    };

    /** @brief Stateful pose writer; all states and transition rules are inert
     * bounded asset data. */
    struct GraphStateMachineNode final {
        GraphStateId initial{};
        std::vector<GraphState> states = std::vector<GraphState>(std::size_t{0});
        std::vector<GraphStateTransition> transitions = std::vector<GraphStateTransition>(std::size_t{0});
        bool operator==(const GraphStateMachineNode &) const = default;
    };

    struct GraphActionSlotTag;
    struct GraphActionTag;
    struct GraphActionSectionTag;
    struct GraphActionLoopTag;
    /** @brief Stable graph-wide named action-slot identity, deliberately shared by
     * called definitions. */
    using GraphActionSlotId = Foundation::Detail::NonZeroId64<GraphActionSlotTag, AnimationErrors::IdentityInvalid>;
    /** @brief Stable graph-wide immutable action-sequence identity. */
    using GraphActionId = Foundation::Detail::NonZeroId64<GraphActionTag, AnimationErrors::IdentityInvalid>;
    /** @brief Stable action-local section identity. */
    using GraphActionSectionId = Foundation::Detail::NonZeroId64<GraphActionSectionTag, AnimationErrors::IdentityInvalid>;
    /** @brief Stable action-local loop-region identity. */
    using GraphActionLoopId = Foundation::Detail::NonZeroId64<GraphActionLoopTag, AnimationErrors::IdentityInvalid>;

    /** @brief One instance-wide named composition channel; a name is bounded tool
     * metadata, while id is authoritative. */
    struct GraphActionSlot final {
        GraphActionSlotId id{};
        std::string name = std::string(std::size_t{0}, char{});
        bool operator==(const GraphActionSlot &) const = default;
    };

    /** @brief Exact clip interval and canonical successor; absent next finishes the
     * sequence. */
    struct GraphActionSection final {
        GraphActionSectionId id{};
        std::string name = std::string(std::size_t{0}, char{});
        AnimationClipId clip{};
        AnimationTime begin{};
        AnimationTime end{};
        GraphActionSectionId next{};
        bool operator==(const GraphActionSection &) const = default;
    };
    /** @brief Explicit loop termination policy; UntilReleased reserves
     * repetitions==0. */
    enum class GraphActionLoopMode : std::uint8_t {
        Finite,
        UntilReleased,
        Count
    };

    /** @brief Nonoverlapping section-path loop; last.next is the sole exit
     * authority.
     * @details The ordinary next graph is acyclic. This declaration alone
     * authorizes last->first repetition. Finite repetitions includes the initial
     * pass; an explicit start inside a region counts its partial initial pass once.
     */
    struct GraphActionLoopRegion final {
        GraphActionLoopId id{};
        GraphActionSectionId first{};
        GraphActionSectionId last{};
        GraphActionLoopMode mode{GraphActionLoopMode::Finite};
        std::uint32_t repetitions{1};
        bool operator==(const GraphActionLoopRegion &) const = default;
    };

    /** @brief Immutable graph-local animation sequence, independent of cinematic
     * player ownership. */
    struct GraphActionSequence final {
        GraphActionSectionId entry{};
        std::vector<GraphActionSection> sections = std::vector<GraphActionSection>(std::size_t{0});
        std::vector<GraphActionLoopRegion> loops = std::vector<GraphActionLoopRegion>(std::size_t{0});
        bool operator==(const GraphActionSequence &) const = default;
    };
    /** @brief Whether a new interrupt request may replace the active action.
     * Explicit owner cancellation remains available. */
    enum class GraphActionInterruption : std::uint8_t {
        Reject,
        AtSectionBoundary,
        Immediate,
        Count
    };
    /** @brief Reverse playback is an explicit action policy, not reverse simulation
     * time. */
    enum class GraphActionReverse : std::uint8_t {
        Reject,
        Allow,
        Count
    };

    /** @brief Prepared action sequence and its sole slot/mask/envelope policies.
     * @details Actions advance once per instance tick even when a slot output is
     * masked or not visited. The underlying graph also keeps advancing. Blend-out
     * holds the terminal action sample while handing back to the current base;
     * it never restores a stale pre-action whole-body pose.
     */
    struct GraphActionDefinition final {
        GraphActionId id{};
        std::string name = std::string(std::size_t{0}, char{});
        GraphActionSlotId slot{};
        GraphActionSequence sequence{};
        GraphBoneMask mask{};
        AnimationDeltaTime blendIn{};
        AnimationDeltaTime blendOut{};
        GraphStateBlendCurve curve{GraphStateBlendCurve::Linear};
        GraphActionInterruption interruption{GraphActionInterruption::Immediate};
        GraphActionReverse reverse{GraphActionReverse::Reject};
        GraphMissingJointPolicy missingJoint{GraphMissingJointPolicy::PreserveBase};
        bool operator==(const GraphActionDefinition &) const = default;
    };

    /** @brief Base Pose to Result Pose writer for one declared slot; repeated
     * occurrences share the same request/cursor state. */
    struct GraphActionSlotNode final {
        GraphActionSlotId slot{};
        bool operator==(const GraphActionSlotNode &) const = default;
    };

    /** @brief Supported node semantics; future nodes require a versioned compiler
     * contract. */
    using GraphNodePayload = std::variant<GraphClipNode, GraphBlendNode, GraphParameterNode, GraphOutputNode, GraphInputNode, GraphCallNode,
                                          GraphStateMachineNode, GraphBlendTreeNode, GraphActionSlotNode>;

    /** @brief Authoring node with stable pins and typed semantics. */
    struct GraphNode final {
        GraphNodeId id{};
        GraphNodePayload payload{};
        std::vector<GraphPin> pins = std::vector<GraphPin>(std::size_t{0});
        bool operator==(const GraphNode &) const = default;
    };

    /** @brief Stable source/destination pin endpoint, scoped to its definition. */
    struct GraphEndpoint final {
        GraphNodeId node{};
        GraphPinId pin{};
        auto operator<=>(const GraphEndpoint &) const = default;
    };

    /** @brief Explicit typed edge; one source must feed every input pin. */
    struct GraphConnection final {
        GraphEndpoint source{};
        GraphEndpoint destination{};
        GraphValueType type{GraphValueType::Pose};
        auto operator<=>(const GraphConnection &) const = default;
    };

    /** @brief Reusable acyclic definition with one pose output. */
    struct GraphDefinition final {
        GraphDefinitionId id{};
        std::vector<GraphInterfaceInput> inputs = std::vector<GraphInterfaceInput>(std::size_t{0});
        std::vector<GraphNode> nodes = std::vector<GraphNode>(std::size_t{0});
        std::vector<GraphConnection> connections = std::vector<GraphConnection>(std::size_t{0});
        bool operator==(const GraphDefinition &) const = default;
    };

    /** @brief Complete detached schema-5 candidate retaining pose/state policies
     * and adding typed blend spaces and sync groups. */
    struct AnimationGraphData final {
        /** @brief Creates detached fields; checked-library bookkeeping may throw std::bad_alloc. */
        AnimationGraphData() = default;
        /** @brief Copies detached fields. @param other Value to copy. @throws std::bad_alloc If copying cannot allocate. */
        AnimationGraphData(const AnimationGraphData &other) = default;
        /** @brief Copies fields. @param other Value to copy. @return This value. @throws std::bad_alloc If copying cannot allocate. */
        AnimationGraphData &operator=(const AnimationGraphData &other) = default;
        /** @brief Prepares empty buffers before transferring ownership. @param other Source whose buffers become empty.
         * @throws std::bad_alloc Leaving the source unchanged when buffer bookkeeping cannot allocate. */
        AnimationGraphData(AnimationGraphData &&other);
        /** @brief Replaces buffers without allocation. @param other Source to consume. @return This value. */
        AnimationGraphData &operator=(AnimationGraphData &&other) noexcept = default;
        std::uint32_t schemaVersion{CurrentAnimationGraphSchemaVersion};
        AnimationGraphContractVersion contractVersion{CurrentAnimationGraphContractVersion};
        AnimationGraphId id{};
        SkeletonId skeleton{};
        GraphDefinitionId entry{};
        std::vector<GraphParameter> parameters = std::vector<GraphParameter>(std::size_t{0});
        std::vector<GraphDefinition> definitions = std::vector<GraphDefinition>(std::size_t{0});
        std::vector<GraphActionSlot> actionSlots = std::vector<GraphActionSlot>(std::size_t{0});
        std::vector<GraphActionDefinition> actions = std::vector<GraphActionDefinition>(std::size_t{0});
        bool operator==(const AnimationGraphData &) const = default;
    };

    /** @brief Implementation safety ceilings independent of caller compile policy.
     */
    struct AnimationGraphHardLimits final {
        static constexpr std::uint32_t Definitions = 64;
        static constexpr std::uint32_t Nodes = 4096;
        static constexpr std::uint32_t Pins = 16384;
        static constexpr std::uint32_t Connections = 8192;
        static constexpr std::uint32_t Parameters = 256;
        static constexpr std::uint32_t CallDepth = 32;
        static constexpr std::uint32_t EvaluationInstructions = 65536;
        static constexpr std::uint32_t ParameterNameBytes = 64;
        static constexpr std::uint32_t SourceMapCallEntries = 131072;
        static constexpr std::uint32_t PoseSlots = 4096;
        static constexpr std::uint32_t ScalarSlots = 4096;
        static constexpr std::uint32_t ClipPlayers = 4096;
        static constexpr std::uint64_t PoseTransforms = 1048576;
        static constexpr std::uint32_t StateMachines = 1024;
        static constexpr std::uint32_t States = 256;
        static constexpr std::uint32_t Transitions = 2048;
        static constexpr std::uint32_t Conditions = 4096;
        static constexpr std::uint32_t StateEvents = 262144;
        static constexpr std::uint32_t BlendSamplesPerTree = 32;
        static constexpr std::uint32_t BlendTrianglesPerTree = 64;
        static constexpr std::uint32_t BlendMembers = 4096;
        static constexpr std::uint32_t ActionSlots = 32;
        static constexpr std::uint32_t Actions = 256;
        static constexpr std::uint32_t ActionSections = 4096;
        static constexpr std::uint32_t ActionLoops = 256;
        static constexpr std::uint32_t ActionRequests = 256;
        static constexpr std::uint32_t ActionTransitions = 4096;
    };

    /** @brief Finite compile policy; all counts apply across the whole asset. */
    struct AnimationGraphLimits final {
        std::uint32_t definitions{AnimationGraphHardLimits::Definitions};
        std::uint32_t nodes{AnimationGraphHardLimits::Nodes};
        std::uint32_t pins{AnimationGraphHardLimits::Pins};
        std::uint32_t connections{AnimationGraphHardLimits::Connections};
        std::uint32_t parameters{AnimationGraphHardLimits::Parameters};
        std::uint32_t callDepth{AnimationGraphHardLimits::CallDepth};
        std::uint32_t evaluationInstructions{AnimationGraphHardLimits::EvaluationInstructions};
        std::uint32_t sourceMapCallEntries{AnimationGraphHardLimits::SourceMapCallEntries};
        std::uint32_t poseSlots{AnimationGraphHardLimits::PoseSlots};
        std::uint32_t scalarSlots{AnimationGraphHardLimits::ScalarSlots};
        std::uint32_t clipPlayers{AnimationGraphHardLimits::ClipPlayers};
        std::uint64_t poseTransforms{AnimationGraphHardLimits::PoseTransforms};
        std::uint32_t stateMachines{AnimationGraphHardLimits::StateMachines};
        std::uint32_t states{AnimationGraphHardLimits::States};
        std::uint32_t transitions{AnimationGraphHardLimits::Transitions};
        std::uint32_t conditions{AnimationGraphHardLimits::Conditions};
        std::uint32_t stateEvents{AnimationGraphHardLimits::StateEvents};
        std::uint32_t actionSlots{AnimationGraphHardLimits::ActionSlots};
        std::uint32_t actions{AnimationGraphHardLimits::Actions};
        std::uint32_t actionSections{AnimationGraphHardLimits::ActionSections};
        std::uint32_t actionLoops{AnimationGraphHardLimits::ActionLoops};
    };

    /** @brief Captured synchronous admission state; no work survives the
     * invocation. */
    struct AnimationGraphCompileContext final {
        AnimationGraphLimits limits{};
        CancellationToken cancellation{};
        bool accepting{true};
        std::optional<AnimationGraphId> replacing{};
        const SkeletonAsset *skeleton{};              /**< Invocation-scoped validated immutable
                                                         dependency snapshot. */
        SkeletonAssetGeneration skeletonGeneration{}; /**< Generation issued by the caller's skeleton
                                                         publication owner. */
        std::span<const AnimationClipAsset> clips{};  /**< Exact validated clip dependencies, pinned by the caller
                                                         through compilation. */
    };

    /** @brief Stable source location for compiler/editor mapping, independent of
     * input vector order. */
    struct GraphSourceLocation final {
        GraphDefinitionId definition{};
        GraphNodeId node{};
        GraphPinId pin{};
        auto operator<=>(const GraphSourceLocation &) const = default;
    };

    /** @brief Canonical instruction input; register indexes address an earlier
     * instruction in this definition. */
    struct GraphCompiledInput final {
        GraphPinId destination{};
        std::uint32_t instruction{};
        GraphSourceLocation source{};
        GraphValueType type{};
        bool operator==(const GraphCompiledInput &) const = default;
    };

    /** @brief Typed immutable opcode and exact authoring source; pins are canonical
     * by stable id. */
    struct GraphInstruction final {
        GraphSourceLocation source{};
        GraphNodePayload payload{};
        std::vector<GraphPin> pins = std::vector<GraphPin>(std::size_t{0});
        std::vector<GraphCompiledInput> inputs = std::vector<GraphCompiledInput>(std::size_t{0});
        std::optional<std::uint32_t> parameterIndex{};
        std::optional<std::uint32_t> actionSlot{};
        std::array<std::uint32_t, 2> blendParameters{};
        std::optional<std::uint32_t> definitionIndex{};
        std::vector<std::uint32_t> stateDefinitions = std::vector<std::uint32_t>(std::size_t{0}); /**< Definition indexes for state call
                                                          edges; clips are excluded. */
        bool operator==(const GraphInstruction &) const = default;
    };

    /** @brief Canonical subprogram; all data dependencies precede their consumer.
     */
    struct GraphCompiledDefinition final {
        /** @brief Creates detached fields; checked-library bookkeeping may throw std::bad_alloc. */
        GraphCompiledDefinition() = default;
        /** @brief Copies detached fields. @param other Value to copy. @throws std::bad_alloc If copying cannot allocate. */
        GraphCompiledDefinition(const GraphCompiledDefinition &other) = default;
        /** @brief Copies fields. @param other Value to copy. @return This value. @throws std::bad_alloc If copying cannot allocate. */
        GraphCompiledDefinition &operator=(const GraphCompiledDefinition &other) = default;
        /** @brief Prepares empty buffers before transferring ownership. @param other Source whose buffers become empty.
         * @throws std::bad_alloc Leaving the source unchanged when buffer bookkeeping cannot allocate. */
        GraphCompiledDefinition(GraphCompiledDefinition &&other);
        /** @brief Replaces buffers without allocation. @param other Source to consume. @return This value. */
        GraphCompiledDefinition &operator=(GraphCompiledDefinition &&other) noexcept = default;
        GraphDefinitionId id{};
        std::vector<GraphInterfaceInput> inputs = std::vector<GraphInterfaceInput>(std::size_t{0});
        std::vector<GraphInstruction> instructions = std::vector<GraphInstruction>(std::size_t{0});
        std::uint32_t outputInstruction{};
        std::uint32_t callDepth{};
        std::uint64_t evaluationInstructions{}; /**< Counts repeated subgraph calls,
                                                   bounded to 65536. */
        std::uint64_t sourceMapCallEntries{};   /**< Sum of call-path and typed state-guard
                                                   entries across expanded occurrences. */
        bool operator==(const GraphCompiledDefinition &) const = default;
    };

    /** @brief Type-specific preallocated working-storage slot; Pose slots contain a
     * complete skeleton pose. */
    struct GraphWorkingSlot final {
        GraphValueType type{};
        std::uint32_t index{};
        auto operator<=>(const GraphWorkingSlot &) const = default;
    };

    /** @brief Exact operand access to its actual working-slot writer, including an
     * Input node's interface alias. destination is the local consumer pin; an
     * interface alias uses its Result pin.
     */
    struct GraphOccurrenceInput final {
        GraphPinId destination{};
        GraphWorkingSlot slot{};
        std::uint32_t sourceOccurrence{};
        bool operator==(const GraphOccurrenceInput &) const = default;
    };

    /** @brief One compiler-resolved active-branch guard; lanes distinguish
     * self-transition incarnations. */
    struct GraphStateGuard final {
        std::uint32_t machine{};
        std::uint32_t state{};
        std::uint8_t lane{};
        bool operator==(const GraphStateGuard &) const = default;
    };
    /** @brief State control precedes guarded children; completion writes the
     * composed pose afterward. */
    enum class GraphOccurrencePhase : std::uint8_t {
        Evaluate,
        StateBegin,
        StateEnd
    };

    /** @brief One exact immutable clip/player member admitted by the compiler. */
    struct GraphBlendSampleBinding final {
        /** @brief Creates detached fields; checked-library bookkeeping may throw std::bad_alloc. */
        GraphBlendSampleBinding() = default;
        /** @brief Copies detached fields. @param other Value to copy. @throws std::bad_alloc If copying cannot allocate. */
        GraphBlendSampleBinding(const GraphBlendSampleBinding &other) = default;
        /** @brief Copies fields. @param other Value to copy. @return This value. @throws std::bad_alloc If copying cannot allocate. */
        GraphBlendSampleBinding &operator=(const GraphBlendSampleBinding &other) = default;
        /** @brief Prepares empty buffers before transferring ownership. @param other Source whose buffers become empty.
         * @throws std::bad_alloc Leaving the source unchanged when buffer bookkeeping cannot allocate. */
        GraphBlendSampleBinding(GraphBlendSampleBinding &&other);
        /** @brief Replaces buffers without allocation. @param other Source to consume. @return This value. */
        GraphBlendSampleBinding &operator=(GraphBlendSampleBinding &&other) noexcept = default;
        std::uint32_t player{};
        std::uint32_t clip{};
        std::uint32_t member{};
        std::vector<std::uint8_t> coverage = std::vector<std::uint8_t>(std::size_t{0});
        bool operator==(const GraphBlendSampleBinding &) const = default;
    };

    /** @brief Exact compiler-owned access plan; state control precedes guarded
     * children, pose completion follows them. Input, Output and Call alias their
     * real writer; inline state clips use their exact clip binding/player access.
     */
    struct GraphInstructionOccurrence final {
        /** @brief Creates detached fields; checked-library bookkeeping may throw std::bad_alloc. */
        GraphInstructionOccurrence() = default;
        /** @brief Copies detached fields. @param other Value to copy. @throws std::bad_alloc If copying cannot allocate. */
        GraphInstructionOccurrence(const GraphInstructionOccurrence &other) = default;
        /** @brief Copies fields. @param other Value to copy. @return This value. @throws std::bad_alloc If copying cannot allocate. */
        GraphInstructionOccurrence &operator=(const GraphInstructionOccurrence &other) = default;
        /** @brief Prepares empty buffers before transferring ownership. @param other Source whose buffers become empty.
         * @throws std::bad_alloc Leaving the source unchanged when buffer bookkeeping cannot allocate. */
        GraphInstructionOccurrence(GraphInstructionOccurrence &&other);
        /** @brief Replaces buffers without allocation. @param other Source to consume. @return This value. */
        GraphInstructionOccurrence &operator=(GraphInstructionOccurrence &&other) noexcept = default;
        GraphSourceLocation source{};
        std::vector<GraphSourceLocation> callPath = std::vector<GraphSourceLocation>(std::size_t{0}); /**< Root-to-leaf stable call nodes;
                                                        repeated calls remain distinct. */
        std::uint32_t definitionIndex{};
        std::uint32_t instructionIndex{};
        std::vector<GraphOccurrenceInput> inputs = std::vector<GraphOccurrenceInput>(std::size_t{0});
        GraphWorkingSlot output{};
        std::uint32_t outputProducer{}; /**< Exact Clip/Blend/Parameter writer; self
                                           for writes, earlier for aliases. */
        std::optional<std::uint32_t> clipPlayer{};
        std::optional<std::uint32_t> clipBindingIndex{};
        std::optional<std::uint32_t> parameterRead{};
        std::optional<std::uint32_t> actionSlot{};
        std::optional<std::uint32_t> stateMachine{};
        GraphOccurrencePhase phase{GraphOccurrencePhase::Evaluate};
        std::vector<GraphStateGuard> guards = std::vector<GraphStateGuard>(std::size_t{0});
        std::vector<float> jointMask = std::vector<float>(std::size_t{0});                  /**< Complete dense blend mask, bound to exact
                                                            skeleton order. */
        std::vector<std::uint8_t> clipCoverage = std::vector<std::uint8_t>(std::size_t{0}); /**< Complete dense authored-track
                                                     coverage for a clip writer. */
        std::vector<GraphBlendSampleBinding> blendSamples = std::vector<GraphBlendSampleBinding>(std::size_t{0});
        std::optional<GraphWorkingSlot> blendScratch{};
        std::optional<std::uint32_t> blendTree{};
        std::array<std::uint32_t, 2> blendParameters{};
        bool operator==(const GraphInstructionOccurrence &) const = default;
    };

    /** @brief Actual per-lane state execution storage; ranges reset only when that
     * state incarnation enters. */
    struct GraphStateBranch final {
        GraphStateId id{};
        std::array<GraphWorkingSlot, 2> output{};
        std::array<std::uint32_t, 2> begin{};
        std::array<std::uint32_t, 2> end{};
        std::optional<AnimationDeltaTime> loopPeriod{};
        bool operator==(const GraphStateBranch &) const = default;
    };

    /** @brief Compiler-owned complete machine occurrence; no runtime pointer or
     * generation is retained. */
    struct GraphStateMachineBinding final {
        /** @brief Creates detached fields; checked-library bookkeeping may throw std::bad_alloc. */
        GraphStateMachineBinding() = default;
        /** @brief Copies detached fields. @param other Value to copy. @throws std::bad_alloc If copying cannot allocate. */
        GraphStateMachineBinding(const GraphStateMachineBinding &other) = default;
        /** @brief Copies fields. @param other Value to copy. @return This value. @throws std::bad_alloc If copying cannot allocate. */
        GraphStateMachineBinding &operator=(const GraphStateMachineBinding &other) = default;
        /** @brief Prepares empty buffers before transferring ownership. @param other Source whose buffers become empty.
         * @throws std::bad_alloc Leaving the source unchanged when buffer bookkeeping cannot allocate. */
        GraphStateMachineBinding(GraphStateMachineBinding &&other);
        /** @brief Replaces buffers without allocation. @param other Source to consume. @return This value. */
        GraphStateMachineBinding &operator=(GraphStateMachineBinding &&other) noexcept = default;
        std::uint32_t beginOccurrence{};
        std::uint32_t endOccurrence{};
        std::uint32_t conditionOffset{};
        std::array<GraphWorkingSlot, 4> poses{}; /**< Committed/candidate composed and committed/candidate frozen
                                                    source buffers. */
        std::vector<GraphStateBranch> branches = std::vector<GraphStateBranch>(std::size_t{0});
        bool operator==(const GraphStateMachineBinding &) const = default;
    };

    /** @brief Exact immutable dependency compatibility captured without retaining
     * caller-owned views. */
    struct GraphSkeletonBinding final {
        SkeletonId id{};
        SkeletonAssetGeneration generation{};
        SkeletonAssetContractVersion contractVersion{};
        std::uint32_t joints{};
        auto operator<=>(const GraphSkeletonBinding &) const = default;
    };

    /** @brief Complete finite allocation requirements for a future graph instance,
     * not an evaluator. */
    struct GraphMemoryRequirements final {
        std::array<std::uint32_t, 5> workingSlots{}; /**< Indexed by known GraphValueType; every type has
                                                        separate storage. */
        std::uint64_t poseTransforms{};
        std::uint32_t clipPlayers{};
        std::uint32_t blendTrees{};
        std::uint32_t blendMembers{};
        std::uint32_t actionSlots{};
        std::uint32_t parameters{};
        std::uint32_t callDepth{};
        std::uint32_t stateMachines{};
        std::uint32_t conditionLatches{};
        std::uint32_t stateEvents{};
        std::uint64_t compositionEntries{}; /**< Immutable dense mask/coverage
                                               elements; bounded by poseTransforms. */
        bool operator==(const GraphMemoryRequirements &) const = default;
    };

    /**
     * @brief Detached target-independent compiled declarations for a cooked graph payload.
     * @details Contains no source document, native pointers, runtime publication generations or
     *          dependency-bound occurrence layout. Stable compiled order, operand provenance and
     *          declared expansion costs are verified before dependency binding. Binary codecs
     *          must bound every table before allocation and encode fields explicitly.
     */
    struct AnimationGraphCookedData final {
        /** @brief Creates detached fields; checked-library bookkeeping may throw std::bad_alloc. */
        AnimationGraphCookedData() = default;
        /** @brief Copies detached fields. @param other Value to copy. @throws std::bad_alloc If copying cannot allocate. */
        AnimationGraphCookedData(const AnimationGraphCookedData &other) = default;
        /** @brief Copies fields. @param other Value to copy. @return This value. @throws std::bad_alloc If copying cannot allocate. */
        AnimationGraphCookedData &operator=(const AnimationGraphCookedData &other) = default;
        /** @brief Prepares empty buffers before transferring ownership. @param other Source whose buffers become empty.
         * @throws std::bad_alloc Leaving the source unchanged when buffer bookkeeping cannot allocate. */
        AnimationGraphCookedData(AnimationGraphCookedData &&other);
        /** @brief Replaces buffers without allocation. @param other Source to consume. @return This value. */
        AnimationGraphCookedData &operator=(AnimationGraphCookedData &&other) noexcept = default;
        AnimationGraphContractVersion contractVersion{CurrentAnimationGraphContractVersion};
        AnimationGraphId id{};
        SkeletonId skeleton{};
        GraphDefinitionId entry{};
        std::vector<GraphParameter> parameters = std::vector<GraphParameter>(std::size_t{0});
        std::vector<GraphActionSlot> actionSlots = std::vector<GraphActionSlot>(std::size_t{0});
        std::vector<GraphActionDefinition> actions = std::vector<GraphActionDefinition>(std::size_t{0});
        std::vector<GraphCompiledDefinition> definitions = std::vector<GraphCompiledDefinition>(std::size_t{0});
    };

    namespace Detail {
        struct ClipSceneProgramAccess;
    }

    /** @brief Owned validated program; runtime instances borrow immutable spans and
     * own mutable state. */
    class AnimationGraphProgram final {
    public:
        /** @brief Copies detached fields. @param other Value to copy. @throws std::bad_alloc If copying cannot allocate. */
        AnimationGraphProgram(const AnimationGraphProgram &other) = default;
        /** @brief Copies fields. @param other Value to copy. @return This value. @throws std::bad_alloc If copying cannot allocate. */
        AnimationGraphProgram &operator=(const AnimationGraphProgram &other) = default;
        /** @brief Prepares empty buffers before transferring ownership. @param other Source whose buffers become empty.
         * @throws std::bad_alloc Leaving the source unchanged when buffer bookkeeping cannot allocate. */
        AnimationGraphProgram(AnimationGraphProgram &&other);
        /** @brief Replaces buffers without allocation. @param other Source to consume. @return This value. */
        AnimationGraphProgram &operator=(AnimationGraphProgram &&other) noexcept = default;
        /** @brief Compiles a detached candidate without publication or I/O.
         * @param data Current schema-6 candidate; migrate supported earlier source
         * contracts explicitly first.
         * @param context Captured limits, cancellation, optional replacement identity
         * and exact pinned dependencies.
         * @return Complete immutable program or stable graph failure; never partial
         * publication.
         * @pre The caller pins the supplied skeleton/clip snapshots and generation
         * for this synchronous call.
         * @post Success copies compatibility and retains no caller-owned
         * pointer/span; publication must recheck snapshot authority.
         * @throws std::bad_alloc When compile-time storage cannot be allocated.
         */
        [[nodiscard]] static Result<AnimationGraphProgram> Compile(const AnimationGraphData &data,
                                                                   const AnimationGraphCompileContext &context = {});

        /**
         * @brief Verifies a cooked instruction schedule and binds actual immutable dependencies.
         * @param data Bounded detached cooked declarations; never source JSON or a native object dump.
         * @param context Current policy and host-issued skeleton/clip publication generations.
         * @return Complete immutable program, or a typed failure before any publication.
         * @details Reuses canonical node, catalog and binding validation. It does not select a
         *          new instruction schedule, parse authoring source, or trust serialized runtime state.
         * @throws std::bad_alloc When bounded cold validation or binding storage cannot be allocated.
         */
        [[nodiscard]] static Result<AnimationGraphProgram> FromCooked(const AnimationGraphCookedData &data,
                                                                      const AnimationGraphCompileContext &context);

        /** @brief Returns the persistent graph identity, absent for an execution-local direct-clip program. */
        [[nodiscard]] AnimationGraphId Id() const noexcept {
            return id_;
        }

        /** @brief Returns exact authored Clip or Graph lineage, without manufacturing a graph asset for a clip. */
        [[nodiscard]] AnimationSourceReference Source() const noexcept {
            if (sourceClip_.IsValid())
                return {AnimationSourceKind::Clip, sourceClip_, {}};
            return {AnimationSourceKind::Graph, {}, id_};
        }

        /** @brief Returns the required skeleton identity. @return Persistent skeleton
         * id. */
        [[nodiscard]] SkeletonId Skeleton() const noexcept {
            return skeleton_;
        }

        /** @brief Returns canonical parameters. @return Immutable lifetime-bound
         * span. */
        [[nodiscard]] std::span<const GraphParameter> Parameters() const noexcept {
            return parameters_;
        }

        /** @brief Returns the immutable instance-wide slot namespace. @return
         * Program-owned canonical declaration span. */
        [[nodiscard]] std::span<const GraphActionSlot> ActionSlots() const noexcept {
            return actionSlots_;
        }

        /** @brief Returns immutable graph-local action sequences. @return
         * Program-owned canonical catalog span. */
        [[nodiscard]] std::span<const GraphActionDefinition> Actions() const noexcept {
            return actions_;
        }

        /** @brief Returns canonical definitions. @return Immutable lifetime-bound
         * span. */
        [[nodiscard]] std::span<const GraphCompiledDefinition> Definitions() const noexcept {
            return definitions_;
        }

        /** @brief Returns the entry definition index. @return Dense immutable program
         * index. */
        [[nodiscard]] std::uint32_t EntryIndex() const noexcept {
            return entry_;
        }

        /** @brief Returns sorted unique clip dependencies. @return Immutable
         * lifetime-bound span. */
        [[nodiscard]] std::span<const AnimationClipId> Clips() const noexcept {
            return clips_;
        }

        /** @brief Returns the complete entry occurrence/access plan. @return
         * Immutable lifetime-bound span. */
        [[nodiscard]] std::span<const GraphInstructionOccurrence> Occurrences() const noexcept {
            return occurrences_;
        }

        /** @brief Returns admitted state-machine occurrences and branch ranges.
         * @return Immutable program-owned bindings. */
        [[nodiscard]] std::span<const GraphStateMachineBinding> StateMachines() const noexcept {
            return stateMachines_;
        }

        /** @brief Returns the final pose access of the entry plan. @return Stable
         * preallocated Pose slot. */
        [[nodiscard]] GraphWorkingSlot OutputSlot() const noexcept {
            return occurrences_.back().output;
        }

        /** @brief Returns exact finite instance storage requirements. @return
         * Borrowed immutable counts. */
        [[nodiscard]] const GraphMemoryRequirements &Memory() const noexcept {
            return memory_;
        }

        /** @brief Returns the exact skeleton compatibility. @return Borrowed
         * immutable identity, generation and layout. */
        [[nodiscard]] const GraphSkeletonBinding &SkeletonBinding() const noexcept {
            return skeletonBinding_;
        }

        /** @brief Returns exact clip compatibility sorted by identity. @return
         * Immutable descriptor span. */
        [[nodiscard]] std::span<const AnimationClipDescriptor> ClipBindings() const noexcept {
            return clipBindings_;
        }

        bool operator==(const AnimationGraphProgram &) const = default;

    private:
        AnimationGraphProgram() = default;
        friend struct Detail::ClipSceneProgramAccess;
        /** @brief Prepares an execution-local absolute-clip plan through the same validator, binder and executor. */
        [[nodiscard]] static Result<AnimationGraphProgram> PrepareClip(AnimationClipId clip, const AnimationGraphCompileContext &context);
        /** @brief Validates authoring topology without runtime dependency admission.
         */
        [[nodiscard]] static Result<AnimationGraphProgram> CompileSchema(const AnimationGraphData &data,
                                                                         const AnimationGraphCompileContext &context);
        /** @brief Binds a validated declaration program through the one canonical dependency owner. */
        [[nodiscard]] static Result<AnimationGraphProgram> BindProgram(AnimationGraphProgram program,
                                                                       const AnimationGraphCompileContext &context);
        friend Result<AnimationGraphData> MigrateAnimationGraph(const AnimationGraphData &, const AnimationGraphCompileContext &);
        // Count construction lets checked-library bookkeeping failures reach the caller's allocation boundary.
        std::vector<GraphInstructionOccurrence> occurrences_ = std::vector<GraphInstructionOccurrence>(std::size_t{0});
        std::vector<GraphStateMachineBinding> stateMachines_ = std::vector<GraphStateMachineBinding>(std::size_t{0});
        GraphMemoryRequirements memory_{};
        GraphSkeletonBinding skeletonBinding_{};
        std::vector<AnimationClipDescriptor> clipBindings_ = std::vector<AnimationClipDescriptor>(std::size_t{0});
        AnimationGraphId id_{};
        AnimationClipId sourceClip_{};
        SkeletonId skeleton_{};
        std::vector<GraphParameter> parameters_ = std::vector<GraphParameter>(std::size_t{0});
        std::vector<GraphActionSlot> actionSlots_ = std::vector<GraphActionSlot>(std::size_t{0});
        std::vector<GraphActionDefinition> actions_ = std::vector<GraphActionDefinition>(std::size_t{0});
        std::vector<GraphCompiledDefinition> definitions_ = std::vector<GraphCompiledDefinition>(std::size_t{0});
        std::vector<AnimationClipId> clips_ = std::vector<AnimationClipId>(std::size_t{0});
        std::uint32_t entry_{};
    };

    /** @brief Explicitly migrates supported source contracts and infers version-1
     * edge types, retaining stable IDs.
     * @param data Detached version-1/2/3/current candidate with its exact semantic
     * version; input is never changed.
     * @param context Captured limits, admission and replacement identity.
     * @return Validated current candidate or stable failure; unknown versions,
     * legacy pose policies and legacy state payloads reject.
     * @throws std::bad_alloc When detached migration storage cannot be allocated.
     */
    [[nodiscard]] Result<AnimationGraphData> MigrateAnimationGraph(const AnimationGraphData &data,
                                                                   const AnimationGraphCompileContext &context = {});
}  // namespace Horo::Animation
