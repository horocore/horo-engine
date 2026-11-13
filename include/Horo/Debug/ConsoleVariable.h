#pragma once

/** @file ConsoleVariable.h @brief Typed descriptor-owned session variables and explicit configuration-service bridges. */
#include "Horo/Debug/CommandRegistry.h"
#include "Horo/Foundation/Configuration.h"

#include <thread>

namespace Horo::Debug {
    /** @brief Exactly one activation boundary per variable; no implicit hot application. */
    enum class ConsoleVariableApplyPolicy : std::uint8_t {
        Immediate,
        NextFrame,
        NextSceneLoad,
        RestartRequired,
        ReadOnly
    };
    /** @brief Session state is ephemeral; durable values require the host's configuration persistence capability. */
    enum class ConsoleVariablePersistence : std::uint8_t {
        Session,
        User,
        Project
    };
    /** @brief Typed runtime value. Configuration-backed variables use only Foundation's supported subset. */
    using ConsoleValue = std::variant<bool, std::int64_t, double, std::string>;

    /** @brief Inert variable schema; contains no execution callback or global registration. */
    struct ConsoleVariableDescriptor final {
        std::uint64_t id{};
        std::string name;
        ConsoleValue defaultValue{false};
        std::optional<double> minimum;
        std::optional<double> maximum;
        std::size_t maximumStringBytes{1024};
        CommandPermission permission{CommandPermission::Developer};
        CommandAvailability availability{CommandAvailability::NonShipping};
        ConsoleVariablePersistence persistence{ConsoleVariablePersistence::Session};
        ConsoleVariableApplyPolicy applyPolicy{ConsoleVariableApplyPolicy::Immediate};
        std::optional<SettingKey> configurationKey; /**< Absent means descriptor-owned session state. */
    };

    /** @brief Configuration owner's persistence seam; the console never serializes a second configuration store. */
    class IConsoleVariablePersistence {
    public:
        virtual ~IConsoleVariablePersistence() = default;
        /** @brief Owns the complete durable configuration transaction, including effective/pending state.
         * @param key Existing registered key. @param value Schema-validated value. @param persistence Explicit durable source.
         * @param policy Declared activation boundary.
         * @param expectedRevision Exact canonical snapshot used for validation; stale requests must be rejected.
         * @return Original owning failure or successful admission; implementations enforce source/write authority and atomic publication.
         */
        [[nodiscard]] virtual Result<void> Store(const SettingKey &key, const SettingValue &value, ConsoleVariablePersistence persistence,
                                                 ConsoleVariableApplyPolicy policy, ConfigurationRevision expectedRevision) = 0;
        /** @brief Projects the configuration owner's pending request without making a console copy authoritative.
         * @param key Existing registered key. @return Owned pending value, if any.
         */
        [[nodiscard]] virtual std::optional<SettingValue> Pending(const SettingKey &key) const = 0;
        /** @brief Reaches the configuration owner's activation boundary.
         * @param key Existing registered key. @param boundary Exact reached lifecycle boundary.
         * @return Whether the owner activated the request, or its original typed failure.
         */
        [[nodiscard]] virtual Result<bool> Apply(const SettingKey &key, ConsoleVariableApplyPolicy boundary) = 0;
    };

    /** @brief Explicit effective/pending projection. Pending requests never replace the active configuration snapshot. */
    struct ConsoleVariableSnapshot final {
        ConsoleValue active;
        std::optional<ConsoleValue> pending;
        ConsoleVariableApplyPolicy applyPolicy{};
    };

    /** @brief One owner-thread variable instance; runtime-only values live here, configuration values do not.
     * Configuration-backed pending data is a typed write request awaiting its declared boundary.
     * Active reads always query ConfigurationService. The host supplies configuration persistence
     * rather than allowing the console to discover files or keep an untyped configuration mirror.
     */
    class ConsoleVariable final {
    public:
        ConsoleVariable(const ConsoleVariable &) = delete;
        ConsoleVariable &operator=(const ConsoleVariable &) = delete;
        ConsoleVariable(ConsoleVariable &&) noexcept = default;
        ConsoleVariable &operator=(ConsoleVariable &&) noexcept = default;
        /** @brief Validates a schema against its artifact and optional existing configuration owner.
         * @param descriptor Owned schema. @param policy Fixed artifact policy.
         * @param configuration Existing authority, required for configurationKey.
         * @param persistence Existing configuration workflow, required for durable variables.
         * @return Variable bound to the calling owner thread or typed composition failure.
         */
        [[nodiscard]] static Result<ConsoleVariable> Create(ConsoleVariableDescriptor descriptor, const RegistryPolicy &policy,
                                                            std::shared_ptr<ConfigurationService> configuration = {},
                                                            std::shared_ptr<IConsoleVariablePersistence> persistence = {});
        /** @brief Reads effective state through the declared authority.
         * @param context Host principal. @return Typed effective/pending projection or denial.
         */
        [[nodiscard]] Result<ConsoleVariableSnapshot> Read(const DebugCommandContext &context) const;
        /** @brief Validates before any owner write; deferred values remain explicitly pending.
         * @param value Already typed candidate. @param context Host principal.
         * @return Successful immediate application/deferred admission or original typed failure.
         */
        [[nodiscard]] Result<void> Set(ConsoleValue value, const DebugCommandContext &context);
        /** @brief Applies only the matching lifecycle boundary on the actual owner thread.
         * @param boundary Host-reached frame, scene-load or restart boundary; never inferred from time.
         * @return Whether a pending value became effective, or typed failure retaining the request.
         */
        [[nodiscard]] Result<bool> Apply(ConsoleVariableApplyPolicy boundary);
        /** @brief Returns owned immutable metadata. @return Borrowed schema. */
        [[nodiscard]] const ConsoleVariableDescriptor &Descriptor() const noexcept;

    private:
        ConsoleVariable(ConsoleVariableDescriptor descriptor, ProductProfile profile, std::shared_ptr<ConfigurationService> configuration,
                        std::shared_ptr<IConsoleVariablePersistence> persistence);
        [[nodiscard]] Result<void> Validate(const ConsoleValue &value) const;
        /** @brief Validates type, ranges and bounded strings without constructing runtime state or accessing services. */
        [[nodiscard]] static Result<void> ValidateValue(const ConsoleVariableDescriptor &descriptor, const ConsoleValue &value);
        /** @brief Commits a typed request against its captured revision through its sole authority. */
        [[nodiscard]] Result<void> Commit(const ConsoleValue &value, std::optional<ConfigurationRevision> expectedRevision);
        ConsoleVariableDescriptor descriptor_;
        ProductProfile profile_;
        std::shared_ptr<ConfigurationService> configuration_;
        std::shared_ptr<IConsoleVariablePersistence> persistence_;
        std::thread::id owner_;
        std::optional<ConsoleValue> sessionValue_;
        std::optional<ConsoleValue> pending_;
        std::optional<ConfigurationRevision> pendingRevision_;
    };
}  // namespace Horo::Debug
