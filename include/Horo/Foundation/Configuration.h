#pragma once

/**
 * @file Configuration.h
 * @brief Typed configuration schema, deterministic source resolution, and immutable snapshots.
 */

#include "Horo/Foundation/CredentialReference.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/Result.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace Horo {
    class ProcessService;

    /** @brief Stable dotted configuration key identity. */
    class SettingKey {
    public:
        explicit SettingKey(std::string value) : m_value(std::move(value)) {}

        [[nodiscard]] const std::string &Value() const noexcept {
            return m_value;
        }

        [[nodiscard]] bool operator==(const SettingKey &) const noexcept = default;

    private:
        std::string m_value;
    };

    /** @brief Transparent hash for configuration setting identities. */
    struct SettingKeyHash {
        std::size_t operator()(const SettingKey &key) const noexcept {
            return std::hash<std::string>{}(key.Value());
        }
    };

    enum class SettingValueType : std::uint8_t {
        Boolean,
        Integer,
        String,
        CredentialReference
    };
    enum class SettingScope : std::uint8_t {
        Engine,
        User,
        Project,
        Workspace,
        Session,
        Invocation
    };
    enum class ReloadPolicy : std::uint8_t {
        Immediate,
        NextFrame,
        NextOperation,
        ProjectReopen,
        ProcessRestart
    };
    /** @brief Host-owned point at which a staged reload may become visible. */
    enum class ConfigurationReloadPoint : std::uint8_t {
        Immediate,
        NextFrame,
        NextOperation,
        NextFrameAndOperation,
        ProjectReopen,
        ProcessRestart
    };
    enum class SettingSensitivity : std::uint8_t {
        Public,
        SecretReference
    };
    using SettingValue = std::variant<bool, std::int64_t, std::string, CredentialReference>;
    using ConfigurationRevision = std::uint64_t;
    enum class ConfigurationDomain : std::uint8_t {
        Engine,
        User,
        Project,
        Workspace,
        Session,
        Invocation,
        All
    };

    /** @brief Canonical configuration source ordered from highest to lowest precedence. */
    enum class ConfigurationSource : std::uint8_t {
        Invocation,
        Environment,
        Session,
        Project,
        User,
        PackagedProfile,
        SchemaDefault
    };

    /** @brief Bit mask used by a setting descriptor to admit resolution sources. */
    enum class ConfigurationSourceMask : std::uint16_t {
        None = 0,
        Invocation = 1U << 0U,
        Environment = 1U << 1U,
        Session = 1U << 2U,
        Project = 1U << 3U,
        User = 1U << 4U,
        PackagedProfile = 1U << 5U
    };

    [[nodiscard]] constexpr ConfigurationSourceMask operator|(const ConfigurationSourceMask left,
                                                              const ConfigurationSourceMask right) noexcept {
        return static_cast<ConfigurationSourceMask>(static_cast<std::uint16_t>(left) | static_cast<std::uint16_t>(right));
    }

    /** @brief Per-setting admission policy for external configuration sources. */
    struct ConfigurationSourcePolicy {
        ConfigurationSourceMask allowedSources = ConfigurationSourceMask::None;
        bool sessionOverridesEnvironment = false;
    };

    /** @brief Resource limits applied before untrusted configuration input is retained. */
    struct ConfigurationLimits {
        std::size_t maximumKeysPerSource = 1024;
        std::size_t maximumKeyBytes = 256;
        std::size_t maximumStringValueBytes = 64 * 1024;
        std::size_t maximumSourceLocationBytes = 1024;
        std::size_t maximumDocumentBytes = 4 * 1024 * 1024;
        std::size_t maximumEnvironmentBindings = 256;
        std::size_t maximumNestingDepth = 16;
    };

    /** @brief Bounded committed-change notification; consumers re-query the authoritative snapshot. */
    struct ConfigurationChangedEvent {
        static constexpr std::string_view HoroEventTypeName = "horo.configuration.changed.v1";
        ConfigurationRevision revision = 0;
        ConfigurationDomain domain = ConfigurationDomain::All;
        std::vector<SettingKey> changedKeys;
    };

    /** @brief Inclusive descriptor-owned integer validation bounds. */
    struct SettingIntegerRange {
        std::int64_t minimum;
        std::int64_t maximum;
    };

    /** @brief Bounded declarative string rules shared by every configuration ingress. */
    struct SettingStringRules {
        std::size_t minimumBytes = 0;
        std::size_t maximumBytes = 64 * 1024;
        std::string requiredPrefix;
        std::string allowedCharacters;    /**< Empty admits all valid UTF-8 scalar sequences. */
        std::vector<std::string> choices; /**< Empty admits all values satisfying the other rules. */
    };

    using SettingValidation = std::variant<std::monostate, SettingIntegerRange, SettingStringRules>;

    /** @brief Inert owner-declared localization identity, interpreted by each host's presentation adapter. */
    struct SettingLabelKey {
        std::string namespaceId;
        std::string key;
    };

    /** @brief Schema descriptor registered by a composition root before resolution begins. */
    struct SettingDescriptor {
        SettingKey key;
        SettingValueType type;
        SettingValue defaultValue;
        SettingScope scope;
        ReloadPolicy reloadPolicy;
        SettingSensitivity sensitivity;
        std::optional<ConfigurationSourcePolicy> sourcePolicy;
        bool readOnly = false; /**< Rejects draft edits while retaining explicit source resolution. */
        SettingValidation validation;
        std::optional<SettingLabelKey> label;
    };

    /** @brief One typed source candidate with safe provenance metadata. */
    struct ConfigurationInputValue {
        SettingValue value;
        std::optional<SourceLocation> location;
    };

    using ConfigurationSourceMap = std::unordered_map<SettingKey, ConfigurationInputValue, SettingKeyHash>;

    /** @brief Complete bounded input set for one deterministic resolution pass. */
    struct ConfigurationResolutionRequest {
        ConfigurationSourceMap invocation;
        ConfigurationSourceMap environment;
        ConfigurationSourceMap session;
        ConfigurationSourceMap project;
        ConfigurationSourceMap user;
        ConfigurationSourceMap packagedProfile;
    };

    /** @brief Explicit host-provided environment-variable to setting-key mapping. */
    struct EnvironmentVariableBinding {
        SettingKey key;
        std::string variable;
    };

    /** @brief Validated supplied value retained to explain precedence without repeating source reads. */
    class ConfigurationCandidate final {
    public:
        /** @brief Owns a source view without copying its captured value. @param source Canonical precedence source.
         * @param input Non-null immutable input owner, including aliasing owners of a captured source set. */
        ConfigurationCandidate(ConfigurationSource source, std::shared_ptr<const ConfigurationInputValue> input)
            : source_(source), input_(std::move(input)) {
            HORO_INVARIANT_MSG(input_ != nullptr, "A configuration candidate requires an immutable input owner.");
        }

        /** @brief Returns source precedence metadata. @return Canonical source. */
        [[nodiscard]] ConfigurationSource Source() const noexcept {
            return source_;
        }

        /** @brief Borrows the captured value for this owning view's lifetime. @return Immutable source value. */
        [[nodiscard]] const SettingValue &Value() const noexcept {
            return input_->value;
        }

        /** @brief Borrows the captured safe provenance. @return Immutable optional source location. */
        [[nodiscard]] const std::optional<SourceLocation> &Location() const noexcept {
            return input_->location;
        }

    private:
        ConfigurationSource source_;
        std::shared_ptr<const ConfigurationInputValue> input_;
    };

    /** @brief Winning typed value and its safe source provenance. */
    struct ResolvedSetting {
        SettingValue value;
        ConfigurationSource source = ConfigurationSource::SchemaDefault;
        std::optional<SourceLocation> location;
        SettingSensitivity sensitivity = SettingSensitivity::Public;
        std::vector<ConfigurationCandidate> candidates; /**< Precedence-ordered supplied values, including the schema default. */
    };

    /** @brief Draft transaction formed against one immutable configuration revision. */
    struct ConfigurationDraft {
        ConfigurationRevision baseRevision = 0;
        std::unordered_map<SettingKey, SettingValue, SettingKeyHash> proposedValues;
        ConfigurationSource source = ConfigurationSource::Session;
    };

    /** @brief Immutable, cheap-to-copy reference to one resolved configuration revision. */
    class ConfigurationSnapshot {
    public:
        /** @brief Returns this snapshot's immutable revision. */
        [[nodiscard]] ConfigurationRevision Revision() const noexcept;
        /** @brief Returns a registered value. @param key Registered key. @return Immutable value reference. */
        [[nodiscard]] const SettingValue &Get(const SettingKey &key) const;
        /** @brief Finds a value. @param key Candidate key. @return Copied value when present. */
        [[nodiscard]] std::optional<SettingValue> Find(const SettingKey &key) const;
        /** @brief Finds the resolved value and provenance. @param key Candidate key. @return Immutable resolved entry or null. */
        [[nodiscard]] const ResolvedSetting *FindResolved(const SettingKey &key) const noexcept;
        /** @brief Serializes a deterministic versioned document without secret material. @return Canonical JSON text. */
        [[nodiscard]] std::string ToJson() const;
        /** @brief Serializes only captured values from one source for host persistence. @param source Exact authored source.
         * @return Deterministic versioned document, retaining opaque references and excluding unset defaults. */
        [[nodiscard]] std::string SourceToJson(ConfigurationSource source) const;

    private:
        struct Data {
            ConfigurationRevision revision = 0;
            std::unordered_map<SettingKey, ResolvedSetting, SettingKeyHash> values;
            std::shared_ptr<const ConfigurationResolutionRequest> inputs;
        };
        friend class ConfigurationResolver;
        friend class ConfigurationService;

        explicit ConfigurationSnapshot(std::shared_ptr<const Data> data) : m_data(std::move(data)) {}

        std::shared_ptr<const Data> m_data;
    };

    using ConfigurationSnapshotRef = ConfigurationSnapshot;

    /** @brief Mutable schema builder sealed before the configuration service is constructed. */
    class ConfigurationSchema {
    public:
        /** @brief Registers one inert descriptor. @param descriptor Descriptor to own. @return Success or a typed schema error. */
        [[nodiscard]] Result<void> Register(const SettingDescriptor &descriptor);
        /** @brief Prevents subsequent registration. @return Success or a typed already-sealed error. */
        [[nodiscard]] Result<void> Seal();
        /** @brief Finds an immutable descriptor. @param key Candidate key. @return Descriptor or null. */
        [[nodiscard]] const SettingDescriptor *FindDescriptor(const SettingKey &key) const noexcept;
        /** @brief Reports whether registration is complete. */
        [[nodiscard]] bool IsSealed() const noexcept;
        /** @brief Copies descriptors in stable key order for an editing session. @return Owned immutable metadata copies. */
        [[nodiscard]] std::vector<SettingDescriptor> Descriptors() const;
        /** @brief Tests a value against a primitive schema type. */
        [[nodiscard]] static bool MatchesType(SettingValueType type, const SettingValue &value);
        /** @brief Tests descriptor source admission using the canonical compatibility mapping. @param descriptor Sealed metadata.
         * @param source Candidate source. @return True when explicitly or compatibly admitted. */
        [[nodiscard]] static bool AllowsSource(const SettingDescriptor &descriptor, ConfigurationSource source) noexcept;

    private:
        friend class ConfigurationResolver;
        friend class ConfigurationService;
        [[nodiscard]] static Error ErrorFor(const ErrorCodeDescriptor &descriptor);
        bool m_sealed = false;
        std::unordered_map<SettingKey, SettingDescriptor, SettingKeyHash> m_descriptors;
    };

    /** @brief Stateless canonical resolver shared by every host composition. */
    class ConfigurationResolver {
    public:
        /**
         * @brief Resolves all declared sources into one immutable snapshot.
         * @param schema Sealed schema that defines every legal key and source policy.
         * @param request Complete captured source inputs; no ambient reads occur.
         * @param revision Revision assigned to the produced snapshot.
         * @param limits Untrusted-input limits.
         * @return Complete immutable snapshot or one ordered diagnostic failure.
         */
        [[nodiscard]] static Result<ConfigurationSnapshot> Resolve(const ConfigurationSchema &schema,
                                                                   const ConfigurationResolutionRequest &request,
                                                                   ConfigurationRevision revision = 0,
                                                                   const ConfigurationLimits &limits = {});

        /**
         * @brief Captures explicitly bound environment values through the host process seam.
         * @param schema Sealed schema used for type and sensitivity checks.
         * @param bindings Explicit non-colliding HORO_ variable bindings.
         * @param processes Host-owned environment provider.
         * @param limits Untrusted-input limits.
         * @return Typed environment source map or a safe diagnostic failure.
         */
        [[nodiscard]] static Result<ConfigurationSourceMap> CaptureEnvironment(const ConfigurationSchema &schema,
                                                                               std::span<const EnvironmentVariableBinding> bindings,
                                                                               const ProcessService &processes,
                                                                               const ConfigurationLimits &limits = {});

        /**
         * @brief Parses one versioned configuration document into a typed source map.
         * @param schema Sealed schema used for typed values.
         * @param document Complete UTF-8 JSON document.
         * @param sourceName Safe logical path/name recorded in provenance.
         * @param limits Untrusted-input limits.
         * @return Typed source map or a bounded parse/validation failure.
         */
        [[nodiscard]] static Result<ConfigurationSourceMap> ParseDocument(const ConfigurationSchema &schema, std::string_view document,
                                                                          std::string sourceName, const ConfigurationLimits &limits = {});
    };

    /** @brief Composition-root-owned authority that atomically replaces validated configuration snapshots. */
    class ConfigurationService {
    public:
        /** @brief Owner-thread prepared write retaining exclusive writer authority until commit or destruction.
         * @details Snapshot reads remain available. The service must outlive the transaction. A transaction must be
         * committed or destroyed on its creating thread before another write is attempted on that thread.
         */
        class PreparedCommit final {
        public:
            PreparedCommit(PreparedCommit &&) noexcept = default;
            PreparedCommit &operator=(PreparedCommit &&) noexcept = default;
            PreparedCommit(const PreparedCommit &) = delete;
            PreparedCommit &operator=(const PreparedCommit &) = delete;
            /** @brief Returns the completely validated candidate for host persistence. @return Captured immutable candidate. */
            [[nodiscard]] ConfigurationSnapshot Candidate() const;
            /** @brief Publishes the prepared state and releases writer authority before notifications. @return Success or invalid
             * transaction. */
            [[nodiscard]] Result<void> Commit();

        private:
            friend class ConfigurationService;
            PreparedCommit(ConfigurationService &owner, std::unique_lock<std::mutex> lock, ConfigurationSnapshot candidate,
                           ConfigurationChangedEvent event);
            ConfigurationService *owner_;
            std::unique_lock<std::mutex> lock_;
            ConfigurationSnapshot candidate_;
            ConfigurationChangedEvent event_;
        };

        explicit ConfigurationService(ConfigurationSchema schema, EngineDataBus *events = nullptr);
        /** @brief Constructs the authority from explicitly captured initial sources without a change notification.
         * @param schema Valid sealed schema. @param events Borrowed optional process bus. @param initial Valid captured source inputs.
         * @pre The composition root has validated the initial source request. */
        ConfigurationService(ConfigurationSchema schema, EngineDataBus *events, const ConfigurationResolutionRequest &initial);
        [[nodiscard]] ConfigurationSnapshot Snapshot() const;
        /** @brief Returns the service-owned sealed schema. @return Borrowed metadata valid for the service lifetime. */
        [[nodiscard]] const ConfigurationSchema &Schema() const noexcept;
        /**
         * @brief Builds an immutable preview without changing active configuration or publishing events.
         * @param draft Proposed source values against the active revision.
         * @return Complete preview snapshot or typed validation/stale failure.
         */
        [[nodiscard]] Result<ConfigurationSnapshot> Preview(const ConfigurationDraft &draft) const;
        /**
         * @brief Validates and reserves one write so a host can durably prepare its domain without a stale publication race.
         * @param draft Proposed source transaction and captured base revision.
         * @return Owner-thread prepared commit. Destruction cancels without changing configuration or publishing events.
         * @note All configuration writers share this gate. Persistence runs outside the snapshot/registry mutex.
         */
        [[nodiscard]] Result<PreparedCommit> Prepare(const ConfigurationDraft &draft);
        /** @brief Checks a draft against the current revision and schema without mutating the active snapshot. */
        [[nodiscard]] Result<void> Validate(const ConfigurationDraft &draft) const;
        /** @brief Atomically commits a draft if valid and matching baseRevision. */
        [[nodiscard]] Result<void> Commit(const ConfigurationDraft &draft);
        /** @brief Resolves and atomically publishes a complete multi-source candidate. */
        [[nodiscard]] Result<void> ResolveAndCommit(const ConfigurationResolutionRequest &request, const ConfigurationLimits &limits = {});
        /**
         * @brief Validates a complete reload candidate and replaces any older pending candidate.
         * @param request Complete captured source set.
         * @param limits Untrusted-input limits.
         * @return Validation findings or success; the active snapshot is unchanged.
         */
        [[nodiscard]] Result<void> StageReload(const ConfigurationResolutionRequest &request, const ConfigurationLimits &limits = {});
        /**
         * @brief Discards a staged or in-flight reload after host source capture or parsing fails.
         * @note The host must serialize capture attempts in source-change order before calling this or StageReload.
         */
        void CancelPendingReload();
        /**
         * @brief Activates the latest staged candidate when every changed descriptor permits this synchronization point.
         * @param point Host-owned activation boundary; call only off frame/job hot paths.
         * @return True for a committed snapshot, false for no pending change or a deferred policy.
         * @note Notifications are queued on EngineDataBus and delivered by its owner through DispatchQueued().
         */
        [[nodiscard]] Result<bool> ActivateReload(ConfigurationReloadPoint point);
        /** @brief Parses a versioned JSON document as user configuration and atomically commits it. */
        [[nodiscard]] Result<void> LoadJson(const std::string &jsonString);
        /** @brief Legacy direct file ingress is disabled; use Platform::ConfigurationFileStore. */
        [[nodiscard]] Result<void> LoadFile(const std::string &path) const;
        /** @brief Legacy direct persistence is disabled; use Platform::ConfigurationFileStore. */
        [[nodiscard]] Result<void> SaveFile(const std::string &path) const;

    private:
        struct PendingReload {
            std::shared_ptr<const ConfigurationSnapshot::Data> snapshot;
            std::vector<SettingKey> changedKeys;
            ConfigurationRevision baseRevision = 0;
        };

        ConfigurationSchema m_schema;
        // Serializes writers across host persistence and publication. Prepared commits are same-thread RAII owners.
        // Snapshot readers and source staging use m_mutex independently. Owners drain transactions before service shutdown.
        std::mutex m_writerMutex;
        mutable std::mutex m_mutex;
        std::shared_ptr<const ConfigurationSnapshot::Data> m_active;
        std::optional<PendingReload> m_pendingReload;
        std::uint64_t m_reloadSequence = 0;
        EngineDataBus *m_events = nullptr; /**< Borrowed process-owned notification bus. */
    };
}  // namespace Horo
