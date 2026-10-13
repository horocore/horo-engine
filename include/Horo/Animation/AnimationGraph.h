#pragma once

/** @file AnimationGraph.h
 * @brief Versioned animation graph authoring and bounded immutable compilation.
 */
#include "Horo/Animation/AnimationClip.h"
#include "Horo/Foundation/CancellationToken.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Horo::Animation {
    /** @brief Current authoring schema; earlier versions require explicit migration. */
    inline constexpr std::uint32_t CurrentAnimationGraphSchemaVersion = 4;

    /** @brief Exact graph semantic contract, independent of source-envelope migration. */
    struct AnimationGraphContractVersion final {
        std::uint16_t major{};
        std::uint16_t minor{};
        std::uint16_t patch{};
        auto operator<=>(const AnimationGraphContractVersion &) const = default;
    };

    /** @brief Graph semantic contract implemented by this compiler. */
    inline constexpr AnimationGraphContractVersion CurrentAnimationGraphContractVersion{1, 2, 0};

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
    /** @brief Stable definition-local input identity shared by calls and input nodes. */
    using GraphInterfaceId = Foundation::Detail::NonZeroId64<GraphInterfaceTag, AnimationErrors::IdentityInvalid>;
    /** @brief Stable state identity local to one state-machine node. */
    using GraphStateId = Foundation::Detail::NonZeroId64<GraphStateTag, AnimationErrors::IdentityInvalid>;
    /** @brief Stable transition identity local to one state-machine node. */
    using GraphTransitionId = Foundation::Detail::NonZeroId64<GraphTransitionTag, AnimationErrors::IdentityInvalid>;

    /** @brief Pin transport vocabulary; Unspecified is admitted only by version-1 migration. */
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
        std::string name{}; /**< Bounded ASCII identifier for display/binding; identity remains id. */
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
    /** @brief Semantic pin roles; blend inputs use FirstPose, SecondPose and Weight. */
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

    /** @brief A stable input or output pin; only Interface roles carry interfaceId. */
    struct GraphPin final {
        GraphPinId id{};
        GraphPinRole role{GraphPinRole::Result};
        GraphValueType type{GraphValueType::Pose};
        bool output{true};
        GraphInterfaceId interfaceId{};
        bool operator==(const GraphPin &) const = default;
    };

    /** @brief Clip sampling reference; the instance owner resolves its skeleton and generation. */
    struct GraphClipNode final {
        AnimationClipId clip{};
        std::optional<AnimationReferencePoseId> referencePose{}; /**< Additive reference identity supplied by the ReferencePose operand. */
        bool operator==(const GraphClipNode &) const = default;
    };

    /** @brief Normalized interpolation or reference-relative rotation delta application space. */
    enum class GraphBlendMode : std::uint8_t {
        Normalized,
        AdditiveLocal,
        AdditiveParent,
        Count
    };
    /** @brief Completion policy for joints without an authored overlay contribution. */
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

    /** @brief Bounded sparse mask resolved to exact skeleton order during compilation. */
    struct GraphBoneMask final {
        float defaultWeight{1.0F}; /**< Applies to joints not explicitly listed. */
        std::vector<GraphBoneWeight> joints{};
        bool operator==(const GraphBoneMask &) const = default;
    };

    /** @brief Ordered pose layer; additive modes require an explicit ReferencePose operand.
     * @details FirstPose is the base, SecondPose the overlay. Finite Weight is clamped
     * to [0,1], then multiplied by the mask. Normalized weights sum to one per joint.
     * AdditiveLocal postmultiplies a reference-relative rotation delta; AdditiveParent
     * premultiplies it. Translation and scale remain local in both modes.
     */
    struct GraphBlendNode final {
        GraphBlendMode mode{GraphBlendMode::Normalized};
        GraphBoneMask mask{};
        GraphMissingJointPolicy missingJoint{GraphMissingJointPolicy::SkeletonReference};
        bool operator==(const GraphBlendNode &) const = default;
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

    /** @brief Call to an asset-local definition; external graph discovery is forbidden. */
    struct GraphCallNode final {
        GraphDefinitionId definition{};
        bool operator==(const GraphCallNode &) const = default;
    };

    /** @brief Typed condition vocabulary; floats use separate enter/release thresholds. */
    enum class GraphStatePredicate : std::uint8_t {
        Above,
        Below,
        Equal,
        NotEqual,
        Trigger
    };
    /** @brief Permitted higher-priority interruption origins of an active transition. */
    enum class GraphStateInterruption : std::uint8_t {
        None,
        Source,
        Destination,
        Either
    };
    /** @brief Finite scalar transition-weight curves; pose composition remains a separate kernel. */
    enum class GraphStateBlendCurve : std::uint8_t {
        Linear,
        SmoothStep,
        EaseIn,
        EaseOut
    };

    /** @brief One parameter condition; immutable authoring data, never a runtime latch. */
    struct GraphStateCondition final {
        GraphParameterId parameter{};
        GraphStatePredicate predicate{};
        GraphParameterValue threshold{0.0F};
        float releaseThreshold{};
        bool operator==(const GraphStateCondition &) const = default;
    };

    /** @brief A state references an exact clip or an asset-local definition without external interface inputs. */
    struct GraphState final {
        GraphStateId id{};
        std::variant<AnimationClipId, GraphDefinitionId> source{};
        std::optional<AnimationDeltaTime>
            loopPeriod{}; /**< Explicit subgraph cycle; direct clips derive their cycle from the clip descriptor. */
        bool operator==(const GraphState &) const = default;
    };

    /** @brief Typed transition; stable identity breaks priority ties and survives canonicalization. */
    struct GraphStateTransition final {
        GraphTransitionId id{};
        GraphStateId source{};
        GraphStateId destination{};
        std::int32_t priority{};
        AnimationDeltaTime duration{};
        std::optional<AnimationTime> exitTime{}; /**< Nonnegative authored elapsed time since entry, including loops. */
        GraphStateInterruption interruption{GraphStateInterruption::None};
        GraphStateBlendCurve curve{GraphStateBlendCurve::Linear};
        std::vector<GraphStateCondition> conditions{};
        bool operator==(const GraphStateTransition &) const = default;
    };

    /** @brief Stateful pose writer; all states and transition rules are inert bounded asset data. */
    struct GraphStateMachineNode final {
        GraphStateId initial{};
        std::vector<GraphState> states{};
        std::vector<GraphStateTransition> transitions{};
        bool operator==(const GraphStateMachineNode &) const = default;
    };

    /** @brief Supported node semantics; future nodes require a versioned compiler contract. */
    using GraphNodePayload = std::variant<GraphClipNode, GraphBlendNode, GraphParameterNode, GraphOutputNode, GraphInputNode, GraphCallNode,
                                          GraphStateMachineNode>;

    /** @brief Authoring node with stable pins and typed semantics. */
    struct GraphNode final {
        GraphNodeId id{};
        GraphNodePayload payload{};
        std::vector<GraphPin> pins{};
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
        std::vector<GraphInterfaceInput> inputs{};
        std::vector<GraphNode> nodes{};
        std::vector<GraphConnection> connections{};
        bool operator==(const GraphDefinition &) const = default;
    };

    /** @brief Complete detached schema-4 candidate retaining pose-layer policies and adding typed state payloads. */
    struct AnimationGraphData final {
        std::uint32_t schemaVersion{CurrentAnimationGraphSchemaVersion};
        AnimationGraphContractVersion contractVersion{CurrentAnimationGraphContractVersion};
        AnimationGraphId id{};
        SkeletonId skeleton{};
        GraphDefinitionId entry{};
        std::vector<GraphParameter> parameters{};
        std::vector<GraphDefinition> definitions{};
        bool operator==(const AnimationGraphData &) const = default;
    };

    /** @brief Implementation safety ceilings independent of caller compile policy. */
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
    };

    /** @brief Captured synchronous admission state; no work survives the invocation. */
    struct AnimationGraphCompileContext final {
        AnimationGraphLimits limits{};
        CancellationToken cancellation{};
        bool accepting{true};
        std::optional<AnimationGraphId> replacing{};
        const SkeletonAsset *skeleton{};              /**< Invocation-scoped validated immutable dependency snapshot. */
        SkeletonAssetGeneration skeletonGeneration{}; /**< Generation issued by the caller's skeleton publication owner. */
        std::span<const AnimationClipAsset> clips{};  /**< Exact validated clip dependencies, pinned by the caller through compilation. */
    };

    /** @brief Stable source location for compiler/editor mapping, independent of input vector order. */
    struct GraphSourceLocation final {
        GraphDefinitionId definition{};
        GraphNodeId node{};
        GraphPinId pin{};
        auto operator<=>(const GraphSourceLocation &) const = default;
    };

    /** @brief Canonical instruction input; register indexes address an earlier instruction in this definition. */
    struct GraphCompiledInput final {
        GraphPinId destination{};
        std::uint32_t instruction{};
        GraphSourceLocation source{};
        GraphValueType type{};
        bool operator==(const GraphCompiledInput &) const = default;
    };

    /** @brief Typed immutable opcode and exact authoring source; pins are canonical by stable id. */
    struct GraphInstruction final {
        GraphSourceLocation source{};
        GraphNodePayload payload{};
        std::vector<GraphPin> pins{};
        std::vector<GraphCompiledInput> inputs{};
        std::optional<std::uint32_t> parameterIndex{};
        std::optional<std::uint32_t> definitionIndex{};
        std::vector<std::uint32_t> stateDefinitions{}; /**< Definition indexes for state call edges; clips are excluded. */
        bool operator==(const GraphInstruction &) const = default;
    };

    /** @brief Canonical subprogram; all data dependencies precede their consumer. */
    struct GraphCompiledDefinition final {
        GraphDefinitionId id{};
        std::vector<GraphInterfaceInput> inputs{};
        std::vector<GraphInstruction> instructions{};
        std::uint32_t outputInstruction{};
        std::uint32_t callDepth{};
        std::uint64_t evaluationInstructions{}; /**< Counts repeated subgraph calls, bounded to 65536. */
        std::uint64_t sourceMapCallEntries{};   /**< Sum of call-path and typed state-guard entries across expanded occurrences. */
        bool operator==(const GraphCompiledDefinition &) const = default;
    };

    /** @brief Type-specific preallocated working-storage slot; Pose slots contain a complete skeleton pose. */
    struct GraphWorkingSlot final {
        GraphValueType type{};
        std::uint32_t index{};
        auto operator<=>(const GraphWorkingSlot &) const = default;
    };

    /** @brief Exact operand access to its actual working-slot writer, including an Input node's interface alias.
     * destination is the local consumer pin; an interface alias uses its Result pin.
     */
    struct GraphOccurrenceInput final {
        GraphPinId destination{};
        GraphWorkingSlot slot{};
        std::uint32_t sourceOccurrence{};
        bool operator==(const GraphOccurrenceInput &) const = default;
    };

    /** @brief One compiler-resolved active-branch guard; lanes distinguish self-transition incarnations. */
    struct GraphStateGuard final {
        std::uint32_t machine{};
        std::uint32_t state{};
        std::uint8_t lane{};
        bool operator==(const GraphStateGuard &) const = default;
    };
    /** @brief State control precedes guarded children; completion writes the composed pose afterward. */
    enum class GraphOccurrencePhase : std::uint8_t {
        Evaluate,
        StateBegin,
        StateEnd
    };

    /** @brief Exact compiler-owned access plan; state control precedes guarded children, pose completion follows them.
     * Input, Output and Call alias their real writer; inline state clips use their exact clip binding/player access.
     */
    struct GraphInstructionOccurrence final {
        GraphSourceLocation source{};
        std::vector<GraphSourceLocation> callPath{}; /**< Root-to-leaf stable call nodes; repeated calls remain distinct. */
        std::uint32_t definitionIndex{};
        std::uint32_t instructionIndex{};
        std::vector<GraphOccurrenceInput> inputs{};
        GraphWorkingSlot output{};
        std::uint32_t outputProducer{}; /**< Exact Clip/Blend/Parameter writer; self for writes, earlier for aliases. */
        std::optional<std::uint32_t> clipPlayer{};
        std::optional<std::uint32_t> clipBindingIndex{};
        std::optional<std::uint32_t> parameterRead{};
        std::optional<std::uint32_t> stateMachine{};
        GraphOccurrencePhase phase{GraphOccurrencePhase::Evaluate};
        std::vector<GraphStateGuard> guards{};
        std::vector<float> jointMask{};           /**< Complete dense blend mask, bound to exact skeleton order. */
        std::vector<std::uint8_t> clipCoverage{}; /**< Complete dense authored-track coverage for a clip writer. */
        bool operator==(const GraphInstructionOccurrence &) const = default;
    };

    /** @brief Actual per-lane state execution storage; ranges reset only when that state incarnation enters. */
    struct GraphStateBranch final {
        GraphStateId id{};
        std::array<GraphWorkingSlot, 2> output{};
        std::array<std::uint32_t, 2> begin{};
        std::array<std::uint32_t, 2> end{};
        std::optional<AnimationDeltaTime> loopPeriod{};
        bool operator==(const GraphStateBranch &) const = default;
    };

    /** @brief Compiler-owned complete machine occurrence; no runtime pointer or generation is retained. */
    struct GraphStateMachineBinding final {
        std::uint32_t beginOccurrence{};
        std::uint32_t endOccurrence{};
        std::uint32_t conditionOffset{};
        std::array<GraphWorkingSlot, 4> poses{}; /**< Committed/candidate composed and committed/candidate frozen source buffers. */
        std::vector<GraphStateBranch> branches{};
        bool operator==(const GraphStateMachineBinding &) const = default;
    };

    /** @brief Exact immutable dependency compatibility captured without retaining caller-owned views. */
    struct GraphSkeletonBinding final {
        SkeletonId id{};
        SkeletonAssetGeneration generation{};
        SkeletonAssetContractVersion contractVersion{};
        std::uint32_t joints{};
        auto operator<=>(const GraphSkeletonBinding &) const = default;
    };

    /** @brief Complete finite allocation requirements for a future graph instance, not an evaluator. */
    struct GraphMemoryRequirements final {
        std::array<std::uint32_t, 5> workingSlots{}; /**< Indexed by known GraphValueType; every type has separate storage. */
        std::uint64_t poseTransforms{};
        std::uint32_t clipPlayers{};
        std::uint32_t parameters{};
        std::uint32_t callDepth{};
        std::uint32_t stateMachines{};
        std::uint32_t conditionLatches{};
        std::uint32_t stateEvents{};
        std::uint64_t compositionEntries{}; /**< Immutable dense mask/coverage elements; bounded by poseTransforms. */
        bool operator==(const GraphMemoryRequirements &) const = default;
    };

    /** @brief Owned validated program; runtime instances borrow immutable spans and own mutable state. */
    class AnimationGraphProgram final {
    public:
        /** @brief Compiles a detached candidate without publication or I/O.
         * @param data Current schema-4 candidate; migrate supported earlier source contracts explicitly first.
         * @param context Captured limits, cancellation, optional replacement identity and exact pinned dependencies.
         * @return Complete immutable program or stable graph failure; never partial publication.
         * @pre The caller pins the supplied skeleton/clip snapshots and generation for this synchronous call.
         * @post Success copies compatibility and retains no caller-owned pointer/span; publication must recheck snapshot authority.
         * @throws std::bad_alloc When compile-time storage cannot be allocated.
         */
        [[nodiscard]] static Result<AnimationGraphProgram> Compile(const AnimationGraphData &data,
                                                                   const AnimationGraphCompileContext &context = {});

        /** @brief Returns the stable asset identity. @return Persistent graph id. */
        [[nodiscard]] AnimationGraphId Id() const noexcept {
            return id_;
        }

        /** @brief Returns the required skeleton identity. @return Persistent skeleton id. */
        [[nodiscard]] SkeletonId Skeleton() const noexcept {
            return skeleton_;
        }

        /** @brief Returns canonical parameters. @return Immutable lifetime-bound span. */
        [[nodiscard]] std::span<const GraphParameter> Parameters() const noexcept {
            return parameters_;
        }

        /** @brief Returns canonical definitions. @return Immutable lifetime-bound span. */
        [[nodiscard]] std::span<const GraphCompiledDefinition> Definitions() const noexcept {
            return definitions_;
        }

        /** @brief Returns the entry definition index. @return Dense immutable program index. */
        [[nodiscard]] std::uint32_t EntryIndex() const noexcept {
            return entry_;
        }

        /** @brief Returns sorted unique clip dependencies. @return Immutable lifetime-bound span. */
        [[nodiscard]] std::span<const AnimationClipId> Clips() const noexcept {
            return clips_;
        }

        /** @brief Returns the complete entry occurrence/access plan. @return Immutable lifetime-bound span. */
        [[nodiscard]] std::span<const GraphInstructionOccurrence> Occurrences() const noexcept {
            return occurrences_;
        }

        /** @brief Returns admitted state-machine occurrences and branch ranges. @return Immutable program-owned bindings. */
        [[nodiscard]] std::span<const GraphStateMachineBinding> StateMachines() const noexcept {
            return stateMachines_;
        }

        /** @brief Returns the final pose access of the entry plan. @return Stable preallocated Pose slot. */
        [[nodiscard]] GraphWorkingSlot OutputSlot() const noexcept {
            return occurrences_.back().output;
        }

        /** @brief Returns exact finite instance storage requirements. @return Borrowed immutable counts. */
        [[nodiscard]] const GraphMemoryRequirements &Memory() const noexcept {
            return memory_;
        }

        /** @brief Returns the exact skeleton compatibility. @return Borrowed immutable identity, generation and layout. */
        [[nodiscard]] const GraphSkeletonBinding &SkeletonBinding() const noexcept {
            return skeletonBinding_;
        }

        /** @brief Returns exact clip compatibility sorted by identity. @return Immutable descriptor span. */
        [[nodiscard]] std::span<const AnimationClipDescriptor> ClipBindings() const noexcept {
            return clipBindings_;
        }

        bool operator==(const AnimationGraphProgram &) const = default;

    private:
        AnimationGraphProgram() = default;
        /** @brief Validates authoring topology without runtime dependency admission. */
        [[nodiscard]] static Result<AnimationGraphProgram> CompileSchema(const AnimationGraphData &data,
                                                                         const AnimationGraphCompileContext &context);
        friend Result<AnimationGraphData> MigrateAnimationGraph(const AnimationGraphData &, const AnimationGraphCompileContext &);
        std::vector<GraphInstructionOccurrence> occurrences_{};
        std::vector<GraphStateMachineBinding> stateMachines_{};
        GraphMemoryRequirements memory_{};
        GraphSkeletonBinding skeletonBinding_{};
        std::vector<AnimationClipDescriptor> clipBindings_{};
        AnimationGraphId id_{};
        SkeletonId skeleton_{};
        std::vector<GraphParameter> parameters_{};
        std::vector<GraphCompiledDefinition> definitions_{};
        std::vector<AnimationClipId> clips_{};
        std::uint32_t entry_{};
    };

    /** @brief Explicitly migrates supported source contracts and infers version-1 edge types, retaining stable IDs.
     * @param data Detached version-1/2/3/current candidate with its exact semantic version; input is never changed.
     * @param context Captured limits, admission and replacement identity.
     * @return Validated current candidate or stable failure; unknown versions, legacy pose policies and legacy state payloads reject.
     * @throws std::bad_alloc When detached migration storage cannot be allocated.
     */
    [[nodiscard]] Result<AnimationGraphData> MigrateAnimationGraph(const AnimationGraphData &data,
                                                                   const AnimationGraphCompileContext &context = {});
}  // namespace Horo::Animation
