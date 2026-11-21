#pragma once

/** @file ConsoleVariableSet.h @brief Atomic bounded variable composition and policy-filtered typed access. */
#include "Horo/Debug/ConsoleVariable.h"

namespace Horo::Debug {
    /** @brief An immutable-name variable request with zero (read) or one (write) already converted value. */
    struct PreparedConsoleVariable final {
        std::string name;
        CommandInvocation invocation;
    };

    /** @brief Owns one immutable variable inventory and its descriptor-owned session state.
     * The host passes its complete command inventory to validate cross-kind ID/name collisions.
     * Writes and activation are owner-thread operations; discovery reads immutable metadata only.
     */
    class ConsoleVariableSet final {
    public:
        ConsoleVariableSet(const ConsoleVariableSet &) = delete;
        ConsoleVariableSet &operator=(const ConsoleVariableSet &) = delete;
        ConsoleVariableSet(ConsoleVariableSet &&) noexcept = default;
        ConsoleVariableSet &operator=(ConsoleVariableSet &&) noexcept = default;
        /** @brief Validates the complete batch before publishing a variable inventory.
         * @param descriptors Explicit contributions, including unavailable entries for collision validation.
         * @param commands Complete command contributions from this host composition.
         * @param policy Fixed profile, separate shipping variable allowlist and finite budgets.
         * @param configuration Existing configuration authority for declared key bridges.
         * @param persistence Existing durable configuration transaction owner.
         * @return Owned immutable inventory or typed failure; no setting is written during composition.
         */
        [[nodiscard]] static Result<ConsoleVariableSet> Create(std::span<const ConsoleVariableDescriptor> descriptors,
                                                               std::span<const DebugCommandDescriptor> commands,
                                                               const RegistryPolicy &policy,
                                                               std::shared_ptr<ConfigurationService> configuration = {},
                                                               std::shared_ptr<IConsoleVariablePersistence> persistence = {});
        /** @brief Reads one exact allowed variable.
         * @param name Canonical name. @param principal Current host principal. @return Typed effective/pending state or denial.
         */
        [[nodiscard]] Result<ConsoleVariableSnapshot> Read(std::string_view name, const DebugCommandContext &principal) const;
        /** @brief Validates and submits one typed variable change through its declared owner.
         * @param name Canonical name. @param value Typed candidate. @param principal Current host principal.
         * @return Immediate application/deferred admission or original typed failure.
         */
        [[nodiscard]] Result<void> Set(std::string_view name, ConsoleValue value, const DebugCommandContext &principal);
        /** @brief Parses name or name/value with the same lexical/type rules used for commands.
         * @param input Untrusted line. @param principal Current host principal. @param limits Common dispatcher budgets.
         * @return Owned typed request or policy/usage failure, without reading or changing variable state.
         * @note Safe on submitting threads: only immutable descriptor metadata is accessed. Startup scripts remain command-only.
         */
        [[nodiscard]] Result<PreparedConsoleVariable> Prepare(std::string_view input, const DebugCommandContext &principal,
                                                              const ConsoleLimits &limits) const;
        /** @brief Reaches one owner boundary; requests are independent, not an implicit multi-setting transaction.
         * @param boundary Explicit lifecycle point. @return Applied count or the first failure, retaining failed pending requests.
         */
        [[nodiscard]] Result<std::size_t> Apply(ConsoleVariableApplyPolicy boundary);
        /** @brief Discovers immutable allowed variable metadata without exposing denied values.
         * @param text Name substring, or prefix for autocomplete. @param principal Current host principal.
         * @param prefix Selects prefix matching. @return Owned finite metadata in lexical name order.
         */
        [[nodiscard]] std::vector<ConsoleVariableDescriptor> Discover(std::string_view text, const DebugCommandContext &principal,
                                                                      bool prefix = false) const;
        /** @brief Returns the fixed variable inventory profile. @return Immutable composition identity. */
        [[nodiscard]] ProductProfile Profile() const noexcept;
        /** @brief Returns the fixed state owner for host composition validation. @return Construction thread identity. */
        [[nodiscard]] std::thread::id OwnerThread() const noexcept;

    private:
        ConsoleVariableSet() = default;
        ProductProfile profile_{};
        std::vector<ConsoleVariable> variables_;
        std::thread::id owner_{std::this_thread::get_id()};
    };
}  // namespace Horo::Debug
