#include "UiStyleResolverState.h"

#include <algorithm>
#include <limits>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::Runtime::Ui {
    /** @copydoc UiStyleResolver::Create */
    Result<UiStyleResolver> UiStyleResolver::Create(const UiStyleResolverDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return StyleInternal::Failure<UiStyleResolver>(UiErrors::StyleInvalid);
        try {
            return Result<UiStyleResolver>::Success(UiStyleResolver{std::make_shared<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return StyleInternal::Failure<UiStyleResolver>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiStyleResolver::UiStyleResolver */
    UiStyleResolver::UiStyleResolver(std::shared_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiStyleResolver::~UiStyleResolver */
    UiStyleResolver::~UiStyleResolver() {
        Shutdown();
    }

    /** @copydoc UiStyleResolver::UiStyleResolver */
    UiStyleResolver::UiStyleResolver(UiStyleResolver &&) noexcept = default;

    /** @copydoc UiStyleResolver::operator= */
    UiStyleResolver &UiStyleResolver::operator=(UiStyleResolver &&other) noexcept {
        if (this != &other) {
            Shutdown();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiStyleResolver::Invalidate */
    Result<void> UiStyleResolver::Invalidate(const UiStyleInvalidation &invalidation) const {
        if (!storage_ || storage_->lifecycle != UiStyleResolverState::Active)
            return StyleInternal::Failure(UiErrors::StyleLifecycleUnavailable);
        if (storage_->prepared)
            return StyleInternal::Failure(UiErrors::StyleCandidateBusy);
        if (!invalidation.tree.IsValid())
            return StyleInternal::Failure(UiErrors::StyleInvalid);
        if (invalidation.kind != UiStyleInvalidationKind::All && !invalidation.element.IsValid())
            return StyleInternal::Failure(UiErrors::StyleInvalid);
        if (invalidation.kind == UiStyleInvalidationKind::All) {
            storage_->invalidations.clear();
            storage_->invalidations.push_back(invalidation);
            return Result<void>::Success();
        }
        const auto existing = std::find_if(storage_->invalidations.begin(), storage_->invalidations.end(),
                                           [&invalidation](const UiStyleInvalidation &queued) {
            return queued.tree == invalidation.tree && queued.element == invalidation.element;
        });
        if (existing != storage_->invalidations.end()) {
            const auto strength = [](const UiStyleInvalidationKind kind) {
                switch (kind) {
                    case UiStyleInvalidationKind::Paint:
                        return 0;
                    case UiStyleInvalidationKind::Measure:
                        return 1;
                    case UiStyleInvalidationKind::Subtree:
                        return 2;
                    case UiStyleInvalidationKind::All:
                        return 3;
                }
                return 0;
            };
            if (strength(invalidation.kind) > strength(existing->kind))
                existing->kind = invalidation.kind;
            return Result<void>::Success();
        }
        if (storage_->invalidations.size() == storage_->descriptor.invalidationCapacity)
            return StyleInternal::Failure(UiErrors::CapacityExceeded);
        storage_->invalidations.push_back(invalidation);
        return Result<void>::Success();
    }

    /** @copydoc UiStyleResolver::Prepare */
    Result<UiStyleResolver::PreparedUpdate> UiStyleResolver::Prepare(const UiElementTree &tree, const RuntimeStyleRegistry &registry,
                                                                     const UiStyleUpdateRequest &request) const {
        if (!storage_ || storage_->lifecycle != UiStyleResolverState::Active)
            return StyleInternal::Failure<PreparedUpdate>(UiErrors::StyleLifecycleUnavailable);
        if (storage_->prepared)
            return StyleInternal::Failure<PreparedUpdate>(UiErrors::StyleCandidateBusy);
        if (const auto valid = storage_->ValidateRequest(tree, registry, request); valid.HasError())
            return Result<PreparedUpdate>::Failure(valid.ErrorValue());

        if (const auto prepared = storage_->PrepareCandidate(tree, request); prepared.HasError())
            return Result<PreparedUpdate>::Failure(prepared.ErrorValue());
        const bool sourcesChanged = !storage_->hasPublication || storage_->sources != request.sources;
        const bool anyDirty = std::ranges::any_of(storage_->candidateNodes, [](const Storage::Node &node) {
            return node.dirty;
        });
        if (!sourcesChanged && !anyDirty && storage_->invalidations.empty()) {
            storage_->current->leases.fetch_add(1);
            storage_->prepared = true;
            return Result<PreparedUpdate>::Success(
                PreparedUpdate{storage_, UiComputedStyleSnapshot{storage_->current}, false, tree, registry});
        }
        if (const auto resolved = storage_->ResolveCandidate(registry, request); resolved.HasError())
            return Result<PreparedUpdate>::Failure(resolved.ErrorValue());
        auto published = storage_->BuildSnapshot(registry, request);
        if (published.HasError())
            return Result<PreparedUpdate>::Failure(published.ErrorValue());
        storage_->prepared = true;
        return Result<PreparedUpdate>::Success(
            PreparedUpdate{storage_, UiComputedStyleSnapshot{std::move(published).Value()}, true, tree, registry});
    }

    /** @copydoc UiStyleResolver::PreparedUpdate::PreparedUpdate */
    UiStyleResolver::PreparedUpdate::PreparedUpdate(std::shared_ptr<Storage> owner, UiComputedStyleSnapshot snapshot, const bool changes,
                                                    const UiElementTree &tree, const RuntimeStyleRegistry &registry) noexcept
        : owner_(std::move(owner)), snapshot_(std::move(snapshot)), changes_(changes), treeIssuer_(tree.IssuerPin()),
          registryOwner_(registry.storage_), root_(snapshot_->Records().front().element) {}

    /** @copydoc UiStyleResolver::PreparedUpdate::~PreparedUpdate */
    UiStyleResolver::PreparedUpdate::~PreparedUpdate() {
        Cancel();
    }

    /** @copydoc UiStyleResolver::PreparedUpdate::PreparedUpdate */
    UiStyleResolver::PreparedUpdate::PreparedUpdate(PreparedUpdate &&other) noexcept
        : owner_(std::move(other.owner_)), snapshot_(std::move(other.snapshot_)), changes_(other.changes_),
          treeIssuer_(std::move(other.treeIssuer_)), registryOwner_(std::move(other.registryOwner_)), root_(other.root_) {}

    /** @copydoc UiStyleResolver::PreparedUpdate::operator= */
    UiStyleResolver::PreparedUpdate &UiStyleResolver::PreparedUpdate::operator=(PreparedUpdate &&other) noexcept {
        if (this != &other) {
            Cancel();
            owner_ = std::move(other.owner_);
            snapshot_ = std::move(other.snapshot_);
            changes_ = other.changes_;
            treeIssuer_ = std::move(other.treeIssuer_);
            registryOwner_ = std::move(other.registryOwner_);
            root_ = other.root_;
        }
        return *this;
    }

    /** @copydoc UiStyleResolver::PreparedUpdate::Abandon */
    void UiStyleResolver::PreparedUpdate::Abandon() noexcept {
        Cancel();
    }

    /** @copydoc UiStyleResolver::PreparedUpdate::Cancel */
    void UiStyleResolver::PreparedUpdate::Cancel() noexcept {
        if (owner_)
            owner_->prepared = false;
        snapshot_.reset();
        treeIssuer_.reset();
        registryOwner_.reset();
        owner_.reset();
    }

    /** @copydoc UiStyleResolver::PreparedUpdate::Candidate */
    const UiComputedStyleSnapshot &UiStyleResolver::PreparedUpdate::Candidate() const noexcept {
        return *snapshot_;
    }

    /** @copydoc UiStyleResolver::PreparedUpdate::CanPublish */
    Result<void> UiStyleResolver::PreparedUpdate::CanPublish(const UiElementTree &tree, const RuntimeStyleRegistry &registry) const {
        if (!owner_ || !snapshot_ || !owner_->prepared || owner_->lifecycle != UiStyleResolverState::Active ||
            registry.State() != RuntimeStyleRegistryState::Active)
            return StyleInternal::Failure(UiErrors::StyleLifecycleUnavailable);
        const auto &candidate = snapshot_->Descriptor();
        if (const auto root = tree.Root();
            root.HasError() || root.Value().handle != root_ || tree.IssuerPin() != treeIssuer_ || registry.storage_ != registryOwner_ ||
            tree.State() != UiElementTreeState::Active || tree.Instance() != candidate.instance || tree.Canvas() != candidate.canvas ||
            tree.SourceDocument() != candidate.document || tree.SourceDocumentRevision() != candidate.sources.document ||
            tree.Revision() != candidate.sources.tree || registry.Generation() != candidate.sources.registry)
            return StyleInternal::Failure(UiErrors::StyleSourceStale);
        return Result<void>::Success();
    }

    /** @copydoc UiStyleResolver::PublishValidated */
    UiComputedStyleSnapshot UiStyleResolver::PublishValidated(PreparedUpdate &&candidate) noexcept {
        if (candidate.changes_) {
            storage_->ReleaseCurrent();
            storage_->current = std::const_pointer_cast<UiComputedStyleSnapshot::Storage>(candidate.snapshot_->storage_);
            storage_->current->leases.fetch_add(1);
            storage_->activeNodes.swap(storage_->candidateNodes);
            storage_->sources = candidate.snapshot_->Descriptor().sources;
            storage_->publication = candidate.snapshot_->Descriptor().publication;
            storage_->geometry = candidate.snapshot_->Descriptor().geometry;
            storage_->hasPublication = true;
            storage_->invalidations.clear();
        }
        auto published = std::move(*candidate.snapshot_);
        candidate.Cancel();
        return published;
    }

    /** @copydoc UiStyleResolver::Commit */
    Result<UiComputedStyleSnapshot> UiStyleResolver::Commit(PreparedUpdate &&candidate, const UiElementTree &tree,
                                                            const RuntimeStyleRegistry &registry) {
        if (candidate.owner_ != storage_)
            return StyleInternal::Failure<UiComputedStyleSnapshot>(UiErrors::StyleSourceStale);
        if (const auto admitted = candidate.CanPublish(tree, registry); admitted.HasError())
            return Result<UiComputedStyleSnapshot>::Failure(admitted.ErrorValue());
        return Result<UiComputedStyleSnapshot>::Success(PublishValidated(std::move(candidate)));
    }

    /** @copydoc UiStyleResolver::Update */
    Result<UiComputedStyleSnapshot> UiStyleResolver::Update(const UiElementTree &tree, const RuntimeStyleRegistry &registry,
                                                            const UiStyleUpdateRequest &request) {
        auto candidate = Prepare(tree, registry, request);
        if (candidate.HasError())
            return Result<UiComputedStyleSnapshot>::Failure(candidate.ErrorValue());
        return Commit(std::move(candidate).Value(), tree, registry);
    }

    /** @copydoc UiStyleResolver::BeginRetirement */
    Result<void> UiStyleResolver::BeginRetirement() {
        if (!storage_ || storage_->lifecycle != UiStyleResolverState::Active)
            return StyleInternal::Failure(UiErrors::StyleLifecycleUnavailable);
        storage_->lifecycle = UiStyleResolverState::Retiring;
        storage_->invalidations.clear();
        return Result<void>::Success();
    }

    /** @copydoc UiStyleResolver::Shutdown */
    void UiStyleResolver::Shutdown() const noexcept {
        if (!storage_ || storage_->lifecycle == UiStyleResolverState::Stopped)
            return;
        storage_->lifecycle = UiStyleResolverState::Stopped;
        storage_->invalidations.clear();
        storage_->activeNodes.clear();
        storage_->candidateNodes.clear();
        storage_->traversalScratch.clear();
        storage_->ReleaseCurrent();
    }

    /** @copydoc UiStyleResolver::State */
    UiStyleResolverState UiStyleResolver::State() const noexcept {
        return storage_ ? storage_->lifecycle : UiStyleResolverState::Stopped;
    }

    /** @copydoc UiStyleResolver::IsDrained */
    bool UiStyleResolver::IsDrained() const noexcept {
        if (!storage_)
            return true;
        return std::ranges::all_of(storage_->slots, [&storage = *storage_](const std::shared_ptr<UiComputedStyleSnapshot::Storage> &slot) {
            const auto leases = slot->leases.load();
            return leases == 0 || (slot == storage.current && leases == 1);
        });
    }
}  // namespace Horo::Runtime::Ui
