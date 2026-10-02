#include "Horo/Runtime/Ui/UiAsyncActions.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <atomic>
#include <limits>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        /** @brief Checks the closed cancellation vocabulary without mutating state. */
        bool ValidReason(const UiActionCancellationReason reason) noexcept {
            return reason < UiActionCancellationReason::Count;
        }

        /** @brief Bounds immutable error traversal and text without flattening original evidence. */
        bool ValidFailure(const Error *error) noexcept {
            std::size_t bytes{};
            std::size_t causes{};
            for (; error != nullptr; error = error->cause.Get()) {
                if (++causes > 8 || error->code.Value().empty() || error->domain.Value().empty() ||
                    error->severity > ErrorSeverity::Critical || error->diagnostics.size() > 16)
                    return false;
                const auto add = [&bytes](const std::string &text) {
                    if (text.size() > 4096 - bytes)
                        return false;
                    bytes += text.size();
                    return true;
                };
                if (!add(error->code.Value()) || !add(error->domain.Value()) || !add(error->message))
                    return false;
                for (const auto &diagnostic : error->diagnostics)
                    if (diagnostic.severity > DiagnosticSeverity::Fatal || !add(diagnostic.code.Value()) || !add(diagnostic.message) ||
                        !add(diagnostic.location.source) || !add(diagnostic.path))
                        return false;
            }
            return causes != 0;
        }
    }  // namespace

    namespace UiAsyncActionDetail {
        /** @brief Slot lifetime is shared by store, completion lease and cancellation observers only. */
        struct Record final {
            UiAsyncActionSnapshot snapshot;
            std::thread::id ownerThread{std::this_thread::get_id()};
            // Sole owner-thread writer; workers read only this signal. Tokens pin storage through shutdown.
            std::atomic<bool> cancelled{false};
            bool retained{};

            [[nodiscard]] UiAsyncActionKey Key() const noexcept {
                return {snapshot.source, snapshot.request, snapshot.operation};
            }

            [[nodiscard]] Result<void> Writable() const {
                if (ownerThread != std::this_thread::get_id())
                    return Failure(UiErrors::ActionLifecycleUnavailable);
                if (!snapshot.Busy())
                    return Failure(UiErrors::AsyncActionAlreadyTerminal);
                return Result<void>::Success();
            }

            void Cancel(const UiActionCancellationReason reason) noexcept {
                if (!snapshot.Busy())
                    return;
                snapshot.state = UiAsyncActionState::Cancelled;
                snapshot.cancellation = reason;
                cancelled.store(true, std::memory_order_release);
            }
        };

        static_assert(std::atomic<bool>::is_always_lock_free);
    }  // namespace UiAsyncActionDetail

    /** @copydoc UiAsyncActionCancellation::UiAsyncActionCancellation */
    UiAsyncActionCancellation::UiAsyncActionCancellation(std::shared_ptr<UiAsyncActionDetail::Record> record) noexcept
        : record_(std::move(record)) {}

    /** @copydoc UiAsyncActionCancellation::IsCancellationRequested */
    bool UiAsyncActionCancellation::IsCancellationRequested() const noexcept {
        return !record_ || record_->cancelled.load(std::memory_order_acquire);
    }

    /** @copydoc UiAsyncActionProducer::UiAsyncActionProducer */
    UiAsyncActionProducer::UiAsyncActionProducer(std::shared_ptr<UiAsyncActionDetail::Record> record) noexcept
        : record_(std::move(record)) {}

    /** @copydoc UiAsyncActionProducer::~UiAsyncActionProducer */
    UiAsyncActionProducer::~UiAsyncActionProducer() {
        Abandon();
    }

    /** @copydoc UiAsyncActionProducer::UiAsyncActionProducer */
    UiAsyncActionProducer::UiAsyncActionProducer(UiAsyncActionProducer &&) noexcept = default;

    /** @copydoc UiAsyncActionProducer::operator= */
    UiAsyncActionProducer &UiAsyncActionProducer::operator=(UiAsyncActionProducer &&other) noexcept {
        if (this != &other) {
            Abandon();
            record_ = std::move(other.record_);
        }
        return *this;
    }

    /** @copydoc UiAsyncActionProducer::Abandon */
    void UiAsyncActionProducer::Abandon() noexcept {
        if (record_)
            record_->Cancel(UiActionCancellationReason::Requested);
        record_.reset();
    }

    /** @copydoc UiAsyncActionProducer::Key */
    UiAsyncActionKey UiAsyncActionProducer::Key() const noexcept {
        return record_ ? record_->Key() : UiAsyncActionKey{};
    }

    /** @copydoc UiAsyncActionProducer::Cancellation */
    UiAsyncActionCancellation UiAsyncActionProducer::Cancellation() const noexcept {
        return UiAsyncActionCancellation{record_};
    }

    /** @copydoc UiAsyncActionProducer::PublishProgress */
    Result<void> UiAsyncActionProducer::PublishProgress(const UiAsyncActionProgress progress) {
        if (!record_)
            return Failure(UiErrors::ActionResultStale);
        if (const auto writable = record_->Writable(); writable.HasError())
            return writable;
        const auto previous = record_->snapshot.progress;
        if (progress.permille > 1000 || (!progress.determinate && progress.permille != 0) || progress.phase < previous.phase ||
            (progress.phase == previous.phase && previous.determinate && (!progress.determinate || progress.permille < previous.permille)))
            return Failure(UiErrors::AsyncActionProgressInvalid);
        record_->snapshot.progress = progress;
        return Result<void>::Success();
    }

    /** @copydoc UiAsyncActionProducer::Complete */
    Result<void> UiAsyncActionProducer::Complete(UiActionPayload payload) {
        if (!record_)
            return Failure(UiErrors::ActionResultStale);
        if (const auto writable = record_->Writable(); writable.HasError())
            return writable;
        if (const auto valid = payload.Validate(); valid.HasError())
            return valid;
        record_->snapshot.payload = std::move(payload);
        record_->snapshot.state = UiAsyncActionState::Completed;
        return Result<void>::Success();
    }

    /** @copydoc UiAsyncActionProducer::Fail */
    Result<void> UiAsyncActionProducer::Fail(std::shared_ptr<const Error> error) {
        if (!record_)
            return Failure(UiErrors::ActionResultStale);
        if (const auto writable = record_->Writable(); writable.HasError())
            return writable;
        if (!ValidFailure(error.get()))
            return Failure(UiErrors::AsyncActionFailureInvalid);
        record_->snapshot.error = std::move(error);
        record_->snapshot.state = UiAsyncActionState::Failed;
        return Result<void>::Success();
    }

    struct UiAsyncActionStore::Storage final {
        Storage(const UiActionOwnerContext source, const std::uint32_t capacity) : owner(source) {
            records.reserve(capacity);
            for (std::uint32_t index = 0; index < capacity; ++index)
                records.push_back(std::make_shared<UiAsyncActionDetail::Record>());
        }

        [[nodiscard]] auto Find(const UiAsyncActionKey key) const noexcept {
            for (const auto &record : records)
                if (record->retained && record->Key() == key)
                    return record;
            return std::shared_ptr<UiAsyncActionDetail::Record>{};
        }

        UiActionOwnerContext owner;
        std::vector<std::shared_ptr<UiAsyncActionDetail::Record>> records;
        std::thread::id ownerThread{std::this_thread::get_id()};
        std::uint64_t lastRequestSequence{};
        bool active{true};
    };

    /** @copydoc UiAsyncActionStore::Create */
    Result<UiAsyncActionStore> UiAsyncActionStore::Create(const UiActionOwnerContext owner, const std::uint32_t capacity) {
        if (!owner.IsValid() || capacity == 0 || capacity > MaximumUiActionCommands)
            return Failure<UiAsyncActionStore>(UiErrors::ActionInvalid);
        try {
            return Result<UiAsyncActionStore>::Success(UiAsyncActionStore{std::make_unique<Storage>(owner, capacity)});
        } catch (const std::bad_alloc &) {
            return Failure<UiAsyncActionStore>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiAsyncActionStore::UiAsyncActionStore */
    UiAsyncActionStore::UiAsyncActionStore(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiAsyncActionStore::~UiAsyncActionStore */
    UiAsyncActionStore::~UiAsyncActionStore() {
        Retire(UiActionCancellationReason::Shutdown);
    }

    /** @copydoc UiAsyncActionStore::UiAsyncActionStore */
    UiAsyncActionStore::UiAsyncActionStore(UiAsyncActionStore &&) noexcept = default;

    /** @copydoc UiAsyncActionStore::operator= */
    UiAsyncActionStore &UiAsyncActionStore::operator=(UiAsyncActionStore &&other) noexcept {
        if (this != &other) {
            Retire(UiActionCancellationReason::Superseded);
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiAsyncActionStore::Start */
    Result<UiAsyncActionProducer> UiAsyncActionStore::Start(const UiActionRequest &request) {
        if (!storage_ || !storage_->active || storage_->ownerThread != std::this_thread::get_id())
            return Failure<UiAsyncActionProducer>(UiErrors::ActionLifecycleUnavailable);
        if (const auto valid = request.Validate(); valid.HasError())
            return Result<UiAsyncActionProducer>::Failure(valid.ErrorValue());
        if (request.source.owner != storage_->owner || request.id.ownership != storage_->owner.instance.ownership ||
            request.id.sequence.Value() <= storage_->lastRequestSequence)
            return Failure<UiAsyncActionProducer>(UiErrors::ActionSourceStale);
        std::shared_ptr<UiAsyncActionDetail::Record> free;
        for (const auto &record : storage_->records) {
            if (record->retained && record->snapshot.Busy() && record->snapshot.source == request.source)
                return Failure<UiAsyncActionProducer>(UiErrors::AsyncActionBusy);
            if (!record->retained && record.use_count() == 1 && !free)
                free = record;
        }
        if (!free)
            return Failure<UiAsyncActionProducer>(UiErrors::AsyncActionCapacityExceeded);
        const UiActionOperationId operation{request.id.ownership, UiActionOperationSequence::Create(request.id.sequence.Value()).Value()};
        free->snapshot = {request.source, request.id, operation};
        free->cancelled.store(false, std::memory_order_release);
        free->retained = true;
        storage_->lastRequestSequence = request.id.sequence.Value();
        return Result<UiAsyncActionProducer>::Success(UiAsyncActionProducer{std::move(free)});
    }

    /** @copydoc UiAsyncActionStore::Snapshot */
    Result<UiAsyncActionSnapshot> UiAsyncActionStore::Snapshot(const UiAsyncActionKey operation) const {
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id())
            return Failure<UiAsyncActionSnapshot>(UiErrors::ActionLifecycleUnavailable);
        const auto record = storage_->Find(operation);
        if (!record)
            return Failure<UiAsyncActionSnapshot>(UiErrors::ActionResultStale);
        return Result<UiAsyncActionSnapshot>::Success(record->snapshot);
    }

    /** @copydoc UiAsyncActionStore::Project */
    Result<std::optional<UiAsyncActionSnapshot>> UiAsyncActionStore::Project(const UiActionSource source) const {
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id())
            return Failure<std::optional<UiAsyncActionSnapshot>>(UiErrors::ActionLifecycleUnavailable);
        if (!source.IsValid() || source.owner != storage_->owner)
            return Failure<std::optional<UiAsyncActionSnapshot>>(UiErrors::ActionSourceStale);
        const UiAsyncActionSnapshot *latest{};
        for (const auto &record : storage_->records)
            if (record->retained && record->snapshot.source == source &&
                (!latest || record->snapshot.request.sequence > latest->request.sequence))
                latest = &record->snapshot;
        return Result<std::optional<UiAsyncActionSnapshot>>::Success(latest ? std::optional{*latest} : std::nullopt);
    }

    /** @copydoc UiAsyncActionStore::Cancel */
    Result<void> UiAsyncActionStore::Cancel(const UiAsyncActionKey operation, const UiActionCancellationReason reason) {
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id())
            return Failure(UiErrors::ActionLifecycleUnavailable);
        if (!ValidReason(reason))
            return Failure(UiErrors::ActionResultInvalid);
        const auto record = storage_->Find(operation);
        if (!record)
            return Failure(UiErrors::ActionResultStale);
        record->Cancel(reason);
        return Result<void>::Success();
    }

    /** @copydoc UiAsyncActionStore::Release */
    Result<void> UiAsyncActionStore::Release(const UiAsyncActionKey operation) {
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id())
            return Failure(UiErrors::ActionLifecycleUnavailable);
        const auto record = storage_->Find(operation);
        if (!record)
            return Failure(UiErrors::ActionResultStale);
        if (record->snapshot.Busy())
            return Failure(UiErrors::AsyncActionBusy);
        record->retained = false;
        return Result<void>::Success();
    }

    /** @copydoc UiAsyncActionStore::Retire */
    void UiAsyncActionStore::Retire(const UiActionCancellationReason reason) noexcept {
        if (!storage_ || !storage_->active || !ValidReason(reason))
            return;
        storage_->active = false;
        for (const auto &record : storage_->records)
            if (record->retained)
                record->Cancel(reason);
    }
}  // namespace Horo::Runtime::Ui
