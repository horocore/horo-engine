#pragma once

/** @file UiAccessibilityReview.h
 * @brief Explicit bounded semantic QA journal, separate from native screen-reader delivery.
 */
#include "Horo/Runtime/Ui/UiAccessibilityChanges.h"

#include <array>

namespace Horo::Runtime::Ui {
    /** @brief Owned diagnostic event; text ranges never refer back into a widget, snapshot or publisher. */
    struct UiAccessibilityReviewEntry final {
        RuntimeUiInstanceId instance;
        UiCanvasInstanceId canvas;
        UiAccessibilitySemanticRevision revision;
        UiAccessibilityChangeKind kind{};
        UiAccessibilityNodeId node;
        std::uint32_t properties{};
        std::optional<UiAccessibilityRole> role;
        UiAccessibilityValueKind valueKind{UiAccessibilityValueKind::None};
        bool boolean{};
        std::int64_t integer{};
        double number{};
        std::array<char, 256> name{};
        std::array<char, 512> text{};
        std::uint16_t nameBytes{}, textBytes{};
        bool textOmitted{}; /**< Oversized text is omitted whole, never split at an arbitrary UTF-8 byte. */

        /** @brief Returns captured label text, empty unless explicitly enabled. @return Borrowed owned UTF-8. */
        [[nodiscard]] std::string_view Name() const noexcept;
        /** @brief Returns captured value/announcement preview text. @return Borrowed owned UTF-8. */
        [[nodiscard]] std::string_view Text() const noexcept;
    };

    /**
     * @brief Owner-thread optional ring journal over semantic change events; disabled by default.
     * @details Capacity is 64 events. Oldest diagnostics are overwritten with a saturating
     * counter; this never acknowledges, drops or changes accepted speech delivery. Record is
     * event-driven and allocation-free on valid inputs. No native API is invoked; journal
     * entries prove semantic observations only, not screen-reader support or actual speech.
     * Text capture is an explicit development/QA privacy choice, disabled independently.
     */
    class UiAccessibilityReviewLog final {
    public:
        static constexpr std::size_t Capacity = 64;
        /** @brief Enables explicit QA recording; changing policy clears retained text/events.
         * @param enabled Whether to retain semantic event diagnostics.
         * @param captureText Whether project/user text and scalar values may be copied into the bounded journal.
         */
        void SetEnabled(bool enabled, bool captureText = false) noexcept;
        /** @brief Scrubs retained data and counters without changing recording policy. */
        void Clear() noexcept;
        /** @brief Copies one successful publication's changes, once per publication at the UI owner boundary.
         * @param snapshot Exact semantic snapshot adopted by publisher.
         * @param publisher Event producer; speech previews are never treated as delivery acknowledgments.
         * @return Number observed, zero when disabled, or a typed stale-source failure before journal mutation.
         */
        [[nodiscard]] Result<std::size_t> Record(const UiAccessibilitySnapshot &snapshot, const UiAccessibilityChangePublisher &publisher);
        /** @brief Returns retained diagnostic count. @return At most Capacity. */
        [[nodiscard]] std::size_t Size() const noexcept;
        /** @brief Borrows one retained event in oldest-to-newest order.
         * @param index Chronological index. @return Pointer valid until mutation, or null for out-of-range.
         */
        [[nodiscard]] const UiAccessibilityReviewEntry *Entry(std::size_t index) const noexcept;
        /** @brief Returns saturating overwritten diagnostic count. @return Lost journal records, never lost speech count. */
        [[nodiscard]] std::uint64_t Overwritten() const noexcept;

    private:
        std::array<UiAccessibilityReviewEntry, Capacity> entries_{};
        std::size_t head_{}, count_{};
        std::uint64_t overwritten_{};
        bool enabled_{}, captureText_{};
    };
}  // namespace Horo::Runtime::Ui
