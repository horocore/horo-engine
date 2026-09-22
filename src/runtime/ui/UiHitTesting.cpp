#include "Horo/Runtime/Ui/UiHitTesting.h"

#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiPresentationReceipt.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsKnownMode(const UiRenderMode mode) noexcept {
            return mode >= UiRenderMode::ScreenSpaceOverlay && mode <= UiRenderMode::WorldSpace;
        }

        [[nodiscard]] bool SameOwner(const UiHitTestStoreDescriptor &descriptor) noexcept {
            return descriptor.instance.IsValid() && descriptor.canvas.IsValid() &&
                   descriptor.instance.ownership == descriptor.canvas.ownership;
        }

        [[nodiscard]] bool Contains(const UiLogicalRect &rect, const double x, const double y) noexcept {
            const double left = rect.origin.x;
            const double top = rect.origin.y;
            const double right = left + rect.extent.width;
            const double bottom = top + rect.extent.height;
            return x >= left && x < right && y >= top && y < bottom;
        }

        [[nodiscard]] UiLogicalPoint ToLogicalPoint(const double x, const double y) noexcept {
            return {static_cast<std::int32_t>(std::floor(x)), static_cast<std::int32_t>(std::floor(y))};
        }
    }  // namespace

    struct UiHitTestSnapshot::Storage final {
        struct Record final {
            UiElementHandle element;
            UiLogicalRect hitTest;
            UiLogicalRect clip;
            UiHitSlop hitSlop;
            std::array<double, 6> canvasToLocal{};
            std::int32_t zOrder{};
            std::uint32_t paintOrder{};
            bool hasClip{};
            bool visible{};
            bool enabled{};
        };

        mutable std::atomic<std::uint64_t> leases{};
        UiHitTestSnapshotDescriptor descriptor;
        std::vector<Record> records;

        explicit Storage(const std::uint32_t capacity) {
            records.reserve(capacity);
        }
    };

    struct UiHitTestStore::Storage final {
        UiHitTestStoreDescriptor descriptor;
        UiHitTestStoreState lifecycle{UiHitTestStoreState::Active};
        std::vector<std::shared_ptr<UiHitTestSnapshot::Storage>> slots;
        std::shared_ptr<UiHitTestSnapshot::Storage> current;
        std::size_t nextSlot{};

        explicit Storage(const UiHitTestStoreDescriptor &source) : descriptor(source) {
            slots.reserve(source.concurrentSnapshots);
            for (std::uint32_t index = 0; index < source.concurrentSnapshots; ++index)
                slots.push_back(std::make_shared<UiHitTestSnapshot::Storage>(source.elementCapacity));
        }

        Storage(const Storage &) = delete;
        Storage &operator=(const Storage &) = delete;
        Storage(Storage &&) = delete;
        Storage &operator=(Storage &&) = delete;

        ~Storage() {
            ReleaseCurrent();
        }

        [[nodiscard]] std::shared_ptr<UiHitTestSnapshot::Storage> TryAcquire() noexcept {
            for (std::size_t offset = 0; offset < slots.size(); ++offset) {
                const auto index = (nextSlot + offset) % slots.size();
                if (std::uint64_t expected{}; !slots[index]->leases.compare_exchange_strong(expected, 1))
                    continue;
                nextSlot = (index + 1) % slots.size();
                return slots[index];
            }
            return {};
        }

        void ReleaseCurrent() noexcept {
            if (current) {
                current->leases.fetch_sub(1);
                current.reset();
            }
        }
    };

    namespace {
        [[nodiscard]] double TransformDeterminant(const UiLogicalTransform &transform) noexcept {
            const auto &values = transform.values;
            return static_cast<double>(values[0]) * values[3] - static_cast<double>(values[1]) * values[2];
        }

        template <typename Records>
        [[nodiscard]] bool ArePublishElementsValid(const Records records, const std::span<const UiHitTestElement> elements) noexcept {
            for (std::size_t index = 0; index < elements.size(); ++index) {
                const auto &element = elements[index];
                const double determinant = TransformDeterminant(element.transform);
                if (element.element != records[index].element || !element.transform.IsValid() || !element.hitSlop.IsValid() ||
                    (element.hasClip && !element.clip.IsValid()) || !records[index].arrangement.hitTest.IsValid() ||
                    !std::isfinite(determinant) || std::abs(determinant) <= Math::DefaultEpsilon)
                    return false;
            }
            return true;
        }

        template <typename Records, typename Output>
        void PopulateHitTestRecords(const Records records, const std::span<const UiHitTestElement> elements, Output &output) {
            output.resize(records.size());
            for (std::uint32_t index = 0; index < records.size(); ++index) {
                const auto &sourceElement = elements[index];
                const auto &values = sourceElement.transform.values;
                const double determinant = TransformDeterminant(sourceElement.transform);
                output[index] = {sourceElement.element,
                                 records[index].arrangement.hitTest,
                                 sourceElement.clip,
                                 sourceElement.hitSlop,
                                 {values[3] / determinant, -values[1] / determinant, -values[2] / determinant, values[0] / determinant,
                                  (static_cast<double>(values[1]) * values[5] - static_cast<double>(values[3]) * values[4]) / determinant,
                                  (static_cast<double>(values[2]) * values[4] - static_cast<double>(values[0]) * values[5]) / determinant},
                                 sourceElement.zOrder,
                                 index,
                                 sourceElement.hasClip,
                                 sourceElement.visible,
                                 sourceElement.enabled};
            }
        }
    }  // namespace

    /** @copydoc UiHitSlop::IsValid */
    bool UiHitSlop::IsValid() const noexcept {
        return left >= 0 && top >= 0 && right >= 0 && bottom >= 0;
    }

    /** @copydoc UiHitTestStoreDescriptor::IsValid */
    bool UiHitTestStoreDescriptor::IsValid() const noexcept {
        return SameOwner(*this) && document.IsValid() && IsKnownMode(renderMode) && logicalExtent.width > 0 && logicalExtent.height > 0 &&
               elementCapacity > 0 && elementCapacity <= MaximumUiTreeElements && concurrentSnapshots >= 2 &&
               concurrentSnapshots <= MaximumUiHitTestSnapshotsInFlight;
    }

    /** @copydoc UiHitTestSnapshot::UiHitTestSnapshot */
    UiHitTestSnapshot::UiHitTestSnapshot(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiHitTestSnapshot::~UiHitTestSnapshot */
    UiHitTestSnapshot::~UiHitTestSnapshot() {
        Release();
    }

    /** @copydoc UiHitTestSnapshot::UiHitTestSnapshot */
    UiHitTestSnapshot::UiHitTestSnapshot(const UiHitTestSnapshot &other) noexcept : storage_(other.storage_) {
        Retain();
    }

    /** @copydoc UiHitTestSnapshot::operator= */
    UiHitTestSnapshot &UiHitTestSnapshot::operator=(const UiHitTestSnapshot &other) noexcept {
        if (this != &other) {
            Release();
            storage_ = other.storage_;
            Retain();
        }
        return *this;
    }

    /** @copydoc UiHitTestSnapshot::UiHitTestSnapshot */
    UiHitTestSnapshot::UiHitTestSnapshot(UiHitTestSnapshot &&other) noexcept : storage_(std::move(other.storage_)) {}

    /** @copydoc UiHitTestSnapshot::operator= */
    UiHitTestSnapshot &UiHitTestSnapshot::operator=(UiHitTestSnapshot &&other) noexcept {
        if (this != &other) {
            Release();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiHitTestSnapshot::Retain */
    void UiHitTestSnapshot::Retain() const noexcept {
        if (storage_)
            storage_->leases.fetch_add(1);
    }

    /** @copydoc UiHitTestSnapshot::Release */
    void UiHitTestSnapshot::Release() noexcept {
        if (storage_) {
            storage_->leases.fetch_sub(1);
            storage_.reset();
        }
    }

    /** @copydoc UiHitTestSnapshot::Descriptor */
    const UiHitTestSnapshotDescriptor &UiHitTestSnapshot::Descriptor() const noexcept {
        return storage_->descriptor;
    }

    namespace {
        template <typename Storage>
        [[nodiscard]] Result<void> ValidatePresentation(const Storage &storage, const UiRenderViewId requestedView,
                                                        const UiCanvasInstanceId requestedCanvas,
                                                        const UiPresentedInteractionState &presented) {
            if (!requestedView.IsValid() || requestedView != presented.View() || requestedCanvas != storage.descriptor.canvas ||
                presented.Canvas() != storage.descriptor.canvas)
                return Failure(UiErrors::HitTestSourceStale);
            if (!presented.LastPresentedInteraction().IsValid() || presented.LastPresentedInteraction() != storage.descriptor.interaction)
                return Failure(UiErrors::HitTestNotPresented);
            return Result<void>::Success();
        }

        template <typename Storage>
        [[nodiscard]] std::optional<UiHitTestResult> HitLogical(const Storage &storage, const double canvasX, const double canvasY,
                                                                const float rayDistance) noexcept {
            if (canvasX < 0.0 || canvasY < 0.0 || canvasX >= storage.descriptor.logicalExtent.width ||
                canvasY >= storage.descriptor.logicalExtent.height)
                return std::nullopt;
            for (auto iterator = storage.records.rbegin(); iterator != storage.records.rend(); ++iterator) {
                const auto &record = *iterator;
                if (!record.visible || !record.enabled || (record.hasClip && !Contains(record.clip, canvasX, canvasY)))
                    continue;
                const double localX = record.canvasToLocal[0] * canvasX + record.canvasToLocal[1] * canvasY + record.canvasToLocal[4];
                const double localY = record.canvasToLocal[2] * canvasX + record.canvasToLocal[3] * canvasY + record.canvasToLocal[5];
                const double left = static_cast<double>(record.hitTest.origin.x) - record.hitSlop.left;
                const double top = static_cast<double>(record.hitTest.origin.y) - record.hitSlop.top;
                const double right = static_cast<double>(record.hitTest.origin.x) + record.hitTest.extent.width + record.hitSlop.right;
                const double bottom = static_cast<double>(record.hitTest.origin.y) + record.hitTest.extent.height + record.hitSlop.bottom;
                if (localX >= left && localX < right && localY >= top && localY < bottom)
                    return UiHitTestResult{record.element, ToLogicalPoint(canvasX, canvasY), rayDistance};
            }
            return std::nullopt;
        }
    }  // namespace

    /** @copydoc UiHitTestSnapshot::HitTestScreen */
    Result<std::optional<UiHitTestResult>> UiHitTestSnapshot::HitTestScreen(const UiScreenPointerQuery &query,
                                                                            const UiPresentedInteractionState &presented) const {
        if (!storage_)
            return Failure<std::optional<UiHitTestResult>>(UiErrors::HitTestLifecycleUnavailable);
        if (storage_->descriptor.renderMode == UiRenderMode::WorldSpace)
            return Failure<std::optional<UiHitTestResult>>(UiErrors::CanvasSpaceModeMismatch);
        if (const auto validation = ValidatePresentation(*storage_, query.view, query.canvas, presented); validation.HasError())
            return Result<std::optional<UiHitTestResult>>::Failure(validation.ErrorValue());
        if (!query.canvasSpace.IsValid() || query.canvasSpace.logicalExtent != storage_->descriptor.logicalExtent ||
            !std::isfinite(query.pixelX) || !std::isfinite(query.pixelY))
            return Failure<std::optional<UiHitTestResult>>(UiErrors::HitTestInvalid);
        const auto content = query.canvasSpace.ContentPixelRect();
        const double unitsPerPixel =
            static_cast<double>(query.canvasSpace.pixelsPerDip.logicalDips) * 64.0 / query.canvasSpace.pixelsPerDip.pixelUnits;
        const double resolvedWidth = content.width * unitsPerPixel;
        if (const double resolvedHeight = content.height * unitsPerPixel;
            !std::isfinite(unitsPerPixel) || std::abs(resolvedWidth - query.canvasSpace.logicalExtent.width) > 0.5 ||
            std::abs(resolvedHeight - query.canvasSpace.logicalExtent.height) > 0.5)
            return Failure<std::optional<UiHitTestResult>>(UiErrors::HitTestInvalid);
        if (query.pixelX < 0.0F || query.pixelY < 0.0F || query.pixelX >= static_cast<float>(query.canvasSpace.pixelExtent.width) ||
            query.pixelY >= static_cast<float>(query.canvasSpace.pixelExtent.height))
            return Result<std::optional<UiHitTestResult>>::Success(std::nullopt);
        if (query.pixelX < static_cast<float>(content.x) || query.pixelY < static_cast<float>(content.y) ||
            query.pixelX >= static_cast<float>(content.x + content.width) || query.pixelY >= static_cast<float>(content.y + content.height))
            return Result<std::optional<UiHitTestResult>>::Success(std::nullopt);
        const double logicalX = (static_cast<double>(query.pixelX) - content.x) * unitsPerPixel;
        const double logicalY = (static_cast<double>(query.pixelY) - content.y) * unitsPerPixel;
        return Result<std::optional<UiHitTestResult>>::Success(HitLogical(*storage_, logicalX, logicalY, 0.0F));
    }

    /** @copydoc UiHitTestSnapshot::HitTestWorld */
    Result<std::optional<UiHitTestResult>> UiHitTestSnapshot::HitTestWorld(const UiWorldRayQuery &query,
                                                                           const UiPresentedInteractionState &presented) const {
        if (!storage_)
            return Failure<std::optional<UiHitTestResult>>(UiErrors::HitTestLifecycleUnavailable);
        if (storage_->descriptor.renderMode != UiRenderMode::WorldSpace)
            return Failure<std::optional<UiHitTestResult>>(UiErrors::CanvasSpaceModeMismatch);
        if (const auto validation = ValidatePresentation(*storage_, query.view, query.canvas, presented); validation.HasError())
            return Result<std::optional<UiHitTestResult>>::Failure(validation.ErrorValue());

        const auto &ray = query.ray;
        const auto &projection = query.projection;
        const float directionLength = Math::LengthSquared(ray.direction);
        const Math::Vec3 normal = Math::Cross(projection.horizontalExtent, projection.verticalExtent);
        const float normalLength = Math::LengthSquared(normal);
        if (!Math::IsFinite(ray.origin) || !Math::IsFinite(ray.direction) || !std::isfinite(ray.minimumDistance) ||
            !std::isfinite(ray.maximumDistance) || ray.minimumDistance < 0.0F || ray.maximumDistance < ray.minimumDistance ||
            !std::isfinite(directionLength) || std::abs(directionLength - 1.0F) > 0.001F || !Math::IsFinite(projection.origin) ||
            !Math::IsFinite(projection.horizontalExtent) || !Math::IsFinite(projection.verticalExtent) || !std::isfinite(normalLength) ||
            normalLength <= Math::DefaultEpsilon)
            return Failure<std::optional<UiHitTestResult>>(UiErrors::HitTestInvalid);

        const float denominator = Math::Dot(normal, ray.direction);
        if (std::abs(denominator) <= Math::DefaultEpsilon)
            return Result<std::optional<UiHitTestResult>>::Success(std::nullopt);
        const float distance = Math::Dot(normal, projection.origin - ray.origin) / denominator;
        if (!std::isfinite(distance) || distance < ray.minimumDistance || distance > ray.maximumDistance)
            return Result<std::optional<UiHitTestResult>>::Success(std::nullopt);

        const Math::Vec3 offset = ray.origin + ray.direction * distance - projection.origin;
        const double xx = Math::Dot(projection.horizontalExtent, projection.horizontalExtent);
        const double xy = Math::Dot(projection.horizontalExtent, projection.verticalExtent);
        const double yy = Math::Dot(projection.verticalExtent, projection.verticalExtent);
        const double determinant = xx * yy - xy * xy;
        if (!std::isfinite(determinant) || determinant <= static_cast<double>(Math::DefaultEpsilon) * normalLength)
            return Failure<std::optional<UiHitTestResult>>(UiErrors::HitTestInvalid);
        const double px = Math::Dot(offset, projection.horizontalExtent);
        const double py = Math::Dot(offset, projection.verticalExtent);
        const double horizontal = (px * yy - py * xy) / determinant;
        const double vertical = (py * xx - px * xy) / determinant;
        const double logicalX = horizontal * storage_->descriptor.logicalExtent.width;
        const double logicalY = vertical * storage_->descriptor.logicalExtent.height;
        return Result<std::optional<UiHitTestResult>>::Success(HitLogical(*storage_, logicalX, logicalY, distance));
    }

    /** @copydoc UiHitTestStore::Create */
    Result<UiHitTestStore> UiHitTestStore::Create(const UiHitTestStoreDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return Failure<UiHitTestStore>(UiErrors::HitTestInvalid);
        try {
            return Result<UiHitTestStore>::Success(UiHitTestStore{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiHitTestStore>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiHitTestStore::UiHitTestStore */
    UiHitTestStore::UiHitTestStore(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiHitTestStore::~UiHitTestStore */
    UiHitTestStore::~UiHitTestStore() {
        Shutdown();
    }

    /** @copydoc UiHitTestStore::UiHitTestStore */
    UiHitTestStore::UiHitTestStore(UiHitTestStore &&) noexcept = default;

    /** @copydoc UiHitTestStore::operator= */
    UiHitTestStore &UiHitTestStore::operator=(UiHitTestStore &&) noexcept = default;

    /** @copydoc UiHitTestStore::Publish */
    Result<UiHitTestSnapshot> UiHitTestStore::Publish(const UiLayoutSnapshot &layout, const std::span<const UiHitTestElement> elements) {
        if (!storage_ || storage_->lifecycle != UiHitTestStoreState::Active)
            return Failure<UiHitTestSnapshot>(UiErrors::HitTestLifecycleUnavailable);
        const auto &source = layout.Descriptor();
        const auto records = layout.Records();
        if (source.instance != storage_->descriptor.instance || source.canvas != storage_->descriptor.canvas ||
            source.document != storage_->descriptor.document || !source.sources.tree.IsValid() || !source.interaction.IsValid())
            return Failure<UiHitTestSnapshot>(UiErrors::HitTestSourceStale);
        if (records.empty() || records.size() != elements.size() || records.size() > storage_->descriptor.elementCapacity)
            return Failure<UiHitTestSnapshot>(UiErrors::HitTestInvalid);
        if (!ArePublishElementsValid(records, elements))
            return Failure<UiHitTestSnapshot>(UiErrors::HitTestInvalid);

        auto slot = storage_->TryAcquire();
        if (!slot)
            return Failure<UiHitTestSnapshot>(UiErrors::HitTestSnapshotStorageExhausted);
        slot->descriptor = {source.instance,
                            source.canvas,
                            source.document,
                            source.sources.tree,
                            source.interaction,
                            storage_->descriptor.renderMode,
                            storage_->descriptor.logicalExtent};
        PopulateHitTestRecords(records, elements, slot->records);
        std::ranges::sort(slot->records, [](const auto &lhs, const auto &rhs) {
            return lhs.zOrder < rhs.zOrder || (lhs.zOrder == rhs.zOrder && lhs.paintOrder < rhs.paintOrder);
        });

        storage_->ReleaseCurrent();
        storage_->current = slot;
        slot->leases.fetch_add(1);
        return Result<UiHitTestSnapshot>::Success(UiHitTestSnapshot{std::move(slot)});
    }

    /** @copydoc UiHitTestStore::BeginRetirement */
    Result<void> UiHitTestStore::BeginRetirement() {
        if (!storage_ || storage_->lifecycle != UiHitTestStoreState::Active)
            return Failure(UiErrors::HitTestLifecycleUnavailable);
        storage_->lifecycle = UiHitTestStoreState::Retiring;
        return Result<void>::Success();
    }

    /** @copydoc UiHitTestStore::Shutdown */
    void UiHitTestStore::Shutdown() noexcept {
        if (!storage_ || storage_->lifecycle == UiHitTestStoreState::Stopped)
            return;
        storage_->lifecycle = UiHitTestStoreState::Stopped;
        storage_->ReleaseCurrent();
    }

    /** @copydoc UiHitTestStore::State */
    UiHitTestStoreState UiHitTestStore::State() const noexcept {
        return storage_ ? storage_->lifecycle : UiHitTestStoreState::Stopped;
    }

    /** @copydoc UiHitTestStore::IsDrained */
    bool UiHitTestStore::IsDrained() const noexcept {
        if (!storage_)
            return true;
        return std::ranges::all_of(storage_->slots, [this](const auto &slot) {
            const auto leases = slot->leases.load();
            const auto retainedByStore = slot == storage_->current ? 1U : 0U;
            return leases <= retainedByStore;
        });
    }
}  // namespace Horo::Runtime::Ui
