#pragma once

/**
 * @file BlackboardInstance.h
 * @brief Generation-fenced gameplay-AI blackboard storage, snapshots, and safe-point batches.
 */

#include "Horo/AI/AICanonicalState.h"
#include "Horo/AI/AIIdentity.h"
#include "Horo/AI/BlackboardSchema.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Horo::AI {
    /** @brief Maximum writes admitted by one BlackboardSync transaction. */
    inline constexpr std::size_t MaximumBlackboardWritesPerBatch = MaximumBlackboardKeys;
    /** @brief Maximum observers owned by one agent-scoped blackboard instance. */
    inline constexpr std::size_t MaximumBlackboardObservers = MaximumBlackboardKeys;

    /** @brief Exact runtime, agent, schema-publication, and instance generation fence. */
    struct BlackboardInstanceBinding final {
        AgentHandle agent;                  /**< Owning agent in one exact SceneRuntime incarnation. */
        BlackboardSchemaId schema;          /**< Immutable schema identity. */
        std::uint32_t schemaVersion{};      /**< Non-zero authored schema version. */
        std::uint64_t schemaGeneration{};   /**< Non-zero runtime schema-publication generation. */
        std::uint32_t instanceGeneration{}; /**< Non-zero blackboard instance generation. */

        /** @brief Checks reserved values. @return Whether every binding component is well formed. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const BlackboardInstanceBinding &) const noexcept = default;
    };

    /** @brief One detached schema-keyed write owned by a batch. */
    struct BlackboardWrite final {
        BlackboardKeyId key;   /**< Stable typed schema key. */
        BlackboardValue value; /**< Owned typed candidate value. */
    };

    /** @brief Immutable facts published by one safe-point commit. */
    struct BlackboardCommitResult final {
        std::uint64_t revision{};                                         /**< Active monotonic revision after commit. */
        std::array<BlackboardKeyId, MaximumBlackboardKeys> changedKeys{}; /**< Key-sorted changed prefix. */
        std::size_t changedKeyCount{};                                    /**< Active changed-key count. */

        /** @brief Returns the changed-key prefix. @return Key-sorted immutable change facts. */
        [[nodiscard]] std::span<const BlackboardKeyId> Changes() const noexcept {
            return {changedKeys.data(), changedKeyCount};
        }
    };

    class BlackboardInstance;

    /** @brief Immutable validated change facts borrowed only for the duration of one publication callback. */
    class BlackboardNotificationBatch final {
    public:
        /** @brief Returns the exact publishing instance fence. @return Agent/schema/instance binding. */
        [[nodiscard]] const BlackboardInstanceBinding &Binding() const noexcept {
            return binding_;
        }

        /** @brief Returns the newly published revision. @return Non-zero monotonic revision. */
        [[nodiscard]] std::uint64_t Revision() const noexcept {
            return result_.revision;
        }

        /** @brief Returns the changed-key prefix. @return Callback-scoped immutable key-sorted facts. */
        [[nodiscard]] std::span<const BlackboardKeyId> Changes() const noexcept {
            return result_.Changes();
        }

    private:
        friend class BlackboardInstance;

        BlackboardNotificationBatch(BlackboardInstanceBinding binding, BlackboardCommitResult result) noexcept
            : binding_(binding), result_(std::move(result)) {}

        BlackboardInstanceBinding binding_;
        BlackboardCommitResult result_;
    };

    /**
     * @brief Allocation-free observer callback invoked on the BlackboardSync owner thread.
     * @param context Borrowed registration context that must remain alive until removal, task cancellation, replacement, or teardown.
     * @param notification Callback-scoped batch; neither it nor its span may be retained after return.
     */
    using BlackboardObserverCallback = void (*)(void *context, const BlackboardNotificationBatch &notification) noexcept;

    /** @brief Borrowed callback registration scoped to one exact agent and executing task. */
    struct BlackboardObserverRegistration final {
        AgentHandle agent;                     /**< Must equal the instance's exact agent handle. */
        TaskHandle task;                       /**< Active task owner used for deterministic cancellation cleanup. */
        BlackboardKeyId key;                   /**< Stable key whose changes wake this observer. */
        BlackboardObserverCallback callback{}; /**< Non-null, non-throwing owner-thread callback. */
        void *context{};                       /**< Borrowed callback context; ownership stays with the registrant. */
    };

    /** @brief Generation-fenced process-local handle for one observer slot. */
    struct BlackboardObserverToken final {
        BlackboardInstanceBinding binding;                         /**< Exact instance generation that issued the token. */
        std::size_t slot{std::numeric_limits<std::size_t>::max()}; /**< Bounded instance-local slot. */
        std::uint32_t generation{};                                /**< Non-zero slot generation. */

        /** @brief Checks representation. @return Whether the token has a bounded non-zero slot generation. */
        [[nodiscard]] bool IsValid() const noexcept {
            return binding.IsValid() && slot < MaximumBlackboardObservers && generation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const BlackboardObserverToken &) const noexcept = default;
    };

    /** @brief Detached bounded batch fenced to one exact instance revision. */
    class BlackboardWriteBatch final {
    public:
        BlackboardWriteBatch(const BlackboardWriteBatch &) = delete;
        BlackboardWriteBatch &operator=(const BlackboardWriteBatch &) = delete;
        BlackboardWriteBatch(BlackboardWriteBatch &&) noexcept = default;
        BlackboardWriteBatch &operator=(BlackboardWriteBatch &&) noexcept = default;

        /**
         * @brief Adds one owned write without touching live storage.
         * @param write Typed key/value candidate.
         * @return Success or a stable duplicate, invalid-key, or capacity failure.
         */
        [[nodiscard]] Result<void> Stage(BlackboardWrite write);

        /** @brief Returns the exact generation fence. @return Immutable binding. */
        [[nodiscard]] const BlackboardInstanceBinding &Binding() const noexcept {
            return binding_;
        }

        /** @brief Returns the captured active revision. @return Non-zero revision. */
        [[nodiscard]] std::uint64_t BaseRevision() const noexcept {
            return baseRevision_;
        }

        /** @brief Returns the staged prefix. @return Immutable insertion-ordered writes. */
        [[nodiscard]] std::span<const BlackboardWrite> Writes() const noexcept {
            return writes_;
        }

    private:
        friend class BlackboardInstance;

        BlackboardWriteBatch(BlackboardInstanceBinding binding, std::uint64_t revision) : binding_(binding), baseRevision_(revision) {}

        BlackboardInstanceBinding binding_;
        std::uint64_t baseRevision_{};
        std::vector<BlackboardWrite> writes_;
    };

    /** @brief Immutable owned observation stable across commits and expiring on generation replacement or teardown. */
    class BlackboardSnapshot final {
    public:
        /** @brief Returns the captured revision. @return Revision or typed stale failure after generation expiry. */
        [[nodiscard]] Result<std::uint64_t> Revision() const;
        /**
         * @brief Reads a typed value without exposing mutable instance storage.
         * @param key Stable schema key.
         * @return Owned optional value or a typed stale/unknown-key failure.
         */
        [[nodiscard]] Result<std::optional<BlackboardValue>> Read(BlackboardKeyId key) const;

        /** @brief Returns the exact captured generation fence. @return Immutable binding. */
        [[nodiscard]] const BlackboardInstanceBinding &Binding() const noexcept {
            return binding_;
        }

    private:
        friend class BlackboardInstance;

        BlackboardSnapshot(BlackboardInstanceBinding binding, std::shared_ptr<const BlackboardSchema> schema,
                           std::vector<std::optional<BlackboardValue>> values, std::uint64_t revision,
                           std::shared_ptr<std::atomic_bool> generationActive)
            : binding_(binding), schema_(std::move(schema)), values_(std::move(values)), revision_(revision),
              generationActive_(std::move(generationActive)) {}

        BlackboardInstanceBinding binding_;
        std::shared_ptr<const BlackboardSchema> schema_;
        std::vector<std::optional<BlackboardValue>> values_;
        std::uint64_t revision_{};
        std::shared_ptr<std::atomic_bool> generationActive_;
    };

    /** @brief Scene-owner blackboard whose only mutation is transactional BlackboardSync publication. */
    class BlackboardInstance final {
    public:
        /** @brief Factory-only construction token; callers cannot create one. */
        class ConstructionKey final {
            ConstructionKey() = default;
            friend class BlackboardInstance;
        };

        BlackboardInstance() = delete;
        /**
         * @brief Allocates a default-populated instance from one immutable schema publication.
         * @param binding Exact agent, runtime, schema, and instance generations.
         * @param schema Immutable admitted schema.
         * @return Owned instance or a stable validation/storage failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<BlackboardInstance>> Create(const BlackboardInstanceBinding &binding,
                                                                                std::shared_ptr<const BlackboardSchema> schema);
        /**
         * @brief Materializes fully validated canonical values in detached storage, including read-only and absent optional keys.
         * @param binding Destination generation fence, never copied from canonical state.
         * @param schema Admitted destination schema.
         * @param state Complete canonical destination layout after migration.
         * @return Detached instance or typed validation/storage failure; no live publication occurs.
         */
        [[nodiscard]] static Result<std::unique_ptr<BlackboardInstance>> CreateFromCanonical(const BlackboardInstanceBinding &binding,
                                                                                             std::shared_ptr<const BlackboardSchema> schema,
                                                                                             const BlackboardCanonicalState &state);
        /** @brief Captures the sole canonical schema-keyed value layout. @return Owned state or typed lifecycle/storage failure. */
        [[nodiscard]] Result<BlackboardCanonicalState> CaptureCanonical() const;
        /** @brief Captures an immutable value copy for worker observation. @return Snapshot or stale failure. */
        [[nodiscard]] Result<BlackboardSnapshot> Snapshot() const;
        /** @brief Starts a detached fixed-capacity batch. @return Revision-fenced batch or stale failure. */
        [[nodiscard]] Result<BlackboardWriteBatch> BeginWriteBatch() const;
        /**
         * @brief Registers one bounded key observer on the BlackboardSync owner thread.
         * @param registration Exact agent/task/key and borrowed callback context.
         * @return Generation-fenced token or a typed validation, capacity, or reentrancy failure.
         * @pre The callback context remains alive until successful removal, task cancellation, replacement, or teardown.
         */
        [[nodiscard]] Result<BlackboardObserverToken> RegisterObserverAtBlackboardSync(const BlackboardObserverRegistration &registration);
        /**
         * @brief Removes one exact observer generation on the BlackboardSync owner thread.
         * @param token Token returned by this instance generation.
         * @return Whether an active matching observer was removed, or a typed reentrancy failure.
         * @post A successful removal prevents every later callback for the token.
         */
        [[nodiscard]] Result<bool> RemoveObserverAtBlackboardSync(const BlackboardObserverToken &token);
        /**
         * @brief Removes every observer owned by a cancelled task on the BlackboardSync owner thread.
         * @param task Exact executing task handle.
         * @return Removed observer count or a typed invalid-handle/reentrancy failure.
         */
        [[nodiscard]] Result<std::size_t> CancelTaskObserversAtBlackboardSync(TaskHandle task);
        /**
         * @brief Validates and atomically applies a complete batch at the owner BlackboardSync safe point.
         * @param batch Detached candidate batch.
         * @return New revision and deterministic changes, or a stable failure with no mutation.
         */
        [[nodiscard]] Result<BlackboardCommitResult> CommitAtBlackboardSync(BlackboardWriteBatch batch);
        /**
         * @brief Transactionally migrates compatible values to a replacement schema/default layout.
         * @param replacementBinding Strictly newer schema and instance generation fence for the same agent.
         * @param replacementSchema Immutable replacement schema.
         * @return Success or a stable failure that preserves the old active instance.
         */
        [[nodiscard]] Result<void> ReplaceAtBlackboardSync(const BlackboardInstanceBinding &replacementBinding,
                                                           std::shared_ptr<const BlackboardSchema> replacementSchema);
        /** @brief Resets every key to the active schema default. @return Revision/change facts or stable failure. */
        [[nodiscard]] Result<BlackboardCommitResult> ResetAtBlackboardSync();
        /** @brief Idempotently invalidates batches, snapshots, observers, and active storage. @return Success or reentrancy failure. */
        [[nodiscard]] Result<void> TeardownAtBlackboardSync();

        /** @brief Checks lifecycle state. @return Whether the instance accepts snapshots and batches. */
        [[nodiscard]] bool IsActive() const noexcept {
            return active_;
        }

        BlackboardInstance(const BlackboardInstance &) = delete;
        BlackboardInstance &operator=(const BlackboardInstance &) = delete;

        /**
         * @brief Constructs validated factory-owned storage; callers use Create.
         * @param key Unforgeable token issued only by Create.
         * @param binding Validated instance generation fence.
         * @param schema Immutable admitted schema.
         * @param values Default-populated active values.
         * @param scratch Equally sized transaction scratch storage.
         * @param generationActive Shared lease-validity flag for this generation.
         */
        BlackboardInstance(const ConstructionKey &key, const BlackboardInstanceBinding &binding,
                           std::shared_ptr<const BlackboardSchema> schema, std::vector<std::optional<BlackboardValue>> values,
                           std::vector<std::optional<BlackboardValue>> scratch, std::shared_ptr<std::atomic_bool> generationActive)
            : binding_(binding), schema_(std::move(schema)), values_(std::move(values)), scratch_(std::move(scratch)),
              generationActive_(std::move(generationActive)) {
            (void)key;
        }

    private:
        struct ObserverSlot final {
            BlackboardObserverRegistration registration{};
            std::uint32_t generation{1};
            bool active{};
            bool retired{};
        };

        /** @brief Releases an active observer and advances or retires its token generation. @param slot Active owned slot. */
        static void ReleaseObserverSlot(ObserverSlot &slot) noexcept;
        /** @brief Invalidates every active observer before instance generation replacement or teardown. */
        void InvalidateObservers() noexcept;
        /** @brief Freezes and invokes matching callbacks for one non-empty committed result. @param result Published change facts. */
        void PublishNotification(const BlackboardCommitResult &result) noexcept;

        BlackboardInstanceBinding binding_;
        std::shared_ptr<const BlackboardSchema> schema_;
        std::vector<std::optional<BlackboardValue>> values_;
        std::vector<std::optional<BlackboardValue>> scratch_;
        std::uint64_t revision_{1};
        std::shared_ptr<std::atomic_bool> generationActive_;
        std::array<ObserverSlot, MaximumBlackboardObservers> observers_{};
        bool publishing_{};
        bool active_{true};
    };
}  // namespace Horo::AI
