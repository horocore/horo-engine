#pragma once

/** @file CommandRegistry.h
 * @brief Inert runtime console descriptors, policy-filtered discovery and bounded command preparation.
 */

#include "Horo/Foundation/ModuleDescriptor.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Horo::Debug {
    /** @brief Stable host-composed command identity; zero is invalid. */
    using DebugCommandId = std::uint64_t;
    /** @brief Separate packaged artifacts; never switched on a live registry. */
    enum class ProductProfile : std::uint8_t {
        Editor,
        Development,
        Profile,
        Shipping,
        Diagnostics,
        Server
    };
    /** @brief Orthogonal permission bits; Restricted always also requires an authenticated session. */
    enum class CommandPermission : std::uint32_t {
        Public = 1,
        Developer = 2,
        AdminCheat = 4,
        Restricted = 8
    };
    /** @brief Owner-selected execution boundary. */
    enum class CommandThreadPolicy : std::uint8_t {
        ImmediateConsoleThread,
        OwnerThreadNextFrame,
        RenderSafePoint,
        WorkerJob
    };
    /** @brief Artifact inclusion class, independent of permission grants. */
    enum class CommandAvailability : std::uint8_t {
        NonShipping,
        DevelopmentOnly,
        DiagnosticsOnly,
        ShippingAllowlist
    };
    /** @brief Typed argument representation; integer conversion is locale independent. */
    enum class DebugArgumentType : std::uint8_t {
        String,
        Int32,
        Int64,
        Float,
        Bool,
        Enum,
        EntityId
    };
    /** @brief Additional audit/privacy obligations for a descriptor. */
    enum class CommandFlags : std::uint32_t {
        None = 0,
        AuditLogged = 1,
        RedactArguments = 2
    };

    /** @brief Combines inert audit/privacy flags. @param left First flags. @param right Additional flags. @return Combined flags. */
    [[nodiscard]] constexpr CommandFlags operator|(CommandFlags left, CommandFlags right) noexcept {
        return static_cast<CommandFlags>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
    }

    /** @brief Allocation-free inert argument declaration. */
    struct DebugArgumentDescriptor final {
        std::string_view name;
        std::string_view description;
        DebugArgumentType type{DebugArgumentType::String};
        bool required{true};
        std::string_view defaultValue;
        bool sensitive{};
        bool option{}; /**< True accepts --name value, or name value after positionals; false consumes a positional token. */
        std::span<const std::string_view> enumeration;
        std::optional<double> minimum;
        std::optional<double> maximum;
    };

    /** @brief Allocation-free metadata. The host binds execution separately after atomic validation. */
    struct DebugCommandDescriptor final {
        DebugCommandId id{};
        std::string_view name;
        std::string_view summary;
        std::string_view syntax;
        std::span<const DebugArgumentDescriptor> arguments;
        CommandPermission permissions{CommandPermission::Developer};
        CommandThreadPolicy threadPolicy{CommandThreadPolicy::OwnerThreadNextFrame};
        CommandAvailability availability{CommandAvailability::DevelopmentOnly};
        CommandFlags flags{CommandFlags::None};
    };

    /** @brief Runtime principal copied with an invocation, never inferred from input text. */
    struct DebugCommandContext final {
        std::uint32_t permissionMask{1};
        bool hasServerAuthority{};
        bool authenticated{};
    };
    /** @brief Stable console failures, translated to the console error domain. */
    enum class CommandError : std::uint8_t {
        InvalidDescriptor,
        Duplicate,
        Usage,
        PermissionDenied,
        LimitExceeded,
        UnknownCommand,
        StartupDenied,
        WrongThread,
        ShuttingDown,
        MissingCapability,
        HandlerFailed,
        ReentrantDispatch
    };
    /** @brief Creates a typed failure without copying untrusted command arguments.
     * @param error Stable classification. @return Owned safe diagnostic.
     */
    [[nodiscard]] Error MakeCommandError(CommandError error);
    /** @brief Returns inert error metadata for explicit host ModuleHost registration.
     * @return Owned domain contribution with process-lifetime immutable descriptor pointers; starts no service.
     */
    [[nodiscard]] ModuleErrorDomainDescriptor ConsoleErrorDomain();
    /** @brief Validates canonical dotted ASCII identifiers without empty namespace segments.
     * @param text Candidate identity. @return Whether every segment starts with a lowercase letter or underscore.
     */
    [[nodiscard]] bool IsValidConsoleIdentifier(std::string_view text);

    /** @brief Bounds applied before copying input or metadata. */
    struct ConsoleLimits final {
        std::size_t commands{512};
        std::size_t arguments{32};
        std::size_t inputBytes{4096};
        std::size_t metadataBytes{4096};
        std::size_t historyEntries{500};
        std::size_t outputBytes{8192};
        std::size_t totalMetadataBytes{1U << 20U};
        std::size_t totalHistoryBytes{4U << 20U};
    };

    /** @brief Host-owned composition policy. Lists contain exact stable command IDs. */
    struct RegistryPolicy final {
        ProductProfile profile{ProductProfile::Development};
        std::vector<DebugCommandId> shippingAllowlist;
        std::vector<DebugCommandId> startupAllowlist;
        std::vector<std::uint64_t> shippingVariableAllowlist; /**< Separate exact variable IDs, validated by their registry. */
        std::vector<std::string> projectNamespaces;           /**< Additional one-segment namespaces approved by the host. */
        ConsoleLimits limits;
    };

    /** @brief Checks artifact inclusion for validated command or variable metadata.
     * @param id Stable descriptor ID. @param availability Compile/composition class. @param policy Fixed artifact policy.
     * @return Whether the descriptor belongs to this artifact's composed inventory.
     */
    [[nodiscard]] bool IsConsoleDescriptorIncluded(DebugCommandId id, CommandAvailability availability, const RegistryPolicy &policy);
    /** @brief Checks principal grants, authentication and authority for validated metadata.
     * @param permission Single declared permission. @param context Host principal. @param profile Fixed artifact identity.
     * @return True only when every independent grant and profile constraint is met.
     */
    [[nodiscard]] bool IsConsolePermissionGranted(CommandPermission permission, const DebugCommandContext &context, ProductProfile profile);

    /** @brief Complete entity-reference argument DTO; never inferred from the active scene at execution.
     * The scene adapter maps these fields into its existing EntityRef and validates that exact generation.
     * This transport value allocates no identities, owns no registry and grants no access to runtime storage.
     */
    struct DebugEntityReference final {
        std::uint64_t scene{};
        std::uint32_t index{};
        std::uint32_t generation{};
        [[nodiscard]] bool operator==(const DebugEntityReference &) const noexcept = default;
    };

    /** @brief Typed owned argument; handlers never reparse text. EntityId uses scene:index:generation. */
    using ArgumentValue = std::variant<std::string, std::int64_t, double, bool, DebugEntityReference>;

    /** @brief Validated owned request, safe to move into an execution queue. */
    struct CommandInvocation final {
        DebugCommandId id{};
        std::vector<ArgumentValue> arguments;
        std::string redactedText; /**< Safe history projection; sensitive raw input is never retained here. */
    };

    /** @brief Immutable registry owning deep copies of accepted metadata; no handler or service access. */
    class CommandRegistry final {
    public:
        /** @brief Atomically validates all descriptors, then excludes unavailable entries.
         * @param descriptors Explicit host contributions. @param policy Artifact policy and budgets.
         * @return Fully owned immutable registry or typed composition failure; invokes no callbacks.
         */
        [[nodiscard]] static Result<CommandRegistry> Create(std::span<const DebugCommandDescriptor> descriptors,
                                                            const RegistryPolicy &policy);
        ~CommandRegistry();
        CommandRegistry(CommandRegistry &&) noexcept;
        CommandRegistry &operator=(CommandRegistry &&) noexcept;
        CommandRegistry(const CommandRegistry &) = delete;
        CommandRegistry &operator=(const CommandRegistry &) = delete;
        /** @brief Parses, resolves, authorizes and converts the whole request before execution.
         * @param input Bounded command text. @param context Authenticated host principal.
         * @param startup Requires the additional startup allowlist when true.
         * @return Owned typed arguments or a safe usage/policy error with no handler side effects.
         */
        [[nodiscard]] Result<CommandInvocation> Prepare(std::string_view input, const DebugCommandContext &context,
                                                        bool startup = false) const;
        /** @brief Returns authorized descriptor matches in lexical order.
         * @param text Name/summary substring, or name prefix for completion. @param context Current principal.
         * @param prefix True for autocomplete. @return Registry-owned views; valid until registry destruction.
         */
        [[nodiscard]] std::vector<DebugCommandDescriptor> Discover(std::string_view text, const DebugCommandContext &context,
                                                                   bool prefix = false) const;
        /** @brief Resolves an authorized command ID for host dispatch.
         * @param id Exact ID. @param context Current principal. @return Owned metadata view or typed denial.
         */
        [[nodiscard]] Result<DebugCommandDescriptor> Resolve(DebugCommandId id, const DebugCommandContext &context) const;
        /** @brief Returns immutable admitted budgets. @return Registry-owned limits. */
        [[nodiscard]] const ConsoleLimits &Limits() const noexcept;
        /** @brief Returns fixed composition identity, absent only after move. @return Immutable artifact profile. */
        [[nodiscard]] std::optional<ProductProfile> Profile() const noexcept;

    private:
        struct State;
        explicit CommandRegistry(std::unique_ptr<State> state);
        std::unique_ptr<State> state_;
    };

    /** @brief Bounded redacted command/result record, independent of persistent logs. */
    struct HistoryEntry final {
        std::string command;
        std::string output;
        std::optional<OperationId> operation;         /**< Correlates an accepted asynchronous request; does not own its lifecycle. */
        OperationState state{OperationState::Queued}; /**< Last authoritative projection, meaningful only with operation. */
        bool operationUnavailable{};                  /**< The authority no longer retains this result; no terminal state is invented. */

        /** @brief Reports whether this retained row still needs an authoritative result. @return Pending projection state. */
        [[nodiscard]] bool HasPendingResult() const noexcept {
            return operation && !operationUnavailable && state != OperationState::Succeeded && state != OperationState::Failed &&
                   state != OperationState::Cancelled;
        }
    };

    /** @brief Adapter-owned history with bounded entries and bytes; owner-thread only. */
    class ConsoleHistory final {
    public:
        /** @brief Captures finite budgets. @param limits History and per-record byte limits. */
        explicit ConsoleHistory(ConsoleLimits limits = {});
        /** @brief Appends a prepared redacted command and safe bounded output.
         * @param invocation Successfully prepared request. @param output Safe handler output.
         * @param operation Optional accepted operation identity from the same process authority.
         */
        void Append(const CommandInvocation &invocation, std::string_view output, std::optional<OperationId> operation = {});
        /** @brief Projects retained asynchronous results without creating rows, evicting peers or exceeding history budgets.
         * @param snapshot Complete snapshot of the same authority used by the executor, with at most 4096 records/capacity.
         * @return Number of updated rows or typed invalid-source failure. Successful rows retain independent budgets.
         * Malformed source identity/capacity or visible UTF-8 is rejected before changing rows, counters or Revision.
         * A bounded allocation-free preflight simulates the same history-order byte budget used for publication.
         * Missing retained IDs become explicitly unavailable; already terminal/evicted/cleared rows are never resurrected.
         * Success text must be the executor's safe presentation value; failures expose only their stable error code.
         */
        [[nodiscard]] Result<std::size_t> ProjectOperations(const OperationStoreSnapshot &snapshot);
        /** @brief Counts retained rows awaiting results without scanning or allocating. @return Bounded pending row count. */
        [[nodiscard]] std::size_t PendingResults() const noexcept;
        /** @brief Applies a host-validated retained-entry preference at the history owner's lifecycle boundary.
         * @param limit New entry budget, from 1 through 10000; other byte/input/output budgets remain unchanged.
         * @return Number of oldest rows evicted, or typed invalid-limit failure without changing history.
         * Growth reserves bounded storage and can invalidate borrowed rows. Eviction also advances Revision.
         * Removed pending operations stay owned by their process store and cannot reappear through result projection.
         */
        [[nodiscard]] Result<std::size_t> SetEntryLimit(std::size_t limit);
        /** @brief Drops visible records without touching logging stores. */
        void Clear() noexcept;
        /** @brief Returns oldest-to-newest bounded records. @return Borrowed records. */
        [[nodiscard]] std::span<const HistoryEntry> Entries() const noexcept;
        /** @brief Identifies visible-history mutations for invalidating borrowed presentation selections. @return Owner-local revision. */
        [[nodiscard]] std::uint64_t Revision() const noexcept;
        /** @brief Searches newest-first without copying history.
         * @param text Safe query. @param skip Number of newest records to skip. @return Borrowed match or null.
         */
        [[nodiscard]] const HistoryEntry *ReverseSearch(std::string_view text, std::size_t skip = 0) const noexcept;

    private:
        ConsoleLimits limits_;
        std::vector<HistoryEntry> entries_;
        std::size_t retainedBytes_{};
        std::size_t pendingResults_{};
        std::uint64_t revision_{};
    };
}  // namespace Horo::Debug
