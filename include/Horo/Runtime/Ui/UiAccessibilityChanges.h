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

    /** @brief Borrowed announcement copied during publication, scoped to an exact exposed semantic node. */
    struct UiAccessibilityAnnouncementInput final {
        UiAccessibilityAnnouncementId id;
        UiAccessibilityNodeId node;
        UiAccessibilityAnnouncementPolicy policy{UiAccessibilityAnnouncementPolicy::Polite};
        UiAccessibilityTextInput text;
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
                                                                           UiAccessibilityChangeLimits limits = {});
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
         *          Overflow suppresses all announcements and remembers their IDs, so resync never replays them.
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

    private:
        struct Storage;
        explicit UiAccessibilityChangePublisher(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
