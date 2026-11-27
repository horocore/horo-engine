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

        /** @brief Copies one validated event under the journal's text disclosure policy. */
        UiAccessibilityReviewEntry BuildEntry(const UiAccessibilitySnapshot &snapshot, const UiAccessibilityChangePublisher &publisher,
                                              const UiAccessibilityChange &change, const bool captureText) {
            UiAccessibilityReviewEntry entry{.instance = snapshot.Descriptor().instance,
                                             .canvas = snapshot.Descriptor().canvas,
                                             .revision = publisher.Revision(),
                                             .kind = change.kind,
                                             .node = change.node,
                                             .properties = change.properties};
            std::string_view text;
            if (change.kind != UiAccessibilityChangeKind::Removed && change.node.IsValid()) {
                const auto node = snapshot.Get(change.node).Value();
                entry.role = node.role;
                entry.valueKind = node.value.kind;
                if (captureText) {
                    entry.boolean = node.value.boolean;
                    entry.integer = node.value.integer;
                    entry.number = node.value.number;
                    CopyText(snapshot.Text(node.name), entry.name, entry.nameBytes, entry.textOmitted);
                    text = snapshot.Text(node.value.text);
                }
            }
            if (captureText) {
                if (change.kind == UiAccessibilityChangeKind::Announcement)
                    text = publisher.Text(change.text);
                CopyText(text, entry.text, entry.textBytes, entry.textOmitted);
            }
            return entry;
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

    /** @copydoc UiAccessibilityReviewLog::Append */
    void UiAccessibilityReviewLog::Append(const UiAccessibilityReviewEntry &entry, const Publication &publication) noexcept {
        const auto index = (head_ + count_) % Capacity;
        entries_[index] = {entry, publication};
        if (count_ < Capacity) {
            ++count_;
            return;
        }
        head_ = (head_ + 1) % Capacity;
        if (overwritten_ != std::numeric_limits<std::uint64_t>::max())
            ++overwritten_;
    }

    /** @copydoc UiAccessibilityReviewLog::Record */
    Result<std::size_t> UiAccessibilityReviewLog::Record(const UiAccessibilitySnapshot &snapshot,
                                                         const UiAccessibilityChangePublisher &publisher) {
        if (!enabled_)
            return Result<std::size_t>::Success(0);
        if (!publisher.IsCurrentSnapshot(snapshot))
            return Result<std::size_t>::Failure(MakeError(UiErrors::AccessibilitySnapshotSourceStale));
        const Publication current{publisher.ReviewIdentity(), publisher.Revision()};
        const auto recorded = std::ranges::any_of(entries_, [&](const RetainedEvent &prior) {
            return prior.publication.Matches(current);
        });
        if (recorded)
            return Result<std::size_t>::Success(0);

        const auto changes = publisher.Changes();
        for (const auto &change : changes) {
            if (change.kind != UiAccessibilityChangeKind::Removed && change.node.IsValid() && snapshot.Get(change.node).HasError())
                return Result<std::size_t>::Failure(MakeError(UiErrors::AccessibilitySnapshotSourceStale));
        }
        for (const auto &change : changes)
            Append(BuildEntry(snapshot, publisher, change, captureText_), current);
        return Result<std::size_t>::Success(changes.size());
    }

    /** @copydoc UiAccessibilityReviewLog::Size */
    std::size_t UiAccessibilityReviewLog::Size() const noexcept {
        return count_;
    }

    /** @copydoc UiAccessibilityReviewLog::Entry */
    const UiAccessibilityReviewEntry *UiAccessibilityReviewLog::Entry(const std::size_t index) const noexcept {
        if (index >= count_)
            return nullptr;
        return &entries_[(head_ + index) % Capacity].entry;
    }

    /** @copydoc UiAccessibilityReviewLog::Overwritten */
    std::uint64_t UiAccessibilityReviewLog::Overwritten() const noexcept {
        return overwritten_;
    }
}  // namespace Horo::Runtime::Ui
