#pragma once

/** @file SceneDocumentTransaction.h
 * @brief Narrow headless transaction port into the canonical authored Scene document and its existing history.
 */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Scene/SceneSourceModel.h"

#include <memory>
#include <thread>

namespace Horo::Editor {
    class SceneDocument;
    class EditorHistory;
    class NavigationSettingsWorkflow;
    class NavigationSettingsReview;
    class NavigationProjectSettingsSession;

    /** @brief Closed zero-mutation transaction rejection vocabulary. */
    enum class SceneTransactionFailure : std::uint8_t {
        Closed,
        WrongThread,
        Stale,
        Unauthorized,
        Cancelled,
        Invalid,
        Capacity
    };
    /** @brief Return the stable descriptor for a rejected transaction. @param reason Closed cause. @return Descriptor. */
    [[nodiscard]] const ErrorCodeDescriptor &SceneTransactionError(SceneTransactionFailure reason) noexcept;

    /** @brief Host-issued document opening identity; never inferred from an address or a generated object. */
    struct SceneTransactionSession final {
        Assets::AssetId owner;
        std::uint64_t generation{};
        [[nodiscard]] bool operator==(const SceneTransactionSession &) const noexcept = default;
    };

    /** @brief Exact immutable authored state and generation ownership captured from one canonical Scene document. */
    struct SceneTransactionSnapshot final {
        SceneTransactionSession session;
        std::uint64_t revision{};
        std::uint64_t state{};
        bool dirty{};
        std::vector<SceneSource::SceneObjectSnapshot> objects;
        std::vector<SceneSource::ScenePrefabInstance> prefabInstances;
    };

    /** @brief Explicit owner-phase permission and revision fence; default admission is denied. */
    struct SceneTransactionContext final {
        SceneTransactionSession session;
        std::uint64_t revision{};
        std::uint64_t state{};
        bool canEdit{};
        CancellationToken cancellation;
    };

    /** @brief Explicit target fragment reservation; generated edits must cover actual new, staged and retired values. */
    struct SceneObjectResourceBudget final {
        std::uint64_t residentBytes{};
        std::uint64_t preparationBytes{};
        std::uint64_t retirementBytes{};
    };

    /** @brief One exact create/update/remove; absence of desired removes, while zero prior content requires a free ID.
     * @details Update/removal must match both canonical content and complete ownership. An empty expected owner
     * matches only hand-authored content; it never authorizes deleting an arbitrary generated object.
     */
    struct SceneObjectReplacement final {
        SceneSource::SceneObjectId object;
        Sha256Digest priorContent{};
        std::optional<SceneSource::SceneGeneratedOwnership> expectedOwnership;
        std::optional<SceneSource::SceneObjectSnapshot> desired;
        SceneObjectResourceBudget resources;
    };

    /** @brief Finite per-transaction limits, bounded further by the existing Scene semantic-history ceiling. */
    struct SceneTransactionLimits final {
        std::size_t maximumChanges{1024};
        std::size_t maximumPreparedBytes{32 * 1024 * 1024};
        std::size_t maximumSnapshotObjects{16'384};
    };

    /** @brief One coherent Scene/history publication; false means an idempotent zero-mutation request. */
    struct SceneTransactionResult final {
        std::uint64_t revision{};
        std::uint64_t state{};
        bool committed{};
    };

    /** @brief Owner-thread headless port to the actual SceneDocument and EditorHistory, never a parallel authoring model.
     * @details Create owns those same canonical types for headless hosts. Bind borrows the existing editor document
     * and its one history; both outlive this port and the host admits only one port per opening. All preparation,
     * validation, reversible deltas and history capacity are reserved before one no-fail owner swap. Failure changes
     * neither visible objects, generated ownership, revision, dirty state nor history. Replay restores exact stored
     * authored values without invoking a generator or asset provider. Only this target sees Scene private headers.
     */
    class SceneDocumentTransactions final {
    public:
        /** @brief Construct an inert port. */
        SceneDocumentTransactions() noexcept;
        /** @brief Release this port and any owned headless document/history. */
        ~SceneDocumentTransactions();
        SceneDocumentTransactions(const SceneDocumentTransactions &) = delete;
        SceneDocumentTransactions &operator=(const SceneDocumentTransactions &) = delete;
        /** @brief Transfer the port and its unchanged owner-thread admission. @param other Source port. */
        SceneDocumentTransactions(SceneDocumentTransactions &&other) noexcept;
        /** @brief Replace this port by transfer. @param other Source port. @return This port. */
        SceneDocumentTransactions &operator=(SceneDocumentTransactions &&other) noexcept;

        /** @brief Create a canonical headless authored Scene and its bounded existing history.
         * @param session Nonzero never-reused host opening identity.
         * @param objects Complete validated authored baseline.
         * @param prefabInstances Complete authored prefab placements.
         * @return Owning port or original typed validation error; failed creation publishes nothing.
         */
        [[nodiscard]] static Result<SceneDocumentTransactions> Create(SceneTransactionSession session,
                                                                      std::vector<SceneSource::SceneObjectSnapshot> objects = {},
                                                                      std::vector<SceneSource::ScenePrefabInstance> prefabInstances = {});
        /** @brief Borrow the existing canonical editor owner without creating another root or history.
         * @param document Canonical document borrowed until Shutdown/destruction.
         * @param history That exact document opening's sole history, borrowed for the same lifetime.
         * @param session Explicit host opening identity.
         * @return Prepared port or typed invalid identity error.
         */
        [[nodiscard]] static Result<SceneDocumentTransactions> Bind(SceneDocument &document, EditorHistory &history,
                                                                    SceneTransactionSession session);
        /** @brief Copy one complete owner snapshot.
         * @param limits Finite snapshot allocation and object ceilings.
         * @return Owned authored/ownership values or lifecycle/thread/capacity error.
         */
        [[nodiscard]] Result<SceneTransactionSnapshot> Snapshot(SceneTransactionLimits limits = {}) const;
        /** @brief Queries dirty state without copying the canonical Scene root.
         * @return Owner-thread dirty state or the same closed/reload/thread rejection as Snapshot.
         */
        [[nodiscard]] Result<bool> IsDirty() const;

        /** @brief Atomically apply one complete replacement batch through canonical Scene/history ownership.
         * @param context Fresh exact owner fence, permission and cancellation.
         * @param changes Complete semantic replacements; no subtree or graph scans authorize deletion.
         * @param limits Finite preparation and change ceilings.
         * @return One coherent commit/no-op result or typed zero-mutation rejection.
         */
        [[nodiscard]] Result<SceneTransactionResult> Execute(const SceneTransactionContext &context,
                                                             std::span<const SceneObjectReplacement> changes,
                                                             SceneTransactionLimits limits = {});
        /** @brief Replay the canonical document's next history entry after exact owner admission.
         * @param context Fresh exact owner fence, permission and cancellation.
         * @param redo False for undo, true for redo.
         * @return One coherent replay or typed failure without generator work.
         */
        [[nodiscard]] Result<SceneTransactionResult> Replay(const SceneTransactionContext &context, bool redo);
        /** @brief Mark only an exact captured authored state saved; bake never calls this.
         * @param context Current owner state. @return Success or original lifecycle/state failure.
         */
        [[nodiscard]] Result<void> MarkSaved(const SceneTransactionContext &context);
        /** @brief Close this port's admission; a borrowed document/history remains owned by its host. */
        void Shutdown() noexcept;

    private:
        friend class NavigationSettingsWorkflow;
        friend class NavigationSettingsReview;
        friend class NavigationProjectSettingsSession;

        struct Lifetime final {
            SceneDocumentTransactions *owner{};
        };

        std::shared_ptr<Lifetime> lifetime_;
        struct Prepared;
        /** @brief Stages canonical root/history without publication for an owner-thread multi-document transaction. */
        [[nodiscard]] Result<std::shared_ptr<Prepared>> Prepare(const SceneTransactionContext &context,
                                                                std::span<const SceneObjectReplacement> changes,
                                                                SceneTransactionLimits limits = {}, bool coordinated = false) const;
        /** @brief Stages canonical replay for an exclusively admitted multi-document coordinator. */
        [[nodiscard]] Result<std::shared_ptr<Prepared>> PrepareReplay(const SceneTransactionContext &context, bool redo);
        /** @brief Computes the post-publication fence from prepared owner state without copying a root. */
        [[nodiscard]] SceneTransactionContext PreparedContext(const Prepared &prepared) const noexcept;
        /** @brief Rechecks the exact prepared owner fence without mutation or callbacks. */
        [[nodiscard]] Result<void> ValidatePrepared(const Prepared &prepared) const;
        /** @brief Performs the prevalidated no-fail root/history swap; coordinator validates every owner first. */
        [[nodiscard]] SceneTransactionResult PublishPrepared(Prepared &prepared) noexcept;
        /** @brief Reserve root/history/input preparation before copying any semantic replacements.
         * @param changes Submitted replacements. @param limits Finite host ceilings.
         * @return Remaining semantic history preparation budget or zero-mutation capacity failure.
         */
        [[nodiscard]] Result<std::size_t> PreparationBudget(std::span<const SceneObjectReplacement> changes,
                                                            SceneTransactionLimits limits) const;
        /** @brief Publish validated objects/counters after the no-fail history swap; called only after final admission.
         * @param candidate Fully prepared authored root. @param validated Canonical validated identity high-water marks.
         * @return Committed revision/state; performs no allocation or callbacks.
         */
        [[nodiscard]] SceneTransactionResult PublishRoot(std::vector<SceneSource::SceneObjectSnapshot> &candidate,
                                                         const SceneDocument &validated) noexcept;
        struct State;
        std::unique_ptr<State> state_;
        /** @brief Reject closed, replaced or foreign-thread access without touching canonical state. */
        [[nodiscard]] Result<void> CheckAccess() const;
        /** @brief Admit exact owner/session/thread/permission before inspecting or changing canonical state. */
        [[nodiscard]] Result<void> Admit(const SceneTransactionContext &context) const;
    };

    /** @brief Hard canonical object fingerprint allocation ceiling; transaction-wide budgets cannot raise it. */
    inline constexpr std::size_t MaximumSceneObjectFingerprintBytes = 16U * 1024U * 1024U;

    /** @brief Fingerprint canonical authored object content, excluding its separate inert ownership annotation.
     * @param object Complete typed authored value.
     * @param maximumBytes Positive canonical preparation ceiling, no greater than MaximumSceneObjectFingerprintBytes.
     * @return Stable content digest or original Scene codec failure.
     */
    [[nodiscard]] Result<Sha256Digest> FingerprintSceneObjectContent(const SceneSource::SceneObjectSnapshot &object,
                                                                     std::size_t maximumBytes);
}  // namespace Horo::Editor
