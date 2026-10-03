#pragma once

/** @file UiAccessibilityChanges.h
 * @brief Owner-thread bounded semantic changes over retained Runtime UI accessibility snapshots.
 */

#include "Horo/Runtime/Ui/UiAccessibility.h"

#include <memory>

namespace Horo::Runtime::Ui {
    inline constexpr std::uint32_t MaximumUiAccessibilityChanges = 16'384;
    inline constexpr std::uint32_t MaximumUiAccessibilityAnnouncements = 256;

    /** @brief Stable owner-issued live-region event identity; zero is invalid. */
    struct UiAccessibilityAnnouncementId final {
        std::uint64_t value{};
        [[nodiscard]] constexpr auto operator<=>(const UiAccessibilityAnnouncementId &) const noexcept = default;
    };

    /** @brief Live-region delivery policy; Off validates but produces no announcement. */
    enum class UiAccessibilityAnnouncementPolicy : std::uint8_t {
        Off,
        Polite,
        Assertive
    };

    /** @brief Semantic occurrence category; validation text must match the published node's error. */
    enum class UiAccessibilityAnnouncementKind : std::uint8_t {
        Event,
        Validation,
        Status
    };

    /** @brief Explicit terminal disposition of retained announcement delivery. */
    enum class UiAccessibilityAnnouncementState : std::uint8_t {
        Pending,
        Cancelled
    };

    /** @brief Reason that an accepted occurrence is no longer eligible for speech. */
    enum class UiAccessibilityAnnouncementCancellation : std::uint8_t {
        None,
        OwnerChanged,
        NodeUnavailable,
        Retired
    };

    struct UiAccessibilityAnnouncementGenerationTag;
    using UiAccessibilityAnnouncementGeneration = UiRevision<UiAccessibilityAnnouncementGenerationTag>;

    /** @brief Exact owner/session cursor for cumulative, idempotent acknowledgment. Zero sequence is invalid. */
    struct UiAccessibilityAnnouncementCursor final {
        RuntimeUiInstanceId instance;
        UiCanvasInstanceId canvas;
        UiAccessibilityAnnouncementGeneration generation;
        std::uint64_t sequence{};
        [[nodiscard]] constexpr auto operator<=>(const UiAccessibilityAnnouncementCursor &) const noexcept = default;
    };

    /** @brief Borrowed announcement copied during publication, scoped to an exact exposed semantic node. */
    struct UiAccessibilityAnnouncementInput final {
        UiAccessibilityAnnouncementId id;
        UiAccessibilityNodeId node;
        UiAccessibilityAnnouncementPolicy policy{UiAccessibilityAnnouncementPolicy::Polite};
        UiAccessibilityTextInput text;
        UiAccessibilityAnnouncementKind kind{UiAccessibilityAnnouncementKind::Event};
    };

    /** @brief One accepted FIFO occurrence; text is owned by the publisher until its cursor is acknowledged.
     * @details Cancelled records retain sequence evidence but return no speech text. Revision changes alone do not cancel;
     *          document/tree identity, focus audience or modal activation changes do. Status/events are never coalesced.
     */
    struct UiAccessibilityAnnouncement final {
        UiAccessibilityAnnouncementCursor cursor;
        UiAccessibilityAnnouncementId id;
        UiAccessibilityNodeId node;
        UiAccessibilitySemanticRevision revision;
        UiAccessibilityAnnouncementKind kind{UiAccessibilityAnnouncementKind::Event};
        UiAccessibilityAnnouncementPolicy policy{UiAccessibilityAnnouncementPolicy::Off};
        UiAccessibilityAnnouncementState state{UiAccessibilityAnnouncementState::Pending};
        UiAccessibilityAnnouncementCancellation cancellation{UiAccessibilityAnnouncementCancellation::None};
        UiAccessibilityTextRef text;
    };

    /** @brief Closed change categories; structural records use exact generation-checked identities. */
    enum class UiAccessibilityChangeKind : std::uint8_t {
        Removed,
        Inserted,
        Structure,
        Properties,
        Focus,
        Announcement,
        Resynchronize
    };

    /** @brief Changed semantic property groups, coalesced into one mask per node per publication. */
    enum class UiAccessibilityProperty : std::uint32_t {
        Role = 1U << 0U,
        Source = 1U << 1U,
        Name = 1U << 2U,
        Description = 1U << 3U,
        Value = 1U << 4U,
        State = 1U << 5U,
        Range = 1U << 6U,
        Selection = 1U << 7U,
        Error = 1U << 8U,
        Exposure = 1U << 9U,
        Bounds = 1U << 10U,
        Relations = 1U << 11U,
        Actions = 1U << 12U
    };

    /** @brief One owned change; node is old for Removed, new for other kinds, and invalid for cleared focus/resync. */
    struct UiAccessibilityChange final {
        UiAccessibilityChangeKind kind{UiAccessibilityChangeKind::Properties};
        UiAccessibilityNodeId node;
        UiAccessibilityNodeId previous; /**< Old focus for Focus; previous sibling for Inserted/Structure. */
        UiAccessibilityNodeId parent;   /**< New semantic parent for Inserted/Structure. */
        std::uint32_t properties{};     /**< Mask of UiAccessibilityProperty for Properties. */
        UiAccessibilityAnnouncementId announcement;
        UiAccessibilityAnnouncementPolicy policy{UiAccessibilityAnnouncementPolicy::Off};
        UiAccessibilityTextRef text; /**< Range owned by this publisher, read through Text(). */
    };

    /** @brief Bounded publication outcome; overflow replaces all deltas with one Resynchronize record. */
    enum class UiAccessibilityChangeStatus : std::uint8_t {
        Complete,
        Resynchronize,
        Retired
    };

    /** @brief Load-time budgets; announcement history rejects exhaustion rather than evicting an unexpired identity. */
    struct UiAccessibilityChangeLimits final {
        std::uint32_t changes{1'024};
        std::uint32_t announcements{64};
        std::uint32_t announcementTextBytes{16'384};
        std::uint64_t deduplicationRevisions{32}; /**< Inclusive semantic-revision window; no wall-clock reads. */
        std::uint64_t deliveryGeneration{1}; /**< Host-issued non-reusable session generation within this owner; increment on recreation. */
        /** @brief Validates all storage ceilings. @return Whether creation can reserve the budgets. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /**
     * @brief Move-only owner-thread semantic delta publisher independent of rendering and native screen-reader adapters.
     * @details Create reserves all scratch/output/history storage. Publish retains only the latest immutable snapshot lease.
     *          All calls and borrowed reads are serialized on the Runtime UI owner thread. Output borrows survive failed Publish;
     *          successful Publish, Retire, move assignment or destruction invalidates them. Snapshot leases owned elsewhere survive.
     *          A consumer missing a predecessor revision must resync from the latest full snapshot before applying further deltas.
     */
    class UiAccessibilityChangePublisher final {
    public:
        /** @brief Creates bounded storage for one exact extractor owner.
         * @param owner Exact instance/canvas/document and node budget used by the extractor.
         * @param limits Fixed delta and announcement budgets.
         * @return Publisher or typed descriptor/capacity failure.
         */
        [[nodiscard]] static Result<UiAccessibilityChangePublisher> Create(const UiAccessibilityExtractorDescriptor &owner,
                                                                           const UiAccessibilityChangeLimits &limits = {});
        /** @brief Releases the retained snapshot and owned output. */
        ~UiAccessibilityChangePublisher();
        /** @brief Transfers publisher state and invalidates the source. @param other Owner to transfer. */
        UiAccessibilityChangePublisher(UiAccessibilityChangePublisher &&other) noexcept;
        /** @brief Releases current state and transfers another owner. @param other Owner to transfer. @return This publisher. */
        UiAccessibilityChangePublisher &operator=(UiAccessibilityChangePublisher &&other) noexcept;
        UiAccessibilityChangePublisher(const UiAccessibilityChangePublisher &) = delete;
        UiAccessibilityChangePublisher &operator=(const UiAccessibilityChangePublisher &) = delete;

        /** @brief Atomically compares and adopts a complete newer snapshot; failure preserves output, baseline and history.
         * @param snapshot Exact owner snapshot; first publication inserts the complete tree.
         * @param announcements Bounded active-node live-region events; duplicate IDs within a batch are invalid.
         * @return Success (including explicit resync overflow) or typed stale/schema/capacity/lifecycle failure.
         * @details Removal order is reverse old preorder; additions/updates follow new preorder; focus then announcements follow.
         *          Covered/suppressed/suspended/disabled nodes cannot announce. Off events produce no output.
         *          Delta overflow suppresses announcement delta previews, while accepted delivery remains in Announcements().
         *          Delivery count/byte exhaustion rejects the entire publication before mutation; retry is explicit.
         */
        [[nodiscard]] Result<void> Publish(const UiAccessibilitySnapshot &snapshot,
                                           std::span<const UiAccessibilityAnnouncementInput> announcements = {});
        /** @brief Publishes final removals/cleared focus (or resync on overflow), releases the baseline, and closes admission idempotently.
         */
        void Retire() noexcept;
        /** @brief Returns current admission. @return True after retirement or move. */
        [[nodiscard]] bool IsRetired() const noexcept;
        /** @brief Returns output status. @return Complete, overflow resync, or final retirement. */
        [[nodiscard]] UiAccessibilityChangeStatus Status() const noexcept;
        /** @brief Returns the predecessor semantic revision. @return Invalid for the first publication. */
        [[nodiscard]] UiAccessibilitySemanticRevision PreviousRevision() const noexcept;
        /** @brief Returns the adopted semantic revision. @return Invalid before first publication. */
        [[nodiscard]] UiAccessibilitySemanticRevision Revision() const noexcept;
        /** @brief Returns bounded immutable change records. @return Borrowed output with the class lifetime rules. */
        [[nodiscard]] std::span<const UiAccessibilityChange> Changes() const noexcept;
        /** @brief Resolves an announcement's owned text. @param text Range from current Changes(). @return Text or empty for invalid range.
         */
        [[nodiscard]] std::string_view Text(UiAccessibilityTextRef text) const noexcept;
        /** @brief Returns accepted unacknowledged occurrences in sequence order, independent of semantic delta resync.
         * @return Borrowed retained queue. Successful Publish, Acknowledge, Retire, move or destruction invalidates the borrow.
         * @note Changes() announcement records are previews; Announcements() is the sole delivery authority.
         */
        [[nodiscard]] std::span<const UiAccessibilityAnnouncement> Announcements() const noexcept;
        /** @brief Resolves retained speech text only for an exact currently pending cursor.
         * @param cursor Cursor from Announcements(). @return Owned borrowed text, or empty for stale/cancelled/acknowledged cursors.
         */
        [[nodiscard]] std::string_view AnnouncementText(const UiAccessibilityAnnouncementCursor &cursor) const noexcept;
        /** @brief Cumulatively acknowledges accepted delivery, releasing its prefix and bytes without replay.
         * @param cursor Exact owner/session and accepted sequence; already acknowledged cursors are idempotent.
         * @return Success or stale/foreign/future cursor failure; valid acknowledgments remain permitted after retirement.
         * @note Acknowledge only after copying/delivering or observing cancellation. Resync never implies acknowledgment.
         */
        [[nodiscard]] Result<void> Acknowledge(const UiAccessibilityAnnouncementCursor &cursor);

    private:
        struct Storage;
        explicit UiAccessibilityChangePublisher(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
