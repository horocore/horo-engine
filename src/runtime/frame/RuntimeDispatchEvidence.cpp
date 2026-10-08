#include "Horo/Runtime/RuntimeDispatchEvidence.h"

#include "internal/RuntimeDispatchOrdinal.h"

#include <atomic>
#include <new>
#include <thread>

namespace Horo::Runtime {
    /** @copydoc RuntimeDispatchSource::BindingStatus */
    RuntimeDispatchStatus RuntimeDispatchSource::BindingStatus() const noexcept {
        return ValidateRun();
    }

    struct RuntimeDispatchSource::Storage final {
        Storage() = default;

        Storage(const Storage &) = delete;
        Storage &operator=(const Storage &) = delete;
        Storage(Storage &&) = delete;
        Storage &operator=(Storage &&) = delete;

        const std::thread::id owner{std::this_thread::get_id()};
        // Only retirement may cross the owner thread. Foreign reads reject before accessing all other mutable fields.
        std::atomic<bool> retired{};
        RuntimeDispatchFacts facts;
        std::uint64_t ordinal{};
        RuntimePhase phase{};
        bool active{};
        bool exhausted{};
    };

    /** @copydoc RuntimeDispatchSource::IsValid */
    bool RuntimeDispatchSource::IsValid() const noexcept {
        return storage_.Get() != nullptr;
    }

    /** @copydoc RuntimeDispatchSource::Initialize */
    bool RuntimeDispatchSource::Initialize() {
        try {
            storage_.PublisherPin() = std::make_shared<Storage>();
            return true;
        } catch (const std::bad_alloc &) {
            return false;
        }
    }

    /** @copydoc RuntimeDispatchSource::ValidateRun */
    RuntimeDispatchStatus RuntimeDispatchSource::ValidateRun() const noexcept {
        using enum RuntimeDispatchStatus;
        if (!storage_.Get())
            return Invalid;
        if (storage_->owner != std::this_thread::get_id())
            return WrongThread;
        if (storage_->retired.load())
            return Retired;
        if (storage_->exhausted)
            return Exhausted;
        return storage_->active ? Reentrant : Valid;
    }

    /** @copydoc RuntimeDispatchSource::Begin */
    RuntimeDispatchStatus RuntimeDispatchSource::Begin(const RuntimePhase phase, const RuntimeDispatchFacts &facts) noexcept {
        using enum RuntimeDispatchStatus;
        if (const auto valid = ValidateRun(); valid != Valid)
            return valid;
        if (!Internal::AdvanceDispatchOrdinal(storage_->ordinal)) {
            storage_->exhausted = true;
            return Exhausted;
        }
        storage_->phase = phase;
        storage_->facts = facts;
        storage_->active = true;
        return Valid;
    }

    /** @copydoc RuntimeDispatchSource::End */
    void RuntimeDispatchSource::End() noexcept {
        storage_->active = false;
    }

    /** @copydoc RuntimeDispatchSource::Retire */
    void RuntimeDispatchSource::Retire() noexcept {
        if (storage_.Get())
            storage_->retired.store(true);
    }

    /** @copydoc RuntimeDispatchSource::Evidence */
    RuntimeDispatchEvidence RuntimeDispatchSource::Evidence() const noexcept {
        return RuntimeDispatchEvidence{storage_.ReadPin(), storage_->ordinal};
    }

    /** @copydoc RuntimeDispatchEvidence::Read */
    RuntimeDispatchStatus RuntimeDispatchEvidence::Read(const RuntimeDispatchSource &expected, const RuntimePhase phase,
                                                        RuntimeDispatchFacts &facts) const noexcept {
        using enum RuntimeDispatchStatus;
        if (!storage_ || ordinal_ == 0)
            return Invalid;
        if (storage_->owner != std::this_thread::get_id())
            return WrongThread;
        if (storage_.get() != expected.storage_.Get())
            return ForeignSource;
        if (storage_->retired.load())
            return Retired;
        if (!storage_->active || storage_->ordinal != ordinal_)
            return Stale;
        if (storage_->phase != phase)
            return WrongPhase;
        facts = storage_->facts;
        return Valid;
    }
}  // namespace Horo::Runtime
