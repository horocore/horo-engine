#pragma once

/** @file PerceptionSenseProvider.h
 * @brief Bounded custom spatial senses bound to an admitted executable generation.
 */

#include "Horo/AI/AITaskScheduler.h"
#include "Horo/AI/PerceptionDescriptorRegistry.h"
#include "Horo/AI/PerceptionMemory.h"
#include "Horo/Foundation/Diagnostics.h"

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace Horo::Gameplay {
    class LuaPerceptionSenseProgram;
}

namespace Horo::AI {
    /** @brief Captured candidate facts; providers receive no live Scene or blackboard capability. */
    struct PerceptionSenseCandidate final {
        PerceptionSourceRef source;
        Math::WorldCoordinate64 position;
    };

    /** @brief Call-scoped frozen facts from the authoritative spatial owner. */
    struct PerceptionSenseInput final {
        AgentHandle agent;
        PerceptionSourceRef listener;
        Math::WorldCoordinate64 position;
        std::uint64_t tick{};
        std::uint64_t spatialRevision{};
        std::span<const PerceptionSenseCandidate> candidates; /**< Strictly increasing source identities, captured by the spatial owner. */
        CancellationToken cancellation; /**< Replaced by the instance's admitted ancestry before invoking the provider. */
    };

    /** @brief Binary sensing outcome for an input candidate, never a provider-supplied entity or position. */
    struct PerceptionSenseSample final {
        std::uint32_t candidate{}; /**< Index into this invocation's candidate span. */
        bool sensed{true};         /**< False requests sensing loss; omission leaves existing memory untouched. */
    };

    /** @brief Fixed result storage owned by the caller, not the provider's image or VM. */
    struct PerceptionSenseResult final {
        static constexpr std::size_t MaximumCandidates = 128;
        std::array<PerceptionSenseSample, MaximumCandidates> samples{};
        std::size_t count{};
    };

    /** @brief Owner-only synchronous custom sense; retained inputs and private workers are prohibited. */
    class IPerceptionSenseProvider {
    public:
        virtual ~IPerceptionSenseProvider() = default;
        /** @brief Evaluates one bounded frozen candidate slice.
         * @param input Borrowed immutable facts, valid only for this invocation.
         * @param output Caller storage; return unique, ascending input-candidate indices.
         * @param budget Local declared allowance; consume before each bounded operation.
         * @return Written prefix length or the original typed provider failure.
         * @details No allocation, blocking I/O, private jobs or input retention. No blackboard mutation authority is supplied.
         */
        [[nodiscard]] virtual Result<std::size_t> Evaluate(const PerceptionSenseInput &input, std::span<PerceptionSenseSample> output,
                                                           AiWorkBudget &budget) = 0;
    };

    /** @brief Inert custom spatial-sense metadata; built-in Gameplay event identities cannot be overridden. */
    struct PerceptionSenseContract final {
        SenseTypeDescriptor sense;
        StimulusTypeDescriptor stimulus; /**< Version one binary spatial-observation payload. */
        std::size_t instanceBytes{};     /**< Declared private instance storage, charged at activation. */
        std::size_t workUnits{};         /**< Provider cost; the runner additionally charges ValidationWorkUnits. */
        std::size_t maximumCandidates{PerceptionSenseResult::MaximumCandidates}; /**< Per-provider input/output bound. */
        SourceLocation source; /**< Owned authoring/diagnostic location; never executable authority. */
    };

    /** @brief Module-owned factory pair; the generation pins userData and callback code through destruction. */
    struct PerceptionSenseFactory final {
        void *userData{};
        IPerceptionSenseProvider *(*create)(void *){};
        void (*destroy)(void *, IPerceptionSenseProvider *) noexcept {};
    };

    /** @brief One inert generated contribution; capturing it never invokes its factory. */
    struct PerceptionSenseRegistration final {
        PerceptionSenseContract contract;
        PerceptionSenseFactory factory;
    };

    /** @brief Validates one native contribution without invoking provider code.
     * @param registration Complete bounded metadata and factory pair. @return Success or typed descriptor/capacity failure.
     */
    [[nodiscard]] Result<void> ValidatePerceptionSenseRegistration(const PerceptionSenseRegistration &registration);

    /** @brief Exact owner binding for one agent's custom sense instance. */
    struct PerceptionSenseBinding final {
        AgentHandle agent;
        PerceptionSourceRef listener;
        PerceptionListenerTypeId listenerType;
        CancellationToken cancellation;
    };

    class PerceptionSenseInstance;

    /** @brief Host-admitted native generation, retaining its actual module owner and revocation state.
     * @details Capture metadata at composition. Creation, evaluation and final release belong to the acquiring owner thread.
     * The admission alias must retain the same executable owner that owns every factory and userData value.
     */
    class PerceptionSenseGeneration final {
    public:
        static constexpr std::size_t MaximumSenses = 64;
        static constexpr std::size_t MaximumInstanceBytes = 1U << 20;
        static constexpr std::size_t MaximumWorkUnits = 4096;
        static constexpr std::size_t MaximumSourceBytes = 4096;
        static constexpr std::size_t ValidationWorkUnits = 2; /**< Input/output validation, each bounded by MaximumCandidates. */

        /** @brief Copies native metadata and exact executable ownership without creating instances.
         * @param registrations Borrowed complete native contributions. @param admission Alias to the actual owner's live admission.
         * @return Immutable generation or typed descriptor/storage failure. No factory runs during capture.
         */
        [[nodiscard]] static Result<std::shared_ptr<const PerceptionSenseGeneration>> Capture(
            std::span<const PerceptionSenseRegistration> registrations, std::shared_ptr<const std::atomic_bool> admission);
        /** @brief Reads stable sense-ordered inert contracts. @return Generation-owned metadata, including after revocation. */
        [[nodiscard]] std::span<const PerceptionSenseContract> Contracts() const noexcept;
        /** @brief Checks executable admission on the acquiring owner thread. @return False after owner retirement. */
        [[nodiscard]] bool CanAdmit() const noexcept;
        /** @brief Creates one instance after exact descriptor, listener, capability and owner admission.
         * @param self Exact shared generation, retained through factory destruction. @param sense Selected custom sense.
         * @param registry Frozen metadata used by this scene. @param capabilities Explicit host grants.
         * @param binding Exact agent/listener lifetime. @return Owned instance or typed admission/provider failure.
         */
        [[nodiscard]] Result<std::unique_ptr<PerceptionSenseInstance>> Create(std::shared_ptr<const PerceptionSenseGeneration> self,
                                                                              SenseTypeId sense,
                                                                              const PerceptionDescriptorRegistry &registry,
                                                                              PerceptionCapabilitySet capabilities,
                                                                              PerceptionSenseBinding binding) const;

    private:
        friend class PerceptionSenseInstance;
        friend class ::Horo::Gameplay::LuaPerceptionSenseProgram;
        /** @brief Admits only the existing enforced Lua adapter under an actual native parent's revocation. */
        [[nodiscard]] static Result<std::shared_ptr<const PerceptionSenseGeneration>> CaptureVm(
            std::span<const PerceptionSenseRegistration> registrations, std::shared_ptr<const std::atomic_bool> admission,
            std::shared_ptr<const PerceptionSenseGeneration> parent);
        /** @brief Shares bounded capture mechanics without exposing Script authority to native metadata. */
        [[nodiscard]] static Result<std::shared_ptr<const PerceptionSenseGeneration>> CaptureAdmitted(
            std::span<const PerceptionSenseRegistration> registrations, std::shared_ptr<const std::atomic_bool> admission,
            std::shared_ptr<const PerceptionSenseGeneration> parent);
        PerceptionSenseGeneration() = default;
        std::vector<PerceptionSenseContract> contracts_;
        std::vector<PerceptionSenseFactory> factories_;
        std::shared_ptr<const std::atomic_bool> admission_;
        std::shared_ptr<const PerceptionSenseGeneration> parent_;
        std::thread::id owner_;
    };

    /** @brief One owner-bound provider; module code remains pinned until the module-owned destroy callback returns. */
    class PerceptionSenseInstance final {
    public:
        /** @brief Destroys provider storage on its owner before releasing the executable lease. */
        ~PerceptionSenseInstance();
        PerceptionSenseInstance(const PerceptionSenseInstance &) = delete;
        PerceptionSenseInstance &operator=(const PerceptionSenseInstance &) = delete;
        /** @brief Executes once per increasing simulation tick without live Scene or blackboard access.
         * @param input Exact owner and frozen candidate slice. @param agentBudget Agent allowance.
         * @param globalBudget Global allowance; an aliased budget is charged once.
         * @return Validated owned samples or a sense-local typed failure with no partial samples.
         * @details The spatial owner revalidates its publication and every entity before applying samples at PerceptionSensePoll.
         * Module revocation and cancellation are checked before and after callbacks. Insufficient budgets invoke no provider.
         * Exceptions, invalid output and exceeded local bounds retire this instance; original typed errors remain retryable by its owner.
         * If exception diagnostic storage is unavailable, the provider-failure code is retained with an empty message.
         */
        [[nodiscard]] Result<PerceptionSenseResult> Evaluate(const PerceptionSenseInput &input, AiWorkBudget &agentBudget,
                                                             AiWorkBudget &globalBudget);

        /** @brief Checks owner-thread, executable/cancellation admission and sense-local state before gathering candidates.
         * @return False after retirement, quarantine, cancellation, off-owner or during a provider callback.
         * @details Callers may use this before borrowing mutable Scene-owner state. Evaluation rechecks admission itself.
         */
        [[nodiscard]] bool CanEvaluate() const noexcept;

        /** @brief Reads the captured exact agent/listener lifetime. @return Borrowed immutable binding while this instance lives. */
        [[nodiscard]] const PerceptionSenseBinding &Binding() const noexcept;
        /** @brief Reads captured policy and identities. @return Borrowed immutable contract while this instance lives. */
        [[nodiscard]] const PerceptionSenseContract &Contract() const noexcept;

    private:
        friend class PerceptionSenseGeneration;
        /** @brief Captures exact provider ownership before its factory is invoked. */
        PerceptionSenseInstance(std::shared_ptr<const PerceptionSenseGeneration> generation, std::size_t index,
                                PerceptionSenseBinding binding) noexcept;
        /** @brief Invokes the factory under its image pin and rechecks retirement before exposing the instance. */
        [[nodiscard]] Result<void> Initialize();
        /** @brief Validates owner, binding and monotonic input before spending work allowances. */
        [[nodiscard]] Result<void> AdmitInput(const PerceptionSenseInput &input) const;
        /** @brief Isolates provider exceptions and local-budget overruns without bypassing the caller's lifetime recheck. */
        [[nodiscard]] Result<std::size_t> RunProvider(const PerceptionSenseInput &input, std::span<PerceptionSenseSample> output,
                                                      AiWorkBudget &budget);
        std::shared_ptr<const PerceptionSenseGeneration> generation_;
        std::size_t index_{};
        PerceptionSenseBinding binding_;
        IPerceptionSenseProvider *provider_{};
        std::optional<std::uint64_t> lastTick_;
        std::uint64_t lastSpatialRevision_{};
        bool failed_{};
        bool evaluating_{};
    };
}  // namespace Horo::AI
