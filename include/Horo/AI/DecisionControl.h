#pragma once
/** @file DecisionControl.h
 * @brief Typed intrinsic source operations and explicit cross-family calls; no synthetic provider identities.
 */
#include "Horo/AI/DecisionIdentity.h"

#include <cstddef>

namespace Horo::AI {
    /** @brief Authored semantics, separate from display names and provider type identity.
     * Unspecified preserves legacy inspectable source but is never compiler executable.
     * Intrinsics require no invented NodeTypeId; provider operations require the actual generated descriptor.
     */
    enum class DecisionControlKind : std::uint8_t {
        Unspecified,
        Root,
        Sequence,
        Selector,
        Parallel,
        Inverter,
        Cooldown,
        Loop,
        TimeLimit,
        Task,
        Condition,
        Service,
        Subplan,
        State,
        Transition,
        Count
    };

    /** @brief Stable call site source; resolution produces an index into one complete cooked dependency table. */
    struct DecisionSubplanCall final {
        DecisionGraphAssetId asset;
        DecisionPlanKind kind{DecisionPlanKind::BehaviorTree};
        DecisionVersionRange schema;
        [[nodiscard]] constexpr auto operator<=>(const DecisionSubplanCall &) const noexcept = default;
    };

    /** @brief Parallel completion policy; branches are stepped in declared order on the owner thread. */
    enum class BehaviorTreeParallelPolicy : std::uint8_t {
        RequireOneSuccess,
        RequireAllSuccess,
        RequireAllComplete,
        StopOthersOnFailure,
        Count,
    };
    /** @brief Reactive condition policy; lower-priority observers must be direct Selector children. */
    enum class BehaviorTreeAbortMode : std::uint8_t {
        None,
        Self,
        LowerPriority,
        Both,
        Count
    };
    /** @brief Service wake policy while its owning subtree is active. */
    enum class BehaviorTreeServiceMode : std::uint8_t {
        Periodic,
        Reactive,
        Count
    };

    /** @brief Authored finite control parameters. Provider/task identities remain in their existing contracts. */
    struct DecisionControlParameters final {
        DecisionControlKind kind{DecisionControlKind::Unspecified};
        BehaviorTreeParallelPolicy parallelPolicy{BehaviorTreeParallelPolicy::RequireAllSuccess};
        BehaviorTreeAbortMode abortMode{BehaviorTreeAbortMode::None};
        BehaviorTreeServiceMode serviceMode{BehaviorTreeServiceMode::Periodic};
        std::uint64_t durationTicks{};
        std::uint64_t intervalTicks{1};
        std::uint32_t iterations{1};
        [[nodiscard]] constexpr auto operator<=>(const DecisionControlParameters &) const noexcept = default;
    };

    /** @brief HSM terminal outcomes are authored explicitly, never inferred from graph shape. */
    enum class DecisionStateTerminal : std::uint8_t {
        None,
        Succeeded,
        Failed,
        Count
    };

    /** @brief Separates proven kernel storage from actual VM quotas and trusted native declarations. */
    enum class DecisionProviderMemoryKind : std::uint8_t {
        UnknownNative,
        NativeDeclared,
        EnforcedVm,
        Count
    };

    struct DecisionProviderMemoryContract final {
        DecisionProviderMemoryKind kind{DecisionProviderMemoryKind::UnknownNative};
        std::uint64_t stateBytes{}; /**< Logical provider-owned state declaration, not allocator/process RSS. */
        std::uint64_t scratchBytes{};
        std::uint64_t enforcedVmBytes{}; /**< Actual captured VM allocation quota only, zero for native declarations. */
    };

    /** @brief Compiler-only proof that every call was resolved against one admitted immutable closure. */
    class DecisionClosureAdmission final {
        DecisionClosureAdmission() = default;
        friend class DecisionGraphCompiler;
    };

    /** @brief Logical owned kernel storage, excluding allocator headers, caller leases and provider-private allocations. */
    struct DecisionKernelStorage final {
        std::uint64_t compiledBytes{};
        std::uint64_t instanceBytes{};
        std::uint64_t scratchBytes{};
        std::uint64_t taskSlots{};
        std::size_t maximumDepth{};
    };

    enum class DecisionMemoryPolicy : std::uint8_t {
        CoreBound,
        IncludeTrustedDeclarations,
        EnforcedTotal,
        Count
    };
}  // namespace Horo::AI
