#pragma once
/** @file DecisionNodeRegistration.h
 * @brief Generated decision-node contributions bound to one revocable executable generation.
 */
#include "Horo/AI/AISceneComponents.h"
#include "Horo/AI/BehaviorTree.h"
#include "Horo/AI/BehaviorTreeRuntime.h"

#include <atomic>
#include <thread>

namespace Horo::Gameplay {
    class LuaDecisionNodeProgram;
}

namespace Horo::AI {
    enum class DecisionNodeRole : std::uint8_t {
        Task,
        Condition,
        Service,
        Count
    };
    enum class DecisionPortDirection : std::uint8_t {
        Input,
        Output,
        Count
    };

    /** @brief Stable typed port metadata, independent of editor display labels. */
    struct DecisionNodePort final {
        std::uint64_t id{};
        DecisionPortDirection direction{DecisionPortDirection::Input};
        BlackboardValueKind kind{BlackboardValueKind::Boolean};
        BlackboardValueCardinality cardinality{BlackboardValueCardinality::Scalar};
        bool required{};
    };

    /** @brief Complete inert contribution copied before generated/module memory can retire. */
    struct DecisionNodeContract final {
        DecisionNodeDescriptor descriptor;
        DecisionNodeRole role{DecisionNodeRole::Task};
        AiCapabilitySet capabilities;
        std::vector<DecisionNodePort> ports;
        std::vector<BehaviorTreeProperty> properties; /**< Typed defaults and stable property identities. */
        DecisionProviderMemoryContract memory;
    };

    /** @brief Exact-generation factory; all callbacks and userData remain protected by the acquired generation lease.
     * Create receives call-scoped properties and must copy any retained values before returning.
     */
    struct DecisionNodeFactoryBinding final {
        void *userData{};
        IBehaviorTreeExecutor *(*create)(void *, std::span<const BehaviorTreeProperty>){};
        void (*destroy)(void *, IBehaviorTreeExecutor *) noexcept {};
    };

    struct DecisionNodeRegistration final {
        DecisionNodeContract contract;
        DecisionNodeFactoryBinding factory;
    };

    /** @brief Authored properties supplied by the compiler, never a mutable document or provider object. */
    struct DecisionNodeConfiguration final {
        DecisionNodeId node;
        std::vector<BehaviorTreeProperty> properties;
    };

    /** @brief Validates one bounded declaration without invoking provider code. */
    [[nodiscard]] Result<void> ValidateDecisionNodeRegistration(const DecisionNodeRegistration &registration);

    /** @brief One host-admitted generation. Metadata survives retirement; creation requires live admission.
     * @details The host supplies its actual owner/code-image or VM lease and an alias to that owner's admission atomic.
     * The atomic alias must retain that same owner. Runtime executors retain this generation through provider destruction.
     * Native hosts already refuse reload/final retirement while these leases survive. Owner-lane destruction is mandatory.
     */
    class DecisionNodeGeneration final {
    public:
        static constexpr std::size_t MaximumNodes = 128;
        /** @brief Capture generated contributions in canonical type order with exact executable ownership.
         * An optional actual leaf provider extends lifetime/revocation for its declared script contributions.
         */
        [[nodiscard]] static Result<std::shared_ptr<const DecisionNodeGeneration>> Capture(
            std::span<const DecisionNodeRegistration> registrations, std::shared_ptr<const std::atomic_bool> admission,
            std::shared_ptr<const DecisionNodeGeneration> provider = {});
        /** @brief Combines admitted leaf generations, retaining every actual executable owner and its revocation.
         * Duplicate type contributions are ambiguous; package version selection remains the existing lock resolver's job.
         * Nested compositions are rejected, bounding revocation work to one level and at most MaximumNodes owners.
         */
        [[nodiscard]] static Result<std::shared_ptr<const DecisionNodeGeneration>> Compose(
            std::span<const std::shared_ptr<const DecisionNodeGeneration>> generations);

        /** @brief Owned metadata only; copying it grants no executable authority. */
        [[nodiscard]] std::span<const DecisionNodeContract> Contracts() const noexcept {
            return contracts_;
        }

        /** @brief Thread-neutral immutable capture fence; never grants permission to invoke callbacks. */
        [[nodiscard]] bool CanUse() const noexcept;
        /** @brief Pure implementation availability; never invokes a factory or probes a VM. */
        [[nodiscard]] bool HasImplementation(DecisionNodeTypeId type) const noexcept;
        /** @brief Logical core callback-owner storage; provider-private state is declared separately. */
        [[nodiscard]] static std::uint64_t ExecutorStorageBytes(std::size_t providers) noexcept;

        /** @brief True only on the acquiring owner thread while the actual generation admits new calls. */
        [[nodiscard]] bool CanAdmit() const noexcept;
        /** @brief Binds an admitted immutable plan to real factories, rejecting missing/ambiguous/version/capability mismatches. */
        [[nodiscard]] Result<std::unique_ptr<IBehaviorTreeExecutor>> CreateExecutor(
            std::shared_ptr<const DecisionNodeGeneration> self, const DecisionAssetPlan &plan, AiCapabilitySet capabilities,
            std::span<const DecisionNodeConfiguration> configuration = {}) const;
        /** @brief Binds only provider callbacks selected by actual executable topology; core control nodes remain flat runtime data. */
        [[nodiscard]] Result<std::unique_ptr<IBehaviorTreeExecutor>> CreateExecutor(
            std::shared_ptr<const DecisionNodeGeneration> self, const BehaviorTreeExecutionPlan &plan, AiCapabilitySet capabilities,
            std::span<const DecisionNodeConfiguration> configuration = {}) const;

    private:
        friend class RegisteredDecisionNodeExecutor;
        friend class ::Horo::Gameplay::LuaDecisionNodeProgram;
        [[nodiscard]] static Result<std::shared_ptr<const DecisionNodeGeneration>> CaptureVm(
            std::span<const DecisionNodeRegistration> registrations, std::shared_ptr<const std::atomic_bool> admission,
            std::shared_ptr<const DecisionNodeGeneration> provider);
        [[nodiscard]] static Result<std::shared_ptr<const DecisionNodeGeneration>> CaptureAdmitted(
            std::span<const DecisionNodeRegistration> registrations, std::shared_ptr<const std::atomic_bool> admission,
            std::shared_ptr<const DecisionNodeGeneration> provider, bool vmAuthority);
        [[nodiscard]] static bool ValidProvider(const std::shared_ptr<const DecisionNodeGeneration> &provider) noexcept;
        [[nodiscard]] Result<void> StoreDeclarations(std::vector<DecisionNodeRegistration> declarations);
        [[nodiscard]] Result<void> AppendGeneration(const std::shared_ptr<const DecisionNodeGeneration> &generation,
                                                    std::vector<DecisionNodeRegistration> &declarations);
        std::vector<DecisionNodeContract> contracts_;
        std::vector<DecisionNodeFactoryBinding> factories_;
        std::shared_ptr<const std::atomic_bool> admission_;
        std::shared_ptr<const DecisionNodeGeneration> provider_;
        std::vector<std::shared_ptr<const DecisionNodeGeneration>> parents_;
        std::thread::id owner_;
    };
}  // namespace Horo::AI
