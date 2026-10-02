#pragma once

#include "Horo/Runtime/Ui/UiAccessibilityChanges.h"

#include <optional>
#include <vector>

namespace Horo::Runtime::Ui::AccessibilityInternal {
    /** @brief Preallocated lossless admission/FIFO delivery retained by the production semantic publisher. */
    class AnnouncementQueue final {
    public:
        /** @brief Reserves all queue and speech-byte storage at publisher creation. */
        AnnouncementQueue(const UiAccessibilityExtractorDescriptor &owner, const UiAccessibilityChangeLimits &limits);
        /** @brief Validates capacity/sequence and suppresses retries of still-unacknowledged occurrences. */
        [[nodiscard]] Result<void> Prepare(std::span<const UiAccessibilityAnnouncementInput> input, std::vector<std::uint8_t> &admitted);
        /** @brief Cancels stale admission scopes and commits fully validated occurrences without allocation. */
        void Commit(const UiAccessibilitySnapshot &snapshot, std::span<const UiAccessibilityAnnouncementInput> input,
                    std::span<const std::uint8_t> admitted) noexcept;
        /** @brief Converts pending delivery to terminal cancellation without releasing sequence evidence. */
        void Retire() noexcept;
        /** @brief Copies no data; returns call-duration retained delivery records. */
        [[nodiscard]] std::span<const UiAccessibilityAnnouncement> Records() const noexcept;
        /** @brief Returns speech only while the exact occurrence is pending. */
        [[nodiscard]] std::string_view Text(const UiAccessibilityAnnouncementCursor &cursor) const noexcept;
        /** @brief Releases an acknowledged prefix and compacts owned bytes in bounded storage. */
        [[nodiscard]] Result<void> Acknowledge(const UiAccessibilityAnnouncementCursor &cursor);

    private:
        /** @brief Closes pending speech when document/tree, audience or modal activation changes. */
        [[nodiscard]] bool OwnerChanged(const UiAccessibilitySnapshot &snapshot) const noexcept;
        /** @brief Cancels one accepted record without replay or sequence loss. */
        static void Cancel(UiAccessibilityAnnouncement &record, UiAccessibilityAnnouncementCancellation reason) noexcept;
        /** @brief Reclaims acknowledged/cancelled speech bytes without moving outside reserved storage. */
        void CompactText() noexcept;
        UiAccessibilityAnnouncementCursor owner_;
        UiAccessibilityChangeLimits limits_;
        std::vector<UiAccessibilityAnnouncement> records_;
        std::vector<char> text_;
        std::optional<UiAccessibilitySnapshotDescriptor> source_;
        std::optional<UiFocusSnapshot> focus_;
        std::uint64_t nextSequence_{1};
        std::uint64_t acknowledged_{};
    };
}  // namespace Horo::Runtime::Ui::AccessibilityInternal
