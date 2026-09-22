#include "Horo/Runtime/Ui/UiImageResource.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] constexpr bool IsKnown(const UiImageColorSpace value) noexcept {
            return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(UiImageColorSpace::Srgb);
        }

        [[nodiscard]] constexpr bool IsKnown(const UiImageResourceKind value) noexcept {
            return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(UiImageResourceKind::Atlas);
        }

        [[nodiscard]] constexpr bool IsKnown(const UiImageFallbackPolicy value) noexcept {
            return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(UiImageFallbackPolicy::Checkerboard);
        }

        [[nodiscard]] constexpr bool IsKnown(const UiImageResidencyState value) noexcept {
            return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(UiImageResidencyState::Failed);
        }

        [[nodiscard]] bool DependenciesCompatible(const std::span<const UiImagePage> pages) noexcept {
            for (std::size_t index = 0; index < pages.size(); ++index)
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (pages[previous].dependency.asset == pages[index].dependency.asset &&
                        pages[previous].dependency.expectedType != pages[index].dependency.expectedType)
                        return false;
            return true;
        }

        [[nodiscard]] bool Drawable(const UiImageResidencyState residency) noexcept {
            return residency == UiImageResidencyState::Resident || residency == UiImageResidencyState::MissingFallback;
        }
    }  // namespace

    /** @copydoc UiImageUvRect::IsValid */
    bool UiImageUvRect::IsValid() const noexcept {
        return std::isfinite(u0) && std::isfinite(v0) && std::isfinite(u1) && std::isfinite(v1) && u0 >= 0.0F && v0 >= 0.0F && u1 <= 1.0F &&
               v1 <= 1.0F && u0 < u1 && v0 < v1;
    }

    /** @copydoc UiImagePage::IsValid */
    bool UiImagePage::IsValid() const noexcept {
        return dependency.asset.IsValid() && !dependency.expectedType.Value().empty() && extent.IsValid() && IsKnown(colorSpace) &&
               sampling.IsValid();
    }

    /** @copydoc UiSpriteRegion::IsValid */
    bool UiSpriteRegion::IsValid(const UiImagePage &ownerPage) const noexcept {
        return id != NoUiImageRegion && pixelExtent.IsValid() && pixelExtent.width <= ownerPage.extent.width &&
               pixelExtent.height <= ownerPage.extent.height && uv.IsValid() && nineSlice.IsValid(pixelExtent);
    }

    /** @copydoc UiImageResourceDescriptor::IsValid */
    bool UiImageResourceDescriptor::IsValid() const noexcept {
        if (!IsKnown(kind) || !IsKnown(fallback) || pages.data() == nullptr || (!regions.empty() && regions.data() == nullptr) ||
            pages.empty() || pages.size() > MaximumUiImagePages || regions.size() > MaximumUiImageRegions || !DependenciesCompatible(pages))
            return false;
        if (kind == UiImageResourceKind::Image && (!regions.empty() || pages.size() != 1))
            return false;
        if (kind == UiImageResourceKind::Atlas && regions.empty())
            return false;
        for (const auto &page : pages)
            if (!page.IsValid())
                return false;
        for (std::size_t index = 0; index < regions.size(); ++index) {
            const auto &region = regions[index];
            if (region.page >= pages.size() || !region.IsValid(pages[region.page]))
                return false;
            for (std::size_t previous = 0; previous < index; ++previous)
                if (regions[previous].id == region.id)
                    return false;
        }
        return true;
    }

    struct UiImageResource::Storage final {
        UiImageResourceKind kind{UiImageResourceKind::Image};
        UiImageFallbackPolicy fallback{UiImageFallbackPolicy::Reject};
        std::vector<UiImagePage> pages;
        std::vector<UiSpriteRegion> regions;
        std::vector<UiAssetDependency> dependencies;
    };

    /** @copydoc UiImageResource::UiImageResource(std::unique_ptr<Storage>) */
    UiImageResource::UiImageResource(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiImageResource::~UiImageResource */
    UiImageResource::~UiImageResource() = default;

    /** @copydoc UiImageResource::UiImageResource(UiImageResource&&) */
    UiImageResource::UiImageResource(UiImageResource &&) noexcept = default;

    /** @copydoc UiImageResource::operator=(UiImageResource&&) */
    UiImageResource &UiImageResource::operator=(UiImageResource &&) noexcept = default;

    /** @copydoc UiImageResource::Create */
    Result<UiImageResource> UiImageResource::Create(const UiImageResourceDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return Failure<UiImageResource>(UiErrors::ImageResourceInvalid);
        try {
            auto storage = std::make_unique<Storage>();
            storage->kind = descriptor.kind;
            storage->fallback = descriptor.fallback;
            storage->pages.assign(descriptor.pages.begin(), descriptor.pages.end());
            storage->regions.assign(descriptor.regions.begin(), descriptor.regions.end());
            std::ranges::sort(storage->regions, {}, &UiSpriteRegion::id);

            for (const auto &page : storage->pages) {
                auto found = std::ranges::find_if(storage->dependencies, [&](const UiAssetDependency &dependency) {
                    return dependency.asset == page.dependency.asset;
                });
                if (found == storage->dependencies.end()) {
                    if (storage->dependencies.size() >= MaximumUiImageDependencies)
                        return Failure<UiImageResource>(UiErrors::CapacityExceeded);
                    storage->dependencies.push_back(page.dependency);
                } else {
                    if (found->expectedType != page.dependency.expectedType)
                        return Failure<UiImageResource>(UiErrors::DependencyInvalid);
                    found->required = found->required || page.dependency.required;
                }
            }
            std::ranges::sort(storage->dependencies, [](const UiAssetDependency &left, const UiAssetDependency &right) {
                if (left.asset != right.asset)
                    return left.asset < right.asset;
                return left.expectedType < right.expectedType;
            });
            return Result<UiImageResource>::Success(UiImageResource{std::move(storage)});
        } catch (const std::bad_alloc &) {
            return Failure<UiImageResource>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiImageResource::IsValid */
    bool UiImageResource::IsValid() const noexcept {
        return static_cast<bool>(storage_);
    }

    /** @copydoc UiImageResource::Kind */
    UiImageResourceKind UiImageResource::Kind() const noexcept {
        return storage_ ? storage_->kind : UiImageResourceKind::Image;
    }

    /** @copydoc UiImageResource::FallbackPolicy */
    UiImageFallbackPolicy UiImageResource::FallbackPolicy() const noexcept {
        return storage_ ? storage_->fallback : UiImageFallbackPolicy::Reject;
    }

    /** @copydoc UiImageResource::Pages */
    std::span<const UiImagePage> UiImageResource::Pages() const noexcept {
        return storage_ ? std::span<const UiImagePage>{storage_->pages} : std::span<const UiImagePage>{};
    }

    /** @copydoc UiImageResource::Regions */
    std::span<const UiSpriteRegion> UiImageResource::Regions() const noexcept {
        return storage_ ? std::span<const UiSpriteRegion>{storage_->regions} : std::span<const UiSpriteRegion>{};
    }

    /** @copydoc UiImageResource::Dependencies */
    std::span<const UiAssetDependency> UiImageResource::Dependencies() const noexcept {
        return storage_ ? std::span<const UiAssetDependency>{storage_->dependencies} : std::span<const UiAssetDependency>{};
    }

    /** @copydoc UiImageResource::Page */
    const UiImagePage *UiImageResource::Page(const std::uint32_t index) const noexcept {
        return storage_ && index < storage_->pages.size() ? &storage_->pages[index] : nullptr;
    }

    /** @copydoc UiImageResource::FindRegion */
    const UiSpriteRegion *UiImageResource::FindRegion(const std::uint32_t id) const noexcept {
        if (!storage_ || id == NoUiImageRegion)
            return nullptr;
        const auto found = std::ranges::lower_bound(storage_->regions, id, {}, &UiSpriteRegion::id);
        return found != storage_->regions.end() && found->id == id ? std::to_address(found) : nullptr;
    }

    /** @copydoc UiImageResource::ResolveRegion */
    Result<UiResolvedImageRegion> UiImageResource::ResolveRegion(const std::uint32_t id) const {
        if (!storage_)
            return Failure<UiResolvedImageRegion>(UiErrors::ImageResourceInvalid);
        if (id == NoUiImageRegion) {
            if (storage_->kind != UiImageResourceKind::Image)
                return Failure<UiResolvedImageRegion>(UiErrors::ImageRegionInvalid);
            return Result<UiResolvedImageRegion>::Success({0, {}, storage_->pages.front().extent, {}});
        }
        const auto *region = FindRegion(id);
        if (region == nullptr)
            return Failure<UiResolvedImageRegion>(UiErrors::ImageRegionInvalid);
        return Result<UiResolvedImageRegion>::Success({region->page, region->uv, region->pixelExtent, region->nineSlice});
    }

    /** @copydoc UiImageResourceRegistryDescriptor::IsValid */
    bool UiImageResourceRegistryDescriptor::IsValid() const noexcept {
        return owner.IsValid() && maximumResources > 0 && maximumResources <= MaximumUiImageResourceSlots;
    }

    struct UiImageResourceSnapshot::LeaseTracker final {
        std::atomic<std::uint64_t> leases{};
    };

    struct UiImageResourceSnapshot::Generation final {
        UiImageResourceHandle handle;
        UiImageResourceRevision revision;
        UiImageResidencyState residency{UiImageResidencyState::Unresolved};
        UiImageResource resource;

        Generation(const UiImageResourceHandle sourceHandle, const UiImageResourceRevision sourceRevision,
                   const UiImageResidencyState sourceResidency, UiImageResource sourceResource) noexcept
            : handle(sourceHandle), revision(sourceRevision), residency(sourceResidency), resource(std::move(sourceResource)) {}
    };

    /** @copydoc UiImageResourceSnapshot::UiImageResourceSnapshot(std::shared_ptr<const Generation>, std::shared_ptr<LeaseTracker>) */
    UiImageResourceSnapshot::UiImageResourceSnapshot(std::shared_ptr<const Generation> generation,
                                                     std::shared_ptr<LeaseTracker> tracker) noexcept
        : generation_(std::move(generation)), tracker_(std::move(tracker)) {
        Retain();
    }

    /** @copydoc UiImageResourceSnapshot::~UiImageResourceSnapshot */
    UiImageResourceSnapshot::~UiImageResourceSnapshot() {
        Release();
    }

    /** @copydoc UiImageResourceSnapshot::UiImageResourceSnapshot(const UiImageResourceSnapshot&) */
    UiImageResourceSnapshot::UiImageResourceSnapshot(const UiImageResourceSnapshot &other) noexcept
        : generation_(other.generation_), tracker_(other.tracker_) {
        Retain();
    }

    /** @copydoc UiImageResourceSnapshot::operator=(const UiImageResourceSnapshot&) */
    UiImageResourceSnapshot &UiImageResourceSnapshot::operator=(const UiImageResourceSnapshot &other) noexcept {
        if (this != &other) {
            UiImageResourceSnapshot replacement{other};
            *this = std::move(replacement);
        }
        return *this;
    }

    /** @copydoc UiImageResourceSnapshot::UiImageResourceSnapshot(UiImageResourceSnapshot&&) */
    UiImageResourceSnapshot::UiImageResourceSnapshot(UiImageResourceSnapshot &&other) noexcept
        : generation_(std::move(other.generation_)), tracker_(std::move(other.tracker_)) {}

    /** @copydoc UiImageResourceSnapshot::operator=(UiImageResourceSnapshot&&) */
    UiImageResourceSnapshot &UiImageResourceSnapshot::operator=(UiImageResourceSnapshot &&other) noexcept {
        if (this != &other) {
            Release();
            generation_ = std::move(other.generation_);
            tracker_ = std::move(other.tracker_);
        }
        return *this;
    }

    /** @copydoc UiImageResourceSnapshot::Retain */
    void UiImageResourceSnapshot::Retain() const noexcept {
        if (!tracker_)
            return;
        auto current = tracker_->leases.load();
        while (current != std::numeric_limits<std::uint64_t>::max()) {
            if (tracker_->leases.compare_exchange_weak(current, current + 1))
                return;
        }
        std::terminate();
    }

    /** @copydoc UiImageResourceSnapshot::Release */
    void UiImageResourceSnapshot::Release() noexcept {
        if (!tracker_)
            return;
        tracker_->leases.fetch_sub(1);
        generation_.reset();
        tracker_.reset();
    }

    /** @copydoc UiImageResourceSnapshot::IsValid */
    bool UiImageResourceSnapshot::IsValid() const noexcept {
        return static_cast<bool>(generation_);
    }

    /** @copydoc UiImageResourceSnapshot::Handle */
    UiImageResourceHandle UiImageResourceSnapshot::Handle() const noexcept {
        return generation_ ? generation_->handle : UiImageResourceHandle{};
    }

    /** @copydoc UiImageResourceSnapshot::Revision */
    UiImageResourceRevision UiImageResourceSnapshot::Revision() const noexcept {
        return generation_ ? generation_->revision : UiImageResourceRevision{};
    }

    /** @copydoc UiImageResourceSnapshot::Residency */
    UiImageResidencyState UiImageResourceSnapshot::Residency() const noexcept {
        return generation_ ? generation_->residency : UiImageResidencyState::Unresolved;
    }

    /** @copydoc UiImageResourceSnapshot::IsDrawable */
    bool UiImageResourceSnapshot::IsDrawable() const noexcept {
        return generation_ && Drawable(generation_->residency);
    }

    /** @copydoc UiImageResourceSnapshot::Resource */
    const UiImageResource *UiImageResourceSnapshot::Resource() const noexcept {
        return generation_ ? &generation_->resource : nullptr;
    }

    /** @copydoc UiImageResourceSnapshot::Dependencies */
    std::span<const UiAssetDependency> UiImageResourceSnapshot::Dependencies() const noexcept {
        return generation_ ? generation_->resource.Dependencies() : std::span<const UiAssetDependency>{};
    }

    struct UiImageResourceRegistry::Storage final {
        struct Slot final {
            std::uint32_t generation{1};
            std::shared_ptr<const UiImageResourceSnapshot::Generation> current;
        };

        UiImageResourceRegistryDescriptor descriptor;
        UiImageResourceRegistryState lifecycle{UiImageResourceRegistryState::Active};
        std::vector<Slot> slots;
        std::shared_ptr<UiImageResourceSnapshot::LeaseTracker> tracker{std::make_shared<UiImageResourceSnapshot::LeaseTracker>()};
        std::size_t nextSlot{1};

        explicit Storage(const UiImageResourceRegistryDescriptor &source)
            : descriptor(source), slots(static_cast<std::size_t>(source.maximumResources) + 1U) {}

        [[nodiscard]] UiImageResourceHandle Handle(const std::uint32_t slot) const noexcept {
            return {descriptor.owner, slot, slots[slot].generation};
        }

        [[nodiscard]] std::shared_ptr<const UiImageResourceSnapshot::Generation> TryFind(const UiImageResourceHandle &handle) const {
            if (handle.slot == 0 || handle.slot >= slots.size())
                return {};
            const auto &slot = slots[handle.slot];
            if (!slot.current || slot.generation != handle.generation)
                return {};
            return slot.current;
        }
    };

    /** @copydoc UiImageResourceRegistry::Create */
    Result<UiImageResourceRegistry> UiImageResourceRegistry::Create(const UiImageResourceRegistryDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return Failure<UiImageResourceRegistry>(UiErrors::CapacityExceeded);
        try {
            return Result<UiImageResourceRegistry>::Success(UiImageResourceRegistry{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiImageResourceRegistry>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiImageResourceRegistry::UiImageResourceRegistry(std::unique_ptr<Storage>) */
    UiImageResourceRegistry::UiImageResourceRegistry(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiImageResourceRegistry::~UiImageResourceRegistry */
    UiImageResourceRegistry::~UiImageResourceRegistry() {
        Close();
    }

    /** @copydoc UiImageResourceRegistry::UiImageResourceRegistry(UiImageResourceRegistry&&) */
    UiImageResourceRegistry::UiImageResourceRegistry(UiImageResourceRegistry &&) noexcept = default;

    /** @copydoc UiImageResourceRegistry::operator=(UiImageResourceRegistry&&) */
    UiImageResourceRegistry &UiImageResourceRegistry::operator=(UiImageResourceRegistry &&) noexcept = default;

    /** @copydoc UiImageResourceRegistry::Publish */
    Result<UiImageResourceHandle> UiImageResourceRegistry::Publish(UiImageResource resource, const UiImageResourceRevision revision,
                                                                   const UiImageResidencyState residency) {
        if (!storage_ || storage_->lifecycle != UiImageResourceRegistryState::Active)
            return Failure<UiImageResourceHandle>(UiErrors::ImageResourceLifecycleUnavailable);
        if (!resource.IsValid())
            return Failure<UiImageResourceHandle>(UiErrors::ImageResourceInvalid);
        if (!revision.IsValid())
            return Failure<UiImageResourceHandle>(UiErrors::RevisionInvalid);
        if (!IsKnown(residency) ||
            (residency == UiImageResidencyState::MissingFallback && resource.FallbackPolicy() == UiImageFallbackPolicy::Reject))
            return Failure<UiImageResourceHandle>(UiErrors::ImageResidencyInvalid);

        const auto maximum = storage_->slots.size();
        for (std::size_t offset = 0; offset < maximum - 1U; ++offset) {
            const auto index = 1U + ((storage_->nextSlot - 1U + offset) % (maximum - 1U));
            auto &slot = storage_->slots[index];
            if (slot.current || slot.generation == std::numeric_limits<std::uint32_t>::max())
                continue;
            const auto handle = storage_->Handle(static_cast<std::uint32_t>(index));
            try {
                auto generation = std::make_shared<UiImageResourceSnapshot::Generation>(handle, revision, residency, std::move(resource));
                slot.current = std::move(generation);
            } catch (const std::bad_alloc &) {
                return Failure<UiImageResourceHandle>(UiErrors::CapacityExceeded);
            }
            storage_->nextSlot = index == maximum - 1U ? 1U : index + 1U;
            return Result<UiImageResourceHandle>::Success(handle);
        }
        return Failure<UiImageResourceHandle>(UiErrors::ImageResourceStorageExhausted);
    }

    /** @copydoc UiImageResourceRegistry::Reload */
    Result<UiImageResourceHandle> UiImageResourceRegistry::Reload(const UiImageResourceHandle &current, UiImageResource resource,
                                                                  const UiImageResourceRevision revision,
                                                                  const UiImageResidencyState residency) {
        if (!storage_ || storage_->lifecycle != UiImageResourceRegistryState::Active)
            return Failure<UiImageResourceHandle>(UiErrors::ImageResourceLifecycleUnavailable);
        if (const auto owner = ValidateUiHandleOwner(current, storage_->descriptor.owner); owner.HasError())
            return Result<UiImageResourceHandle>::Failure(owner.ErrorValue());
        if (current.slot >= storage_->slots.size())
            return Failure<UiImageResourceHandle>(UiErrors::HandleStale);
        auto &slot = storage_->slots[current.slot];
        if (!slot.current || slot.generation != current.generation)
            return Failure<UiImageResourceHandle>(UiErrors::HandleStale);
        if (!resource.IsValid())
            return Failure<UiImageResourceHandle>(UiErrors::ImageResourceInvalid);
        if (!revision.IsValid())
            return Failure<UiImageResourceHandle>(UiErrors::RevisionInvalid);
        if (!IsKnown(residency) ||
            (residency == UiImageResidencyState::MissingFallback && resource.FallbackPolicy() == UiImageFallbackPolicy::Reject))
            return Failure<UiImageResourceHandle>(UiErrors::ImageResidencyInvalid);
        if (revision.Compare(slot.current->revision) != UiRevisionRelation::Newer)
            return Failure<UiImageResourceHandle>(UiErrors::RevisionStale);
        if (slot.generation == std::numeric_limits<std::uint32_t>::max())
            return Failure<UiImageResourceHandle>(UiErrors::GenerationExhausted);

        const UiImageResourceHandle next{storage_->descriptor.owner, current.slot, slot.generation + 1U};
        try {
            auto generation = std::make_shared<UiImageResourceSnapshot::Generation>(next, revision, residency, std::move(resource));
            slot.current = std::move(generation);
        } catch (const std::bad_alloc &) {
            return Failure<UiImageResourceHandle>(UiErrors::CapacityExceeded);
        }
        ++slot.generation;
        return Result<UiImageResourceHandle>::Success(next);
    }

    /** @copydoc UiImageResourceRegistry::Acquire */
    Result<UiImageResourceSnapshot> UiImageResourceRegistry::Acquire(const UiImageResourceHandle &handle) const {
        if (!storage_ || storage_->lifecycle != UiImageResourceRegistryState::Active)
            return Failure<UiImageResourceSnapshot>(UiErrors::ImageResourceLifecycleUnavailable);
        if (const auto owner = ValidateUiHandleOwner(handle, storage_->descriptor.owner); owner.HasError())
            return Result<UiImageResourceSnapshot>::Failure(owner.ErrorValue());
        const auto generation = storage_->TryFind(handle);
        if (!generation)
            return Failure<UiImageResourceSnapshot>(UiErrors::HandleStale);
        return Result<UiImageResourceSnapshot>::Success(UiImageResourceSnapshot{generation, storage_->tracker});
    }

    /** @copydoc UiImageResourceRegistry::Retire */
    Result<void> UiImageResourceRegistry::Retire(const UiImageResourceHandle &handle) {
        if (!storage_ || storage_->lifecycle != UiImageResourceRegistryState::Active)
            return Failure(UiErrors::ImageResourceLifecycleUnavailable);
        if (const auto owner = ValidateUiHandleOwner(handle, storage_->descriptor.owner); owner.HasError())
            return owner;
        if (handle.slot >= storage_->slots.size())
            return Failure(UiErrors::HandleStale);
        auto &slot = storage_->slots[handle.slot];
        if (!slot.current || slot.generation != handle.generation)
            return Failure(UiErrors::HandleStale);
        if (slot.generation == std::numeric_limits<std::uint32_t>::max())
            return Failure(UiErrors::GenerationExhausted);
        slot.current.reset();
        ++slot.generation;
        return Result<void>::Success();
    }

    /** @copydoc UiImageResourceRegistry::Close */
    void UiImageResourceRegistry::Close() noexcept {
        if (storage_)
            storage_->lifecycle = UiImageResourceRegistryState::Closed;
    }

    /** @copydoc UiImageResourceRegistry::IsDrained */
    bool UiImageResourceRegistry::IsDrained() const noexcept {
        return !storage_ || storage_->tracker->leases.load() == 0;
    }

    /** @copydoc UiImageResourceRegistry::State */
    UiImageResourceRegistryState UiImageResourceRegistry::State() const noexcept {
        return storage_ ? storage_->lifecycle : UiImageResourceRegistryState::Closed;
    }
}  // namespace Horo::Runtime::Ui
