#include "UiAccessibilityAnnouncementQueue.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace Horo::Runtime::Ui::AccessibilityInternal {
    /** @copydoc AnnouncementQueue::AnnouncementQueue */
    AnnouncementQueue::AnnouncementQueue(const UiAccessibilityExtractorDescriptor &owner, const UiAccessibilityChangeLimits &limits)
        : owner_{owner.instance, owner.canvas, UiAccessibilityAnnouncementGeneration::Create(limits.deliveryGeneration).Value(), 0},
          limits_(limits) {
        records_.reserve(limits.announcements);
        text_.reserve(limits.announcementTextBytes);
    }

    /** @copydoc AnnouncementQueue::Prepare */
    Result<void> AnnouncementQueue::Prepare(const std::span<const UiAccessibilityAnnouncementInput> input,
                                            std::vector<std::uint8_t> &admitted) {
        std::size_t count{};
        std::size_t bytes{};
        for (std::size_t index = 0; index < input.size(); ++index) {
            if (admitted[index] == 0)
                continue;
            if (std::ranges::any_of(records_, [&](const auto &record) {
                return record.id == input[index].id && record.node == input[index].node;
            })) {
                admitted[index] = 0;
                continue;
            }
            ++count;
            bytes += input[index].text.text.size();
        }
        if (count > limits_.announcements - records_.size() || bytes > limits_.announcementTextBytes - text_.size())
            return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
        if (count > std::numeric_limits<std::uint64_t>::max() - nextSequence_)
            return Result<void>::Failure(MakeError(UiErrors::GenerationExhausted));
        return Result<void>::Success();
    }

    /** @copydoc AnnouncementQueue::OwnerChanged */
    bool AnnouncementQueue::OwnerChanged(const UiAccessibilitySnapshot &snapshot) const noexcept {
        if (!source_)
            return false;
        if (const auto &next = snapshot.Descriptor();
            source_->documentRevision != next.documentRevision || source_->treeRevision != next.treeRevision)
            return true;
        const auto state = snapshot.FocusState();
        if (focus_.has_value() != (state != nullptr))
            return true;
        return state && (focus_->owner.scope != state->owner.scope || focus_->activeModal != state->activeModal);
    }

    /** @copydoc AnnouncementQueue::Cancel */
    void AnnouncementQueue::Cancel(UiAccessibilityAnnouncement &record, const UiAccessibilityAnnouncementCancellation reason) noexcept {
        if (record.state == UiAccessibilityAnnouncementState::Pending) {
            record.state = UiAccessibilityAnnouncementState::Cancelled;
            record.cancellation = reason;
            record.text = {};
        }
    }

    /** @copydoc AnnouncementQueue::Commit */
    void AnnouncementQueue::Commit(const UiAccessibilitySnapshot &snapshot, const std::span<const UiAccessibilityAnnouncementInput> input,
                                   const std::span<const std::uint8_t> admitted) noexcept {
        const bool changed = OwnerChanged(snapshot);
        for (auto &record : records_) {
            if (changed) {
                Cancel(record, UiAccessibilityAnnouncementCancellation::OwnerChanged);
                continue;
            }
            const auto node = std::ranges::find(snapshot.Nodes(), record.node, &UiAccessibilityNode::id);
            if (node == snapshot.Nodes().end() || node->state.Has(UiAccessibilityStateFlag::Disabled) ||
                (node->exposure != UiAccessibilityExposure::Visible && node->exposure != UiAccessibilityExposure::Offscreen))
                Cancel(record, UiAccessibilityAnnouncementCancellation::NodeUnavailable);
        }
        for (std::size_t index = 0; index < input.size(); ++index) {
            if (admitted[index] == 0)
                continue;
            const auto &event = input[index];
            auto cursor = owner_;
            cursor.sequence = nextSequence_++;
            const auto offset = static_cast<std::uint32_t>(text_.size());
            text_.insert(text_.end(), event.text.text.begin(), event.text.text.end());
            records_.push_back({cursor,
                                event.id,
                                event.node,
                                snapshot.Descriptor().semanticRevision,
                                event.kind,
                                event.policy,
                                UiAccessibilityAnnouncementState::Pending,
                                UiAccessibilityAnnouncementCancellation::None,
                                {offset, static_cast<std::uint32_t>(event.text.text.size()), event.text.source}});
        }
        source_ = snapshot.Descriptor();
        const auto state = snapshot.FocusState();
        focus_ = state ? std::optional<UiFocusSnapshot>{*state} : std::nullopt;
    }

    /** @copydoc AnnouncementQueue::Retire */
    void AnnouncementQueue::Retire() noexcept {
        for (auto &record : records_)
            Cancel(record, UiAccessibilityAnnouncementCancellation::Retired);
        CompactText();
        source_.reset();
        focus_.reset();
    }

    /** @copydoc AnnouncementQueue::Records */
    std::span<const UiAccessibilityAnnouncement> AnnouncementQueue::Records() const noexcept {
        return records_;
    }

    /** @copydoc AnnouncementQueue::Text */
    std::string_view AnnouncementQueue::Text(const UiAccessibilityAnnouncementCursor &cursor) const noexcept {
        const auto record = std::ranges::find(records_, cursor, &UiAccessibilityAnnouncement::cursor);
        if (record == records_.end() || record->state != UiAccessibilityAnnouncementState::Pending)
            return {};
        return {text_.data() + record->text.offset, record->text.size};
    }

    /** @copydoc AnnouncementQueue::CompactText */
    void AnnouncementQueue::CompactText() noexcept {
        std::size_t offset{};
        for (auto &record : records_) {
            if (!record.text.IsPresent())
                continue;
            std::memmove(text_.data() + offset, text_.data() + record.text.offset, record.text.size);
            record.text.offset = static_cast<std::uint32_t>(offset);
            offset += record.text.size;
        }
        text_.resize(offset);
    }

    /** @copydoc AnnouncementQueue::Acknowledge */
    Result<void> AnnouncementQueue::Acknowledge(const UiAccessibilityAnnouncementCursor &cursor) {
        if (cursor.instance != owner_.instance || cursor.canvas != owner_.canvas || cursor.generation != owner_.generation ||
            cursor.sequence == 0 || cursor.sequence >= nextSequence_)
            return Result<void>::Failure(MakeError(UiErrors::AccessibilitySnapshotSourceStale));
        // The cursor may belong to a retained record that prefix erasure moves or destroys.
        const auto sequence = cursor.sequence;
        if (sequence <= acknowledged_)
            return Result<void>::Success();
        const auto end = std::ranges::find_if(records_, [&](const auto &record) {
            return record.cursor.sequence > sequence;
        });
        records_.erase(records_.begin(), end);
        acknowledged_ = sequence;
        CompactText();
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui::AccessibilityInternal
