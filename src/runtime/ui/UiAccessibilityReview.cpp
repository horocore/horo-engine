#include "Horo/Runtime/Ui/UiAccessibilityReview.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <limits>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Copies whole bounded UTF-8 text, reporting omission without retaining a broken prefix. */
        template <std::size_t N>
        void CopyText(const std::string_view source, std::array<char, N> &target, std::uint16_t &bytes, bool &omitted) noexcept {
            if (source.size() > N) {
                omitted = true;
                return;
            }
            std::copy(source.begin(), source.end(), target.begin());
            bytes = static_cast<std::uint16_t>(source.size());
        }
    }  // namespace

    /** @copydoc UiAccessibilityReviewEntry::Name */
    std::string_view UiAccessibilityReviewEntry::Name() const noexcept {
        return {name.data(), nameBytes};
    }

    /** @copydoc UiAccessibilityReviewEntry::Text */
    std::string_view UiAccessibilityReviewEntry::Text() const noexcept {
        return {text.data(), textBytes};
    }

    /** @copydoc UiAccessibilityReviewLog::SetEnabled */
    void UiAccessibilityReviewLog::SetEnabled(const bool enabled, const bool captureText) noexcept {
        if (enabled_ != enabled || captureText_ != captureText)
            Clear();
        enabled_ = enabled;
        captureText_ = captureText;
    }

    /** @copydoc UiAccessibilityReviewLog::Clear */
    void UiAccessibilityReviewLog::Clear() noexcept {
        entries_ = {};
        head_ = count_ = 0;
        overwritten_ = 0;
    }

    /** @copydoc UiAccessibilityReviewLog::Record */
    Result<std::size_t> UiAccessibilityReviewLog::Record(const UiAccessibilitySnapshot &snapshot,
                                                         const UiAccessibilityChangePublisher &publisher) {
        if (!enabled_)
            return Result<std::size_t>::Success(0);
        if (!snapshot.IsValid() || snapshot.Descriptor().semanticRevision != publisher.Revision())
            return Result<std::size_t>::Failure(MakeError(UiErrors::AccessibilitySnapshotSourceStale));
        for (const auto &change : publisher.Changes()) {
            if (change.kind != UiAccessibilityChangeKind::Removed && change.node.IsValid() && snapshot.Get(change.node).HasError())
                return Result<std::size_t>::Failure(MakeError(UiErrors::AccessibilitySnapshotSourceStale));
        }
        for (const auto &change : publisher.Changes()) {
            UiAccessibilityReviewEntry entry;
            entry.instance = snapshot.Descriptor().instance;
            entry.canvas = snapshot.Descriptor().canvas;
            entry.revision = publisher.Revision();
            entry.kind = change.kind;
            entry.node = change.node;
            entry.properties = change.properties;
            if (change.kind != UiAccessibilityChangeKind::Removed && change.node.IsValid()) {
                const auto node = snapshot.Get(change.node).Value();
                entry.role = node.role;
                entry.valueKind = node.value.kind;
                if (captureText_) {
                    entry.boolean = node.value.boolean;
                    entry.integer = node.value.integer;
                    entry.number = node.value.number;
                    CopyText(snapshot.Text(node.name), entry.name, entry.nameBytes, entry.textOmitted);
                    CopyText(snapshot.Text(node.value.text), entry.text, entry.textBytes, entry.textOmitted);
                }
            }
            if (captureText_ && change.kind == UiAccessibilityChangeKind::Announcement) {
                entry.text = {};
                entry.textBytes = 0;
                CopyText(publisher.Text(change.text), entry.text, entry.textBytes, entry.textOmitted);
            }
            if (count_ == Capacity) {
                entries_[head_] = entry;
                head_ = (head_ + 1) % Capacity;
                if (overwritten_ != std::numeric_limits<std::uint64_t>::max())
                    ++overwritten_;
            } else {
                entries_[(head_ + count_) % Capacity] = entry;
                ++count_;
            }
        }
        return Result<std::size_t>::Success(publisher.Changes().size());
    }

    /** @copydoc UiAccessibilityReviewLog::Size */
    std::size_t UiAccessibilityReviewLog::Size() const noexcept {
        return count_;
    }

    /** @copydoc UiAccessibilityReviewLog::Entry */
    const UiAccessibilityReviewEntry *UiAccessibilityReviewLog::Entry(const std::size_t index) const noexcept {
        if (index >= count_)
            return nullptr;
        return &entries_[(head_ + index) % Capacity];
    }

    /** @copydoc UiAccessibilityReviewLog::Overwritten */
    std::uint64_t UiAccessibilityReviewLog::Overwritten() const noexcept {
        return overwritten_;
    }
}  // namespace Horo::Runtime::Ui
