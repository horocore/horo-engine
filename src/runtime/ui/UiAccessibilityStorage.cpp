#include "UiAccessibilityStorage.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] UiAccessibilityNodeId MakeNodeId(const UiElementHandle handle) noexcept {
            return {handle.ownership, handle.slot, handle.generation};
        }

        [[nodiscard]] UiAccessibilityTextRef CopyText(std::vector<char> &destination, const UiAccessibilityTextInput source) {
            if (source.text.empty())
                return {};
            const auto offset = static_cast<std::uint32_t>(destination.size());
            destination.insert(destination.end(), source.text.begin(), source.text.end());
            return {offset, static_cast<std::uint32_t>(source.text.size()), source.source};
        }

        [[nodiscard]] UiAccessibilityValue CopyValue(std::vector<char> &text, const UiAccessibilityValueInput source) {
            UiAccessibilityValue value;
            value.kind = source.kind;
            value.boolean = source.boolean;
            value.integer = source.integer;
            value.number = source.number;
            if (source.kind == UiAccessibilityValueKind::Text)
                value.text = CopyText(text, source.text);
            return value;
        }

        void AppendPublishedNode(const UiElementTree &tree, std::vector<UiAccessibilityNode> &nodes,
                                 std::vector<UiAccessibilityRelation> &relations, std::vector<UiAccessibilityAction> &actions,
                                 std::vector<char> &text, const UiAccessibilityNodeInput &input) {
            const auto element = tree.Find(input.element).Value();
            UiAccessibilityNode node;
            node.id = MakeNodeId(element);
            node.element = element;
            node.elementId = input.element;
            node.role = input.role;
            node.source = input.source;
            node.contributor = input.contributor;
            node.name = CopyText(text, input.name);
            node.description = CopyText(text, input.description);
            node.value = CopyValue(text, input.value);
            node.state = input.state;
            node.hasRange = input.hasRange;
            node.range = input.range;
            node.hasSelection = input.hasSelection;
            node.selection = input.selection;
            node.error.kind = input.error.kind;
            node.error.message = CopyText(text, input.error.message);
            node.exposure = input.exposure;
            node.bounds = input.bounds;
            node.firstRelation = static_cast<std::uint32_t>(relations.size());
            for (const auto &relation : input.relations) {
                const auto target = tree.Find(relation.target).Value();
                relations.push_back({relation.kind, MakeNodeId(target)});
            }
            node.relationCount = static_cast<std::uint32_t>(input.relations.size());
            node.firstAction = static_cast<std::uint32_t>(actions.size());
            for (const auto &action : input.actions)
                actions.push_back({action.id, action.kind, action.argumentKind, CopyText(text, action.name)});
            node.actionCount = static_cast<std::uint32_t>(input.actions.size());
            nodes.push_back(node);
        }
    }  // namespace

    namespace AccessibilityInternal {
        std::size_t FindProjectionIndex(const ProjectionLookup lookup, const UiElementId element) noexcept {
            const auto found = std::lower_bound(lookup.begin(), lookup.end(), element,
                                                [](const ProjectionLookupEntry &entry, const UiElementId candidate) {
                return entry.element < candidate;
            });
            return found != lookup.end() && found->element == element ? found->index : std::numeric_limits<std::size_t>::max();
        }
    }  // namespace AccessibilityInternal

    UiAccessibilitySnapshot::Storage::Storage(const UiAccessibilityLimits &limits) {
        nodes.reserve(limits.nodes);
        relations.reserve(limits.relations);
        actions.reserve(limits.actions);
        text.reserve(limits.textBytes);
    }

    void UiAccessibilitySnapshot::Storage::ResolveParents(const UiElementTree &tree, const AccessibilityInternal::ProjectionLookup lookup) {
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            auto parent = tree.Get(nodes[index].element).Value().parent;
            while (parent.IsValid()) {
                const auto parentRecord = tree.Get(parent).Value();
                const auto parentIndex = AccessibilityInternal::FindProjectionIndex(lookup, parentRecord.id);
                if (parentIndex != std::numeric_limits<std::size_t>::max()) {
                    nodes[index].parent = nodes[parentIndex].id;
                    break;
                }
                parent = parentRecord.parent;
            }
        }
    }

    void UiAccessibilitySnapshot::Storage::Publish(const UiElementTree &tree, const UiAccessibilitySnapshotDescriptor &sourceDescriptor,
                                                   const UiAccessibilityProjection &projection,
                                                   const AccessibilityInternal::ProjectionLookup lookup) {
        descriptor = sourceDescriptor;
        nodes.clear();
        relations.clear();
        actions.clear();
        text.clear();
        for (const auto &input : projection.nodes)
            AppendPublishedNode(tree, nodes, relations, actions, text, input);
        ResolveParents(tree, lookup);
    }

    UiAccessibilityExtractor::Storage::Storage(const UiAccessibilityExtractorDescriptor &source)
        : descriptor(source), cycleScratch(source.limits.nodes) {
        slots.reserve(source.concurrentSnapshots);
        lookupScratch.reserve(source.limits.nodes);
        for (std::uint32_t index = 0; index < source.concurrentSnapshots; ++index)
            slots.push_back(std::make_shared<UiAccessibilitySnapshot::Storage>(source.limits));
    }

    std::shared_ptr<UiAccessibilitySnapshot::Storage> UiAccessibilityExtractor::Storage::TryAcquire() noexcept {
        for (std::size_t offset = 0; offset < slots.size(); ++offset) {
            const auto index = (nextSlot + offset) % slots.size();
            std::uint64_t expected{};
            if (slots[index]->leases.compare_exchange_strong(expected, 1)) {
                nextSlot = (index + 1) % slots.size();
                return slots[index];
            }
        }
        return {};
    }

    bool UiAccessibilityExtractor::Storage::IsDrained() const noexcept {
        for (const auto &slot : slots)
            if (slot->leases.load() != 0)
                return false;
        return true;
    }

    /** @copydoc UiAccessibilitySnapshot::UiAccessibilitySnapshot(std::shared_ptr<const Storage>) */
    UiAccessibilitySnapshot::UiAccessibilitySnapshot(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiAccessibilitySnapshot::~UiAccessibilitySnapshot */
    UiAccessibilitySnapshot::~UiAccessibilitySnapshot() {
        Release();
    }

    /** @copydoc UiAccessibilitySnapshot::UiAccessibilitySnapshot(const UiAccessibilitySnapshot &) */
    UiAccessibilitySnapshot::UiAccessibilitySnapshot(const UiAccessibilitySnapshot &other) noexcept : storage_(other.storage_) {
        Retain();
    }

    /** @copydoc UiAccessibilitySnapshot::operator=(const UiAccessibilitySnapshot &) */
    UiAccessibilitySnapshot &UiAccessibilitySnapshot::operator=(const UiAccessibilitySnapshot &other) noexcept {
        if (this != &other) {
            UiAccessibilitySnapshot replacement{other};
            *this = std::move(replacement);
        }
        return *this;
    }

    /** @copydoc UiAccessibilitySnapshot::UiAccessibilitySnapshot(UiAccessibilitySnapshot &&) */
    UiAccessibilitySnapshot::UiAccessibilitySnapshot(UiAccessibilitySnapshot &&other) noexcept : storage_(std::move(other.storage_)) {}

    /** @copydoc UiAccessibilitySnapshot::operator=(UiAccessibilitySnapshot &&) */
    UiAccessibilitySnapshot &UiAccessibilitySnapshot::operator=(UiAccessibilitySnapshot &&other) noexcept {
        if (this != &other) {
            Release();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiAccessibilitySnapshot::Retain */
    void UiAccessibilitySnapshot::Retain() const noexcept {
        if (!storage_)
            return;
        auto current = storage_->leases.load();
        while (current != std::numeric_limits<std::uint64_t>::max()) {
            if (storage_->leases.compare_exchange_weak(current, current + 1))
                return;
        }
        std::terminate();
    }

    /** @copydoc UiAccessibilitySnapshot::Release */
    void UiAccessibilitySnapshot::Release() noexcept {
        if (!storage_)
            return;
        storage_->leases.fetch_sub(1);
        storage_.reset();
    }

    /** @copydoc UiAccessibilitySnapshot::Descriptor */
    const UiAccessibilitySnapshotDescriptor &UiAccessibilitySnapshot::Descriptor() const noexcept {
        return storage_->descriptor;
    }

    /** @copydoc UiAccessibilitySnapshot::Nodes */
    std::span<const UiAccessibilityNode> UiAccessibilitySnapshot::Nodes() const noexcept {
        return storage_->nodes;
    }

    /** @copydoc UiAccessibilitySnapshot::Relations */
    std::span<const UiAccessibilityRelation> UiAccessibilitySnapshot::Relations() const noexcept {
        return storage_->relations;
    }

    /** @copydoc UiAccessibilitySnapshot::Actions */
    std::span<const UiAccessibilityAction> UiAccessibilitySnapshot::Actions() const noexcept {
        return storage_->actions;
    }

    /** @copydoc UiAccessibilitySnapshot::Text */
    std::string_view UiAccessibilitySnapshot::Text(const UiAccessibilityTextRef text) const noexcept {
        if (!text.IsPresent() || text.offset > storage_->text.size() || text.size > storage_->text.size() - text.offset)
            return {};
        return {storage_->text.data() + text.offset, text.size};
    }

    /** @copydoc UiAccessibilitySnapshot::Find */
    Result<UiAccessibilityNodeId> UiAccessibilitySnapshot::Find(const UiElementId element) const {
        if (!element.IsValid())
            return Failure<UiAccessibilityNodeId>(UiErrors::HandleMalformed);
        for (const auto &node : storage_->nodes)
            if (node.elementId == element)
                return Result<UiAccessibilityNodeId>::Success(node.id);
        return Failure<UiAccessibilityNodeId>(UiErrors::HandleStale);
    }

    /** @copydoc UiAccessibilitySnapshot::Get */
    Result<UiAccessibilityNode> UiAccessibilitySnapshot::Get(const UiAccessibilityNodeId node) const {
        if (!node.IsValid())
            return Failure<UiAccessibilityNode>(UiErrors::HandleMalformed);
        if (node.ownership != storage_->descriptor.instance.ownership)
            return Failure<UiAccessibilityNode>(UiErrors::HandleOwnerMismatch);
        for (const auto &record : storage_->nodes)
            if (record.id == node)
                return Result<UiAccessibilityNode>::Success(record);
        return Failure<UiAccessibilityNode>(UiErrors::HandleStale);
    }

    /** @copydoc UiAccessibilitySnapshot::Actions(const UiAccessibilityNode &) */
    std::span<const UiAccessibilityAction> UiAccessibilitySnapshot::Actions(const UiAccessibilityNode &node) const noexcept {
        if (node.firstAction > storage_->actions.size() || node.actionCount > storage_->actions.size() - node.firstAction)
            return {};
        return {storage_->actions.data() + node.firstAction, node.actionCount};
    }

    /** @copydoc UiAccessibilitySnapshot::Relations(const UiAccessibilityNode &) */
    std::span<const UiAccessibilityRelation> UiAccessibilitySnapshot::Relations(const UiAccessibilityNode &node) const noexcept {
        if (node.firstRelation > storage_->relations.size() || node.relationCount > storage_->relations.size() - node.firstRelation)
            return {};
        return {storage_->relations.data() + node.firstRelation, node.relationCount};
    }

    /** @copydoc UiAccessibilityExtractor::Create */
    Result<UiAccessibilityExtractor> UiAccessibilityExtractor::Create(const UiAccessibilityExtractorDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return Failure<UiAccessibilityExtractor>(UiErrors::AccessibilitySnapshotInvalid);
        try {
            return Result<UiAccessibilityExtractor>::Success(UiAccessibilityExtractor{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiAccessibilityExtractor>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiAccessibilityExtractor::UiAccessibilityExtractor(std::unique_ptr<Storage>) */
    UiAccessibilityExtractor::UiAccessibilityExtractor(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiAccessibilityExtractor::~UiAccessibilityExtractor */
    UiAccessibilityExtractor::~UiAccessibilityExtractor() {
        Close();
    }

    /** @copydoc UiAccessibilityExtractor::UiAccessibilityExtractor(UiAccessibilityExtractor &&) */
    UiAccessibilityExtractor::UiAccessibilityExtractor(UiAccessibilityExtractor &&) noexcept = default;

    /** @copydoc UiAccessibilityExtractor::operator=(UiAccessibilityExtractor &&) */
    UiAccessibilityExtractor &UiAccessibilityExtractor::operator=(UiAccessibilityExtractor &&) noexcept = default;

    /** @copydoc UiAccessibilityExtractor::Close */
    void UiAccessibilityExtractor::Close() noexcept {
        if (storage_)
            storage_->lifecycle = UiAccessibilityExtractorState::Closed;
    }

    /** @copydoc UiAccessibilityExtractor::IsDrained */
    bool UiAccessibilityExtractor::IsDrained() const noexcept {
        return !storage_ || storage_->IsDrained();
    }

    /** @copydoc UiAccessibilityExtractor::State */
    UiAccessibilityExtractorState UiAccessibilityExtractor::State() const noexcept {
        return storage_ ? storage_->lifecycle : UiAccessibilityExtractorState::Closed;
    }
}  // namespace Horo::Runtime::Ui
