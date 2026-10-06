#include "Horo/Runtime/Ui/UiAsyncActions.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <atomic>
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

        /** @brief Consumes a finite text budget without copying provider-owned strings. */
        bool AddFailureText(const std::string_view text, std::size_t &bytes) noexcept {
            if (text.size() > 4096 - bytes)
                return false;
            bytes += text.size();
            return true;
        }

        /** @brief Validates one diagnostic identity and its share of the total failure text bound. */
        bool ValidFailureDiagnostic(const Diagnostic &diagnostic, std::size_t &bytes) noexcept {
            return !diagnostic.code.Value().empty() && diagnostic.severity <= DiagnosticSeverity::Fatal &&
                   AddFailureText(diagnostic.code.Value(), bytes) && AddFailureText(diagnostic.message, bytes) &&
                   AddFailureText(diagnostic.location.source, bytes) && AddFailureText(diagnostic.path, bytes);
        }

        /** @brief Validates one typed error node and its bounded diagnostic collection. */
        bool ValidFailureNode(const Error &error, std::size_t &bytes) noexcept {
            if (error.code.Value().empty() || error.domain.Value().empty() || error.severity > ErrorSeverity::Critical ||
                error.diagnostics.size() > 16 || !AddFailureText(error.code.Value(), bytes) ||
                !AddFailureText(error.domain.Value(), bytes) || !AddFailureText(error.message, bytes))
                return false;
            for (const auto &diagnostic : error.diagnostics)
                if (!ValidFailureDiagnostic(diagnostic, bytes))
                    return false;
            return true;
        }

        /** @brief Bounds immutable error traversal without flattening original evidence. */
        bool ValidFailure(const Error *error) noexcept {
            std::size_t bytes{};
            std::size_t nodes{};
            for (; error != nullptr; error = error->cause.Get())
                if (++nodes > 8 || !ValidFailureNode(*error, bytes))
                    return false;
            return nodes != 0;
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
                cancelled.store(true);
            }
        };

        static_assert(std::atomic<bool>::is_always_lock_free);
    }  // namespace UiAsyncActionDetail

    /** @copydoc UiAsyncActionCancellation::UiAsyncActionCancellation */
    UiAsyncActionCancellation::UiAsyncActionCancellation(std::shared_ptr<const UiAsyncActionDetail::Record> record) noexcept
        : record_(std::move(record)) {}

    /** @copydoc UiAsyncActionCancellation::IsCancellationRequested */
    bool UiAsyncActionCancellation::IsCancellationRequested() const noexcept {
        return !record_ || record_->cancelled.load();
    }

    /** @copydoc UiAsyncActionProducer::UiAsyncActionProducer */
    UiAsyncActionProducer::UiAsyncActionProducer(std::shared_ptr<UiAsyncActionDetail::Record> record) noexcept
        : record_(std::move(record)) {}

    /** @copydoc UiAsyncActionProducer::StateRecord */
    UiAsyncActionDetail::Record *UiAsyncActionProducer::StateRecord() noexcept {
        return record_.get();
    }

    /** @copydoc UiAsyncActionProducer::StateRecord */
    const UiAsyncActionDetail::Record *UiAsyncActionProducer::StateRecord() const noexcept {
        return record_.get();
    }

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
        if (auto *const record = StateRecord())
            record->Cancel(UiActionCancellationReason::Requested);
        record_.reset();
    }

    /** @copydoc UiAsyncActionProducer::Key */
    UiAsyncActionKey UiAsyncActionProducer::Key() const noexcept {
        auto *const record = StateRecord();
        return record ? record->Key() : UiAsyncActionKey{};
    }

    /** @copydoc UiAsyncActionProducer::Cancellation */
    UiAsyncActionCancellation UiAsyncActionProducer::Cancellation() const noexcept {
        return UiAsyncActionCancellation{record_};
    }

    /** @copydoc UiAsyncActionProducer::PublishProgress */
    Result<void> UiAsyncActionProducer::PublishProgress(const UiAsyncActionProgress progress) {
        auto *const record = StateRecord();
        if (!record)
            return Failure(UiErrors::ActionResultStale);
        if (const auto writable = record->Writable(); writable.HasError())
            return writable;
        if (const auto previous = record->snapshot.progress;
            progress.permille > 1000 || (!progress.determinate && progress.permille != 0) || progress.phase < previous.phase ||
            (progress.phase == previous.phase && previous.determinate && (!progress.determinate || progress.permille < previous.permille)))
            return Failure(UiErrors::AsyncActionProgressInvalid);
        record->snapshot.progress = progress;
        return Result<void>::Success();
    }

    /** @copydoc UiAsyncActionProducer::Complete */
    Result<void> UiAsyncActionProducer::Complete(UiActionPayload payload) {
        auto *const record = StateRecord();
        if (!record)
            return Failure(UiErrors::ActionResultStale);
        if (const auto writable = record->Writable(); writable.HasError())
            return writable;
        if (const auto valid = payload.Validate(); valid.HasError())
            return valid;
        record->snapshot.payload = std::move(payload);
        record->snapshot.state = UiAsyncActionState::Completed;
        return Result<void>::Success();
    }

    /** @copydoc UiAsyncActionProducer::Fail */
    Result<void> UiAsyncActionProducer::Fail(std::shared_ptr<const Error> error) {
        auto *const record = StateRecord();
        if (!record)
            return Failure(UiErrors::ActionResultStale);
        if (const auto writable = record->Writable(); writable.HasError())
            return writable;
        if (!ValidFailure(error.get()))
            return Failure(UiErrors::AsyncActionFailureInvalid);
        record->snapshot.error = std::move(error);
        record->snapshot.state = UiAsyncActionState::Failed;
        return Result<void>::Success();
    }

    struct UiAsyncActionStore::Storage final {
        Storage(const UiActionOwnerContext &source, const std::uint32_t capacity) : owner(source) {
            records.reserve(capacity);
            for (std::uint32_t index = 0; index < capacity; ++index)
                records.push_back(std::make_shared<UiAsyncActionDetail::Record>());
        }

        /** @brief Finds retained correlation without granting mutation authority. */
        [[nodiscard]] std::optional<std::size_t> FindIndex(const UiAsyncActionKey &key) const noexcept {
            for (std::size_t index = 0; index < records.size(); ++index)
                if (records[index]->retained && records[index]->Key() == key)
                    return index;
            return std::nullopt;
        }

        /** @brief Borrows exact mutable state only from a mutable owner. */
        [[nodiscard]] UiAsyncActionDetail::Record *Find(const UiAsyncActionKey &key) noexcept {
            const auto index = FindIndex(key);
            return index ? records[*index].get() : nullptr;
        }

        /** @brief Borrows exact read-only state from a const owner. */
        [[nodiscard]] const UiAsyncActionDetail::Record *Find(const UiAsyncActionKey &key) const noexcept {
            const auto index = FindIndex(key);
            return index ? records[*index].get() : nullptr;
        }

        /** @brief Borrows the latest read-only record for one exact source. */
        [[nodiscard]] const UiAsyncActionDetail::Record *Latest(const UiActionSource &source) const noexcept {
            const UiAsyncActionDetail::Record *latest{};
            for (const auto &slot : records) {
                const auto *const record = slot.get();
                if (record->retained && record->snapshot.source == source &&
                    (!latest || record->snapshot.request.sequence > latest->snapshot.request.sequence))
                    latest = record;
            }
            return latest;
        }

        UiActionOwnerContext owner;
        std::vector<std::shared_ptr<UiAsyncActionDetail::Record>> records;
        std::thread::id ownerThread{std::this_thread::get_id()};
        std::uint64_t lastRequestSequence{};
        bool active{true};
    };

    /** @copydoc UiAsyncActionStore::Create */
    Result<UiAsyncActionStore> UiAsyncActionStore::Create(const UiActionOwnerContext &owner, const std::uint32_t capacity) {
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

    /** @copydoc UiAsyncActionStore::StateStorage */
    UiAsyncActionStore::Storage *UiAsyncActionStore::StateStorage() noexcept {
        return storage_.get();
    }

    /** @copydoc UiAsyncActionStore::StateStorage */
    const UiAsyncActionStore::Storage *UiAsyncActionStore::StateStorage() const noexcept {
        return storage_.get();
    }

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
        auto *const storage = StateStorage();
        if (!storage || !storage->active || storage->ownerThread != std::this_thread::get_id())
            return Failure<UiAsyncActionProducer>(UiErrors::ActionLifecycleUnavailable);
        if (const auto valid = request.Validate(); valid.HasError())
            return Result<UiAsyncActionProducer>::Failure(valid.ErrorValue());
        if (request.source.owner != storage->owner || request.id.ownership != storage->owner.instance.ownership ||
            request.id.sequence.Value() <= storage->lastRequestSequence)
            return Failure<UiAsyncActionProducer>(UiErrors::ActionSourceStale);
        std::shared_ptr<UiAsyncActionDetail::Record> free;
        for (const auto &record : storage->records) {
            if (record->retained && record->snapshot.Busy() && record->snapshot.source == request.source)
                return Failure<UiAsyncActionProducer>(UiErrors::AsyncActionBusy);
            if (!record->retained && record.use_count() == 1 && !free)
                free = record;
        }
        if (!free)
            return Failure<UiAsyncActionProducer>(UiErrors::AsyncActionCapacityExceeded);
        const UiActionOperationId operation{request.id.ownership, UiActionOperationSequence::Create(request.id.sequence.Value()).Value()};
        free->snapshot = {request.source, request.id, operation};
        free->cancelled.store(false);
        free->retained = true;
        storage->lastRequestSequence = request.id.sequence.Value();
        return Result<UiAsyncActionProducer>::Success(UiAsyncActionProducer{std::move(free)});
    }

    /** @copydoc UiAsyncActionStore::Snapshot */
    Result<UiAsyncActionSnapshot> UiAsyncActionStore::Snapshot(const UiAsyncActionKey &operation) const {
        auto *const storage = StateStorage();
        if (!storage || storage->ownerThread != std::this_thread::get_id())
            return Failure<UiAsyncActionSnapshot>(UiErrors::ActionLifecycleUnavailable);
        const auto record = storage->Find(operation);
        if (!record)
            return Failure<UiAsyncActionSnapshot>(UiErrors::ActionResultStale);
        return Result<UiAsyncActionSnapshot>::Success(record->snapshot);
    }

    /** @copydoc UiAsyncActionStore::Project */
    Result<std::optional<UiAsyncActionSnapshot>> UiAsyncActionStore::Project(const UiActionSource &source) const {
        auto *const storage = StateStorage();
        if (!storage || storage->ownerThread != std::this_thread::get_id())
            return Failure<std::optional<UiAsyncActionSnapshot>>(UiErrors::ActionLifecycleUnavailable);
        if (!source.IsValid() || source.owner != storage->owner)
            return Failure<std::optional<UiAsyncActionSnapshot>>(UiErrors::ActionSourceStale);
        const auto *const latest = storage->Latest(source);
        return Result<std::optional<UiAsyncActionSnapshot>>::Success(latest ? std::optional{latest->snapshot} : std::nullopt);
    }

    /** @copydoc UiAsyncActionStore::Cancel */
    Result<void> UiAsyncActionStore::Cancel(const UiAsyncActionKey &operation, const UiActionCancellationReason reason) {
        auto *const storage = StateStorage();
        if (!storage || storage->ownerThread != std::this_thread::get_id())
            return Failure(UiErrors::ActionLifecycleUnavailable);
        if (!ValidReason(reason))
            return Failure(UiErrors::ActionResultInvalid);
        const auto record = storage->Find(operation);
        if (!record)
            return Failure(UiErrors::ActionResultStale);
        record->Cancel(reason);
        return Result<void>::Success();
    }

    /** @copydoc UiAsyncActionStore::Release */
    Result<void> UiAsyncActionStore::Release(const UiAsyncActionKey &operation) {
        auto *const storage = StateStorage();
        if (!storage || storage->ownerThread != std::this_thread::get_id())
            return Failure(UiErrors::ActionLifecycleUnavailable);
        const auto record = storage->Find(operation);
        if (!record)
            return Failure(UiErrors::ActionResultStale);
        if (record->snapshot.Busy())
            return Failure(UiErrors::AsyncActionBusy);
        record->retained = false;
        return Result<void>::Success();
    }

    /** @copydoc UiAsyncActionStore::Retire */
    void UiAsyncActionStore::Retire(const UiActionCancellationReason reason) noexcept {
        auto *const storage = StateStorage();
        if (!storage || !storage->active || !ValidReason(reason))
            return;
        storage->active = false;
        for (const auto &record : storage->records)
            if (record->retained)
                record->Cancel(reason);
    }

    /** @copydoc UiAsyncActionStore::CanPrepareReplacementSource */
    bool UiAsyncActionStore::CanPrepareReplacementSource() const noexcept {
        const auto *const storage = StateStorage();
        if (!storage || storage->ownerThread != std::this_thread::get_id())
            return false;
        for (const auto &record : storage->records)
            if (record.use_count() != 1 || record->retained || record->snapshot.error)
                return false;
        return true;
    }

    /** @copydoc UiAsyncActionStore::PrepareReplacementSource */
    void UiAsyncActionStore::PrepareReplacementSource(const UiActionOwnerContext &owner, const std::uint64_t previousRequest) noexcept {
        auto *const storage = StateStorage();
        storage->owner = owner;
        storage->lastRequestSequence = previousRequest;
        storage->active = true;
        for (auto &record : storage->records) {
            record->snapshot = {};
            record->cancelled.store(false);
        }
    }

    /** @copydoc UiAsyncActionStore::DrainReplacementSource */
    std::size_t UiAsyncActionStore::DrainReplacementSource() noexcept {
        auto *const storage = StateStorage();
        if (!storage || storage->ownerThread != std::this_thread::get_id())
            return 0;
        std::size_t drained{};
        for (auto &record : storage->records) {
            if (record.use_count() == 1 && !record->retained && record->snapshot.error) {
                record->snapshot.error.reset();
                ++drained;
            }
        }
        return drained;
    }

}  // namespace Horo::Runtime::Ui
