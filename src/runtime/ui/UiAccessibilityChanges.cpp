#include "Horo/Runtime/Ui/UiAccessibilityChanges.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <new>
#include <optional>
#include <ranges>
#include <tuple>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Builds typed failures without storing user content in diagnostics. */
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &code) {
            return Result<T>::Failure(MakeError(code));
        }

        /** @brief Snapshot index retains canonical position and preceding sibling independently of storage offsets. */
        struct NodeIndex final {
            UiAccessibilityNodeId id;
            std::size_t position{};
            UiAccessibilityNodeId previousSibling;
            UiAccessibilityNodeId lastChild;
        };

        /** @brief Finds an exact generation identity in a sorted, preallocated index. */
        auto Find(std::vector<NodeIndex> &index, const UiAccessibilityNodeId id) {
            return std::ranges::lower_bound(index, id, {}, &NodeIndex::id);
        }

        /** @brief Builds O(N log N) identity/sibling evidence using only reserved storage. */
        void BuildIndex(const UiAccessibilitySnapshot &snapshot, std::vector<NodeIndex> &index) {
            index.clear();
            const auto nodes = snapshot.Nodes();
            for (std::size_t position = 0; position < nodes.size(); ++position)
                index.push_back({nodes[position].id, position, {}, {}});
            std::ranges::sort(index, {}, &NodeIndex::id);
            UiAccessibilityNodeId rootSibling;
            for (const auto &node : nodes) {
                auto &entry = *Find(index, node.id);
                if (node.parent.IsValid()) {
                    auto &parent = *Find(index, node.parent);
                    entry.previousSibling = parent.lastChild;
                    parent.lastChild = node.id;
                } else {
                    entry.previousSibling = rootSibling;
                    rootSibling = node.id;
                }
            }
        }

        /** @brief Compares resolved content/provenance rather than mutable text-arena offsets. */
        bool EqualText(const UiAccessibilitySnapshot &left, const UiAccessibilityTextRef a, const UiAccessibilitySnapshot &right,
                       const UiAccessibilityTextRef b) noexcept {
            return a.source == b.source && left.Text(a) == right.Text(b);
        }

        /** @brief Compares only the active typed value alternative. */
        bool EqualValue(const UiAccessibilitySnapshot &left, const UiAccessibilityValue &a, const UiAccessibilitySnapshot &right,
                        const UiAccessibilityValue &b) noexcept {
            if (a.kind != b.kind)
                return false;
            switch (a.kind) {
                using enum UiAccessibilityValueKind;
                case None:
                    return true;
                case Boolean:
                    return a.boolean == b.boolean;
                case Integer:
                    return a.integer == b.integer;
                case Number:
                    return a.number == b.number;
                case Text:
                    return EqualText(left, a.text, right, b.text);
            }
            return false;
        }

        /** @brief Compares only active optional range metadata. */
        bool EqualRange(const UiAccessibilityNode &a, const UiAccessibilityNode &b) noexcept {
            return a.hasRange == b.hasRange &&
                   (!a.hasRange || std::tie(a.range.minimum, a.range.maximum, a.range.current, a.range.step) ==
                                       std::tie(b.range.minimum, b.range.maximum, b.range.current, b.range.step));
        }

        /** @brief Compares only active optional selection metadata. */
        bool EqualSelection(const UiAccessibilityNode &a, const UiAccessibilityNode &b) noexcept {
            return a.hasSelection == b.hasSelection &&
                   (!a.hasSelection || std::tie(a.selection.mode, a.selection.selected, a.selection.index, a.selection.count) ==
                                           std::tie(b.selection.mode, b.selection.selected, b.selection.index, b.selection.count));
        }

        /** @brief Coalesces every changed semantic field into one bounded property mask. */
        std::uint32_t Properties(const UiAccessibilitySnapshot &before, const UiAccessibilityNode &a, const UiAccessibilitySnapshot &after,
                                 const UiAccessibilityNode &b) noexcept {
            std::uint32_t result{};
            const auto mark = [&](const bool changed, const UiAccessibilityProperty property) {
                if (changed)
                    result |= static_cast<std::uint32_t>(property);
            };
            mark(a.role != b.role, UiAccessibilityProperty::Role);
            mark(a.source != b.source || a.contributor != b.contributor, UiAccessibilityProperty::Source);
            mark(!EqualText(before, a.name, after, b.name), UiAccessibilityProperty::Name);
            mark(!EqualText(before, a.description, after, b.description), UiAccessibilityProperty::Description);
            mark(!EqualValue(before, a.value, after, b.value), UiAccessibilityProperty::Value);
            mark(a.state != b.state, UiAccessibilityProperty::State);
            mark(!EqualRange(a, b), UiAccessibilityProperty::Range);
            mark(!EqualSelection(a, b), UiAccessibilityProperty::Selection);
            mark(a.error.kind != b.error.kind || !EqualText(before, a.error.message, after, b.error.message),
                 UiAccessibilityProperty::Error);
            mark(a.exposure != b.exposure, UiAccessibilityProperty::Exposure);
            mark(a.bounds != b.bounds, UiAccessibilityProperty::Bounds);
            mark(!std::ranges::equal(before.Relations(a), after.Relations(b),
                                     [](const auto &x, const auto &y) {
                return x.kind == y.kind && x.target == y.target;
            }),
                 UiAccessibilityProperty::Relations);
            mark(!std::ranges::equal(before.Actions(a), after.Actions(b),
                                     [&](const auto &x, const auto &y) {
                return x.id == y.id && x.kind == y.kind && x.argumentKind == y.argumentKind && EqualText(before, x.name, after, y.name);
            }),
                 UiAccessibilityProperty::Actions);
            return result;
        }

        /** @brief Reads focus from the authoritative immutable semantic state. */
        UiAccessibilityNodeId Focus(const UiAccessibilitySnapshot &snapshot) noexcept {
            for (const auto &node : snapshot.Nodes())
                if (node.state.Has(UiAccessibilityStateFlag::Focused))
                    return node.id;
            return {};
        }

        /** @brief One bounded deduplication record, fenced by exact node generation. */
        struct AnnouncementHistory final {
            UiAccessibilityAnnouncementId id;
            UiAccessibilityNodeId node;
            std::uint64_t revision{};
        };

        /** @brief Rejects regressions while permitting a newer document to reset runtime revisions. */
        bool StaleRevisions(const UiAccessibilitySnapshotDescriptor &source, const UiAccessibilitySnapshotDescriptor &prior) noexcept {
            using enum UiRevisionRelation;
            if (source.semanticRevision.Compare(prior.semanticRevision) != Newer ||
                source.documentRevision.Compare(prior.documentRevision) == Older)
                return true;
            return source.documentRevision == prior.documentRevision &&
                   (source.treeRevision.Compare(prior.treeRevision) == Older ||
                    source.interactionRevision.Compare(prior.interactionRevision) == Older);
        }

        /** @brief Validates a live-region declaration and its exact active-node admission. */
        Result<void> ValidateAnnouncement(const UiAccessibilitySnapshot &snapshot, const UiAccessibilityAnnouncementInput &announcement) {
            if (announcement.id.value == 0 || announcement.policy > UiAccessibilityAnnouncementPolicy::Assertive ||
                announcement.text.source > UiAccessibilityTextSource::UserContent || announcement.text.text.empty() ||
                !IsValidUtf8ScalarSequence(announcement.text.text))
                return Failure<void>(UiErrors::AccessibilitySchemaInvalid);
            const auto node = snapshot.Get(announcement.node);
            if (node.HasError())
                return Failure<void>(UiErrors::AccessibilitySnapshotSourceStale);
            if ((node.Value().exposure != UiAccessibilityExposure::Visible &&
                 node.Value().exposure != UiAccessibilityExposure::Offscreen) ||
                node.Value().state.Has(UiAccessibilityStateFlag::Disabled))
                return Failure<void>(UiErrors::AccessibilityActionRejected);
            return Result<void>::Success();
        }
    }  // namespace

    struct UiAccessibilityChangePublisher::Storage final {
        UiAccessibilityExtractorDescriptor owner;
        UiAccessibilityChangeLimits limits;
        std::optional<UiAccessibilitySnapshot> baseline;
        std::vector<NodeIndex> oldIndex;
        std::vector<NodeIndex> newIndex;
        std::vector<UiAccessibilityChange> changes;
        std::vector<char> text;
        std::vector<AnnouncementHistory> history;
        std::vector<AnnouncementHistory> candidateHistory;
        std::vector<std::uint8_t> admitted;
        UiAccessibilitySemanticRevision previousRevision;
        UiAccessibilitySemanticRevision revision;
        UiAccessibilityChangeStatus status{UiAccessibilityChangeStatus::Complete};
        bool retired{};

        Storage(const UiAccessibilityExtractorDescriptor &descriptor, const UiAccessibilityChangeLimits budgets)
            : owner(descriptor), limits(budgets) {
            oldIndex.reserve(owner.limits.nodes);
            newIndex.reserve(owner.limits.nodes);
            changes.reserve(limits.changes);
            text.reserve(limits.announcementTextBytes);
            history.reserve(limits.announcements);
            candidateHistory.reserve(limits.announcements);
            admitted.reserve(limits.announcements);
        }

        /** @brief Validates exact lineage before any baseline or output changes. */
        Result<void> ValidateSource(const UiAccessibilitySnapshot &snapshot) const {
            if (!snapshot.IsValid())
                return Failure<void>(UiErrors::AccessibilityLifecycleUnavailable);
            const auto &source = snapshot.Descriptor();
            if (source.instance != owner.instance || source.canvas != owner.canvas || source.document != owner.document ||
                snapshot.Nodes().size() > owner.limits.nodes)
                return Failure<void>(UiErrors::AccessibilitySnapshotSourceStale);
            if (baseline && StaleRevisions(source, baseline->Descriptor()))
                return Failure<void>(UiErrors::AccessibilitySnapshotSourceStale);
            return Result<void>::Success();
        }

        /** @brief Reserves a new live-region identity without evicting an unexpired event. */
        Result<bool> Remember(const UiAccessibilityAnnouncementInput &announcement, const std::uint64_t currentRevision) {
            if (const bool duplicate = std::ranges::any_of(candidateHistory,
                                                           [&](const auto &record) {
                return record.id == announcement.id && record.node == announcement.node;
            });
                duplicate || announcement.policy == UiAccessibilityAnnouncementPolicy::Off)
                return Result<bool>::Success(false);
            if (candidateHistory.size() == limits.announcements)
                return Failure<bool>(UiErrors::CapacityExceeded);
            candidateHistory.emplace_back(announcement.id, announcement.node, currentRevision);
            return Result<bool>::Success(true);
        }

        /** @brief Prepares bounded announcement history transactionally; never evicts an unexpired identity. */
        Result<void> PrepareAnnouncements(const UiAccessibilitySnapshot &snapshot,
                                          const std::span<const UiAccessibilityAnnouncementInput> announcements) {
            if (announcements.size() > limits.announcements)
                return Failure<void>(UiErrors::CapacityExceeded);
            const auto currentRevision = snapshot.Descriptor().semanticRevision.Value();
            candidateHistory.clear();
            for (const auto &record : history)
                if (currentRevision - record.revision <= limits.deduplicationRevisions)
                    candidateHistory.push_back(record);
            admitted.clear();
            std::size_t bytes{};
            for (const auto &announcement : announcements) {
                if (announcement.text.text.size() > limits.announcementTextBytes - bytes)
                    return Failure<void>(UiErrors::CapacityExceeded);
                bytes += announcement.text.text.size();
                if (const auto validation = ValidateAnnouncement(snapshot, announcement); validation.HasError())
                    return validation;
                if (std::ranges::any_of(announcements.first(admitted.size()), [&](const auto &previous) {
                    return previous.id == announcement.id;
                }))
                    return Failure<void>(UiErrors::AccessibilitySchemaInvalid);
                const auto remembered = Remember(announcement, currentRevision);
                if (remembered.HasError())
                    return Result<void>::Failure(remembered.ErrorValue());
                admitted.push_back(static_cast<std::uint8_t>(remembered.Value()));
            }
            return Result<void>::Success();
        }

        /** @brief Saturates output into one explicit resync instead of exposing a partial delta or allocating. */
        void Append(const UiAccessibilityChange &change) noexcept {
            if (status == UiAccessibilityChangeStatus::Resynchronize)
                return;
            if (changes.size() == limits.changes) {
                changes.clear();
                text.clear();
                changes.push_back({.kind = UiAccessibilityChangeKind::Resynchronize});
                status = UiAccessibilityChangeStatus::Resynchronize;
                return;
            }
            changes.push_back(change);
        }

        /** @brief Emits child-before-parent removals, including replaced generations. */
        void Removals() noexcept {
            if (!baseline)
                return;
            for (const auto &node : baseline->Nodes() | std::views::reverse) {
                const auto found = Find(newIndex, node.id);
                if (found == newIndex.end() || found->id != node.id)
                    Append({.kind = UiAccessibilityChangeKind::Removed, .node = node.id});
            }
        }

        /** @brief Emits canonical additions, reordered/reparented nodes and coalesced property updates. */
        void Updates(const UiAccessibilitySnapshot &snapshot) noexcept {
            for (const auto &node : snapshot.Nodes()) {
                const auto &next = *Find(newIndex, node.id);
                const auto found = Find(oldIndex, node.id);
                if (found == oldIndex.end() || found->id != node.id) {
                    Append({.kind = UiAccessibilityChangeKind::Inserted,
                            .node = node.id,
                            .previous = next.previousSibling,
                            .parent = node.parent});
                    continue;
                }
                const auto &old = baseline->Nodes()[found->position];
                if (node.parent != old.parent || next.previousSibling != found->previousSibling)
                    Append({.kind = UiAccessibilityChangeKind::Structure,
                            .node = node.id,
                            .previous = next.previousSibling,
                            .parent = node.parent});
                const auto properties = Properties(*baseline, old, snapshot, node);
                if (properties != 0)
                    Append({.kind = UiAccessibilityChangeKind::Properties, .node = node.id, .properties = properties});
            }
            const auto before = baseline ? Focus(*baseline) : UiAccessibilityNodeId{};
            const auto after = Focus(snapshot);
            if (before != after)
                Append({.kind = UiAccessibilityChangeKind::Focus, .node = after, .previous = before});
        }

        /** @brief Copies only admitted announcement text, suppressing announcements when a full resync is required. */
        void Announcements(const std::span<const UiAccessibilityAnnouncementInput> announcements) noexcept {
            for (std::size_t index = 0; index < announcements.size(); ++index) {
                if (admitted[index] == 0 || status == UiAccessibilityChangeStatus::Resynchronize)
                    continue;
                const auto &announcement = announcements[index];
                const auto offset = static_cast<std::uint32_t>(text.size());
                text.insert(text.end(), announcement.text.text.begin(), announcement.text.text.end());
                Append({.kind = UiAccessibilityChangeKind::Announcement,
                        .node = announcement.node,
                        .announcement = announcement.id,
                        .policy = announcement.policy,
                        .text = {offset, static_cast<std::uint32_t>(announcement.text.text.size()), announcement.text.source}});
            }
        }
    };

    /** @copydoc UiAccessibilityChangeLimits::IsValid */
    bool UiAccessibilityChangeLimits::IsValid() const noexcept {
        return changes != 0 && changes <= MaximumUiAccessibilityChanges && announcements <= MaximumUiAccessibilityAnnouncements &&
               announcementTextBytes <= MaximumUiAccessibilityTextBytes;
    }

    /** @copydoc UiAccessibilityChangePublisher::Create */
    Result<UiAccessibilityChangePublisher> UiAccessibilityChangePublisher::Create(const UiAccessibilityExtractorDescriptor &owner,
                                                                                  const UiAccessibilityChangeLimits limits) {
        if (!owner.IsValid() || !limits.IsValid())
            return Failure<UiAccessibilityChangePublisher>(UiErrors::AccessibilitySnapshotInvalid);
        try {
            return Result<UiAccessibilityChangePublisher>::Success(
                UiAccessibilityChangePublisher{std::make_unique<Storage>(owner, limits)});
        } catch (const std::bad_alloc &) {
            return Failure<UiAccessibilityChangePublisher>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiAccessibilityChangePublisher::UiAccessibilityChangePublisher */
    UiAccessibilityChangePublisher::UiAccessibilityChangePublisher(std::unique_ptr<Storage> storage) noexcept
        : storage_(std::move(storage)) {}

    /** @copydoc UiAccessibilityChangePublisher::~UiAccessibilityChangePublisher */
    UiAccessibilityChangePublisher::~UiAccessibilityChangePublisher() = default;
    /** @copydoc UiAccessibilityChangePublisher::UiAccessibilityChangePublisher */
    UiAccessibilityChangePublisher::UiAccessibilityChangePublisher(UiAccessibilityChangePublisher &&other) noexcept = default;
    /** @copydoc UiAccessibilityChangePublisher::operator= */
    UiAccessibilityChangePublisher &UiAccessibilityChangePublisher::operator=(UiAccessibilityChangePublisher &&other) noexcept = default;

    /** @copydoc UiAccessibilityChangePublisher::Publish */
    Result<void> UiAccessibilityChangePublisher::Publish(const UiAccessibilitySnapshot &snapshot,
                                                         const std::span<const UiAccessibilityAnnouncementInput> announcements) {
        if (!storage_ || storage_->retired)
            return Failure<void>(UiErrors::AccessibilityLifecycleUnavailable);
        if (const auto source = storage_->ValidateSource(snapshot); source.HasError())
            return source;
        if (const auto validation = storage_->PrepareAnnouncements(snapshot, announcements); validation.HasError())
            return validation;
        if (storage_->baseline)
            BuildIndex(*storage_->baseline, storage_->oldIndex);
        BuildIndex(snapshot, storage_->newIndex);
        storage_->changes.clear();
        storage_->text.clear();
        storage_->status = UiAccessibilityChangeStatus::Complete;
        storage_->Removals();
        storage_->Updates(snapshot);
        storage_->Announcements(announcements);
        storage_->history.swap(storage_->candidateHistory);
        storage_->previousRevision = storage_->revision;
        storage_->revision = snapshot.Descriptor().semanticRevision;
        storage_->baseline = snapshot;
        return Result<void>::Success();
    }

    /** @copydoc UiAccessibilityChangePublisher::Retire */
    void UiAccessibilityChangePublisher::Retire() noexcept {
        if (!storage_ || storage_->retired)
            return;
        storage_->changes.clear();
        storage_->text.clear();
        storage_->newIndex.clear();
        storage_->status = UiAccessibilityChangeStatus::Complete;
        storage_->Removals();
        if (storage_->baseline) {
            const auto focus = Focus(*storage_->baseline);
            if (focus.IsValid())
                storage_->Append({.kind = UiAccessibilityChangeKind::Focus, .previous = focus});
        }
        storage_->previousRevision = storage_->revision;
        storage_->baseline.reset();
        storage_->history.clear();
        storage_->candidateHistory.clear();
        storage_->retired = true;
        if (storage_->status != UiAccessibilityChangeStatus::Resynchronize)
            storage_->status = UiAccessibilityChangeStatus::Retired;
    }

    /** @copydoc UiAccessibilityChangePublisher::IsRetired */
    bool UiAccessibilityChangePublisher::IsRetired() const noexcept {
        return !storage_ || storage_->retired;
    }

    /** @copydoc UiAccessibilityChangePublisher::Status */
    UiAccessibilityChangeStatus UiAccessibilityChangePublisher::Status() const noexcept {
        return storage_ ? storage_->status : UiAccessibilityChangeStatus::Retired;
    }

    /** @copydoc UiAccessibilityChangePublisher::PreviousRevision */
    UiAccessibilitySemanticRevision UiAccessibilityChangePublisher::PreviousRevision() const noexcept {
        return storage_ ? storage_->previousRevision : UiAccessibilitySemanticRevision{};
    }

    /** @copydoc UiAccessibilityChangePublisher::Revision */
    UiAccessibilitySemanticRevision UiAccessibilityChangePublisher::Revision() const noexcept {
        return storage_ ? storage_->revision : UiAccessibilitySemanticRevision{};
    }

    /** @copydoc UiAccessibilityChangePublisher::Changes */
    std::span<const UiAccessibilityChange> UiAccessibilityChangePublisher::Changes() const noexcept {
        return storage_ ? std::span<const UiAccessibilityChange>{storage_->changes} : std::span<const UiAccessibilityChange>{};
    }

    /** @copydoc UiAccessibilityChangePublisher::Text */
    std::string_view UiAccessibilityChangePublisher::Text(const UiAccessibilityTextRef text) const noexcept {
        if (!storage_ || !text.IsPresent() || text.offset > storage_->text.size() || text.size > storage_->text.size() - text.offset)
            return {};
        return {storage_->text.data() + text.offset, text.size};
    }
}  // namespace Horo::Runtime::Ui
