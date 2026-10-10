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
    inline constexpr std::uint32_t CurrentAnimationGraphSchemaVersion = 2;

    /** @brief Exact graph semantic contract, independent of source-envelope migration. */
    struct AnimationGraphContractVersion final {
        std::uint16_t major{};
        std::uint16_t minor{};
        std::uint16_t patch{};
        auto operator<=>(const AnimationGraphContractVersion &) const = default;
    };

    /** @brief Graph semantic contract implemented by this compiler. */
    inline constexpr AnimationGraphContractVersion CurrentAnimationGraphContractVersion{1, 0, 0};

    struct GraphDefinitionTag;
    struct GraphNodeTag;
    struct GraphPinTag;
    struct GraphParameterTag;
    struct GraphInterfaceTag;
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
        bool operator==(const GraphClipNode &) const = default;
    };

    /** @brief Two-pose blend opcode without runtime blending policy. */
    struct GraphBlendNode final {
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

    /** @brief Supported node semantics; future nodes require a versioned compiler contract. */
    using GraphNodePayload =
        std::variant<GraphClipNode, GraphBlendNode, GraphParameterNode, GraphOutputNode, GraphInputNode, GraphCallNode>;

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

    /** @brief Complete detached candidate; version 2 requires explicit connection types. */
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
        std::uint64_t sourceMapCallEntries{};   /**< Sum of call-path lengths across expanded instruction occurrences. */
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

    /** @brief Completion-ordered access plan; only Clip, Blend and Parameter instructions write working storage.
     * Input, Output and Call occurrences alias an existing writer. Call completion follows its entire callee.
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
        bool operator==(const GraphInstructionOccurrence &) const = default;
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
        bool operator==(const GraphMemoryRequirements &) const = default;
    };

    /** @brief Owned validated program; runtime instances borrow immutable spans and own mutable state. */
    class AnimationGraphProgram final {
    public:
        /** @brief Compiles a detached candidate without publication or I/O.
         * @param data Version-2 candidate; migrate version 1 explicitly first.
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

    /** @brief Migrates version-1 untyped edges by their validated endpoint types, retaining stable IDs.
     * @param data Detached version-1 or version-2 candidate; input is never changed.
     * @param context Captured limits, admission and replacement identity.
     * @return Validated version-2 candidate or stable failure; unknown versions are rejected.
     * @throws std::bad_alloc When detached migration storage cannot be allocated.
     */
    [[nodiscard]] Result<AnimationGraphData> MigrateAnimationGraph(const AnimationGraphData &data,
                                                                   const AnimationGraphCompileContext &context = {});
}  // namespace Horo::Animation
