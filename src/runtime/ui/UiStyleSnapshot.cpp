#include "UiStyleInternal.h"

#include <algorithm>
#include <utility>

namespace Horo::Runtime::Ui {
    UiComputedStyleSnapshot::Storage::Storage(const std::uint32_t elementCapacity, const std::uint32_t propertyCapacity) {
        records.reserve(elementCapacity);
        lookup.reserve(elementCapacity);
        styles.reserve(elementCapacity);
        properties.reserve(static_cast<std::size_t>(elementCapacity) * propertyCapacity);
    }

    /** @copydoc UiComputedStyleSnapshot::Retain */
    void UiComputedStyleSnapshot::Retain() const noexcept {
        if (storage_)
            storage_->leases.fetch_add(1);
    }

    /** @copydoc UiComputedStyleSnapshot::Release */
    void UiComputedStyleSnapshot::Release() noexcept {
        if (storage_) {
            storage_->leases.fetch_sub(1);
            storage_.reset();
        }
    }

    /** @copydoc UiComputedStyleSnapshot::UiComputedStyleSnapshot */
    UiComputedStyleSnapshot::UiComputedStyleSnapshot(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiComputedStyleSnapshot::~UiComputedStyleSnapshot */
    UiComputedStyleSnapshot::~UiComputedStyleSnapshot() {
        Release();
    }

    /** @copydoc UiComputedStyleSnapshot::UiComputedStyleSnapshot */
    UiComputedStyleSnapshot::UiComputedStyleSnapshot(const UiComputedStyleSnapshot &other) noexcept : storage_(other.storage_) {
        Retain();
    }

    /** @copydoc UiComputedStyleSnapshot::operator= */
    UiComputedStyleSnapshot &UiComputedStyleSnapshot::operator=(const UiComputedStyleSnapshot &other) noexcept {
        if (this != &other) {
            Release();
            storage_ = other.storage_;
            Retain();
        }
        return *this;
    }

    /** @copydoc UiComputedStyleSnapshot::UiComputedStyleSnapshot */
    UiComputedStyleSnapshot::UiComputedStyleSnapshot(UiComputedStyleSnapshot &&other) noexcept : storage_(std::move(other.storage_)) {}

    /** @copydoc UiComputedStyleSnapshot::operator= */
    UiComputedStyleSnapshot &UiComputedStyleSnapshot::operator=(UiComputedStyleSnapshot &&other) noexcept {
        if (this != &other) {
            Release();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiComputedStyleSnapshot::Descriptor */
    const UiComputedStyleSnapshotDescriptor &UiComputedStyleSnapshot::Descriptor() const noexcept {
        return storage_->descriptor;
    }

    /** @copydoc UiComputedStyleSnapshot::Records */
    std::span<const UiComputedStyleRecord> UiComputedStyleSnapshot::Records() const noexcept {
        return storage_ ? std::span<const UiComputedStyleRecord>{storage_->records} : std::span<const UiComputedStyleRecord>{};
    }

    /** @copydoc UiComputedStyleSnapshot::Properties */
    std::span<const UiComputedStyleProperty> UiComputedStyleSnapshot::Properties(const UiComputedStyleRecord &record) const noexcept {
        if (!storage_ || record.firstProperty > storage_->properties.size() ||
            record.propertyCount > storage_->properties.size() - record.firstProperty)
            return {};
        return {storage_->properties.data() + record.firstProperty, record.propertyCount};
    }

    /** @copydoc UiComputedStyleSnapshot::Get */
    Result<UiComputedStyleRecord> UiComputedStyleSnapshot::Get(const UiElementHandle element) const {
        if (!storage_ || !element.IsValid())
            return StyleInternal::Failure<UiComputedStyleRecord>(UiErrors::StyleInvalid);
        const auto found = std::lower_bound(storage_->lookup.begin(), storage_->lookup.end(), element,
                                            [this](const std::uint32_t index, const UiElementHandle value) {
            return storage_->records[index].element < value;
        });
        if (found == storage_->lookup.end() || storage_->records[*found].element != element)
            return StyleInternal::Failure<UiComputedStyleRecord>(UiErrors::HandleStale);
        return Result<UiComputedStyleRecord>::Success(storage_->records[*found]);
    }
}  // namespace Horo::Runtime::Ui
