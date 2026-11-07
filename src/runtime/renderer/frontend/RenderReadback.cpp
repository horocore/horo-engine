#include "Horo/Runtime/Render/RenderReadback.h"

#include "BoundedRenderQueueCore.h"
#include "Horo/Runtime/Render/RenderReadbackErrors.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Render {
    namespace {
        [[nodiscard]] Error ReadbackError(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            return MakeError(descriptor, std::move(message));
        }

        [[nodiscard]] bool IsSourceValid(const RenderReadbackSource &source) noexcept {
            return std::visit([](const auto handle) {
                return handle.IsValid();
            }, source);
        }

        [[nodiscard]] RenderResourceOwnerId SourceOwner(const RenderReadbackSource &source) noexcept {
            return std::visit([](const auto handle) {
                return handle.owner;
            }, source);
        }

        [[nodiscard]] bool IsTerminal(const RenderReadbackState state) noexcept {
            using enum RenderReadbackState;
            return state == Failed || state == Cancelled || state == TimedOut;
        }

        struct RetainedAccounting {
            std::atomic_size_t bytes{0};
        };

        struct RetainedPayload {
            RetainedPayload(std::shared_ptr<RetainedAccounting> owner, std::vector<std::byte> mapped)
                : accounting(std::move(owner)), bytes(std::move(mapped)) {
                accounting->bytes.fetch_add(bytes.size(), std::memory_order_relaxed);
            }

            ~RetainedPayload() {
                accounting->bytes.fetch_sub(bytes.size(), std::memory_order_relaxed);
            }

            RetainedPayload(const RetainedPayload &) = delete;
            RetainedPayload &operator=(const RetainedPayload &) = delete;
            RetainedPayload(RetainedPayload &&) = delete;
            RetainedPayload &operator=(RetainedPayload &&) = delete;

            std::shared_ptr<RetainedAccounting> accounting;
            std::vector<std::byte> bytes;
        };

        struct ReadbackRecord {
            RenderReadbackId id;
            RenderReadbackDescriptor descriptor;
            RenderReadbackState state{RenderReadbackState::Pending};
            RenderTimelinePoint completion;
            std::shared_ptr<const RetainedPayload> payload;
            std::shared_ptr<detail::PreparedReadbackPayload> prepared;
            std::optional<Error> failure;
            bool pendingBytesCharged{true};
            bool preparedAcquired{};
        };

        struct ReadyReadback {
            RenderReadbackId id;
            RenderTimelinePoint completion;
            std::shared_ptr<const RetainedPayload> payload;
            std::shared_ptr<const detail::PreparedReadbackPayload> prepared;
        };

        [[nodiscard]] Error ReadbackAdmissionError(const detail::QueueAdmissionFailure failure) {
            using enum detail::QueueAdmissionFailure;
            switch (failure) {
                case Capacity:
                    return ReadbackError(RenderReadbackErrors::CapacityExceeded,
                                         "Readback metadata, staging, or retained-result capacity is exhausted.");
                case Identity:
                    return ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback request identity space is exhausted.");
                case Allocation:
                    return ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback request storage allocation failed.");
                case Length:
                    return ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback metadata capacity cannot be represented.");
            }
            return ReadbackError(RenderReadbackErrors::CapacityExceeded);
        }
    }  // namespace

    namespace detail {
        /** @brief One cold-owned aligned arena; mutable only while no consumer lease can reference its generation. */
        struct PreparedReadbackPayload final {
            PreparedReadbackPayload(std::shared_ptr<RetainedAccounting> owner, std::size_t maximum, std::size_t alignment)
                : accounting(std::move(owner)), storage(maximum + alignment - 1) {
                void *pointer = storage.data();
                auto space = storage.size();
                const auto *aligned = static_cast<std::byte *>(std::align(alignment, maximum, pointer, space));
                offset = static_cast<std::size_t>(aligned - storage.data());
            }

            ~PreparedReadbackPayload() {
                ReleaseCharge();
            }

            /** @brief Releases only logical result backpressure; physical arena storage remains reserved while the pool lives. */
            void ReleaseCharge() noexcept {
                if (charged) {
                    accounting->bytes.fetch_sub(count, std::memory_order_relaxed);
                    charged = false;
                }
            }

            /** @brief Returns exactly this request's admitted bytes, excluding padding and previous request tails. */
            [[nodiscard]] std::span<std::byte> Writable() noexcept {
                return {storage.data() + offset, count};
            }

            [[nodiscard]] std::span<const std::byte> Bytes() const noexcept {
                return {storage.data() + offset, count};
            }

            std::shared_ptr<RetainedAccounting> accounting;
            std::vector<std::byte> storage;
            std::size_t offset{}, count{};
            RenderReadbackId id;
            RenderTimelinePoint completion;
            bool charged{};
            mutable std::atomic_uint32_t leases{0};
        };
    }  // namespace detail

    /** @copydoc RenderReadbackDescriptor::IsValid */
    bool RenderReadbackDescriptor::IsValid() const noexcept {
        return IsSourceValid(source) && byteCount > 0 && sourceByteOffset <= std::numeric_limits<std::size_t>::max() - byteCount &&
               alignment > 0 && (alignment & (alignment - 1U)) == 0 && timeout.count() > 0;
    }

    class RenderReadbackResult::Impl final {
    public:
        Impl(const RenderReadbackId request, const RenderTimelinePoint completionPoint,
             std::shared_ptr<const RetainedPayload> retainedPayload) noexcept
            : id(request), completion(completionPoint), payload(std::move(retainedPayload)) {}

        RenderReadbackId id;
        RenderTimelinePoint completion;
        std::shared_ptr<const RetainedPayload> payload;
    };

    /** @copydoc RenderReadbackResult::~RenderReadbackResult */
    RenderReadbackResult::RenderReadbackResult() noexcept = default;

    RenderReadbackResult::~RenderReadbackResult() {
        ReleasePreparedLease();
    }

    RenderReadbackResult::RenderReadbackResult(RenderReadbackResult &&) noexcept = default;

    RenderReadbackResult &RenderReadbackResult::operator=(RenderReadbackResult &&other) noexcept {
        if (this != &other) {
            ReleasePreparedLease();
            implementation_ = std::move(other.implementation_);
            prepared_ = std::move(other.prepared_);
        }
        return *this;
    }

    /** @brief Publishes consumer quiescence before the owner may reuse aligned prepared bytes. */
    void RenderReadbackResult::ReleasePreparedLease() noexcept {
        if (prepared_)
            prepared_->leases.fetch_sub(1, std::memory_order_release);
    }

    RenderReadbackResult::RenderReadbackResult(std::unique_ptr<Impl> implementation) noexcept
        : implementation_(std::move(implementation)) {}

    RenderReadbackResult::RenderReadbackResult(std::shared_ptr<const detail::PreparedReadbackPayload> prepared) noexcept
        : prepared_(std::move(prepared)) {
        prepared_->leases.fetch_add(1, std::memory_order_relaxed);
    }

    /** @copydoc RenderReadbackResult::Id */
    RenderReadbackId RenderReadbackResult::Id() const noexcept {
        return prepared_ ? prepared_->id : (implementation_ == nullptr ? RenderReadbackId{} : implementation_->id);
    }

    /** @copydoc RenderReadbackResult::Completion */
    RenderTimelinePoint RenderReadbackResult::Completion() const noexcept {
        return prepared_ ? prepared_->completion : (implementation_ == nullptr ? RenderTimelinePoint{} : implementation_->completion);
    }

    /** @copydoc RenderReadbackResult::Bytes */
    std::span<const std::byte> RenderReadbackResult::Bytes() const noexcept {
        if (prepared_)
            return prepared_->Bytes();
        if (implementation_ == nullptr || implementation_->payload == nullptr)
            return {};
        return implementation_->payload->bytes;
    }

    class RenderReadbackQueue::Impl final {
        using enum RenderReadbackState;
        using Core = detail::BoundedRenderQueueCore<ReadbackRecord, RenderReadbackId, RenderReadbackLimits>;
        using OperationAdapter = detail::QueueOperationAdapter<Core>;
        using Status = RenderReadbackPreparedStatus;

    public:
        Impl(const RenderResourceOwnerId renderer, const RenderReadbackLimits &limits)
            : retained_(std::make_shared<RetainedAccounting>()), core_(renderer, limits),
              operations_(core_, RenderReadbackErrors::WrongThread, RenderReadbackErrors::InvalidRequest,
                          "Readback identity is malformed, foreign, or no longer tracked.") {}

        /** @brief Allocates all prepared payload and ownership slots before queue admission begins. */
        [[nodiscard]] Result<std::size_t> PreparePayloadStorage(RenderReadbackPreparedStorage storage, std::size_t maximumBytes) {
            using Output = Result<std::size_t>;
            if (!core_.OnOwnerThread())
                return Output::Failure(ReadbackError(RenderReadbackErrors::WrongThread));
            if (!CanPrepareStorage(storage, maximumBytes))
                return Output::Failure(
                    ReadbackError(RenderReadbackErrors::InvalidConfiguration,
                                  "Prepare finite readback storage once on an empty queue within its complete CPU payload reservation."));
            try {
                std::vector<std::shared_ptr<detail::PreparedReadbackPayload>> candidate;
                candidate.reserve(storage.slotCount);
                for (std::uint32_t i = 0; i < storage.slotCount; ++i)
                    candidate.push_back(
                        std::make_shared<detail::PreparedReadbackPayload>(retained_, storage.maximumBytesPerResult, storage.alignment));
                const auto bytes = PoolPayloadBytes(candidate);
                if (bytes > maximumBytes)
                    return Output::Failure(ReadbackError(RenderReadbackErrors::CapacityExceeded));
                preparedPool_ = std::move(candidate);
                preparedStorage_ = storage;
                return Output::Success(bytes);
            } catch (const std::bad_alloc &) {
                return Output::Failure(ReadbackError(RenderReadbackErrors::CapacityExceeded));
            } catch (const std::length_error &) {
                return Output::Failure(ReadbackError(RenderReadbackErrors::CapacityExceeded));
            }
        }

        /** @brief Charges actual vector capacities and fixed objects, without allocator/control-block or native-driver estimates. */
        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept {
            return PoolPayloadBytes(preparedPool_);
        }

        /** @brief Reclaims only fully unreferenced generations, with an explicit acquire from consumer lease release. */
        [[nodiscard]] std::size_t CollectPreparedResults() noexcept {
            if (!core_.OnOwnerThread())
                return 0;
            std::size_t count{};
            for (auto &slot : preparedPool_) {
                if (slot->leases.load(std::memory_order_acquire) != 0 || slot.use_count() != 1 || !slot->id.IsValid())
                    continue;
                slot->ReleaseCharge();
                slot->id = {};
                slot->completion = {};
                slot->count = 0;
                ++count;
            }
            return count;
        }

        /** @brief Admits expected pressure without constructing heap-backed errors or another request identity source. */
        [[nodiscard]] RenderReadbackPreparedRequest RequestPrepared(const RenderReadbackDescriptor &descriptor) noexcept {
            if (!core_.OnOwnerThread())
                return {Status::WrongThread, {}};
            if (!core_.Accepting())
                return {Status::Stopped, {}};
            if (!ValidPreparedDescriptor(descriptor))
                return {Status::InvalidDescriptor, {}};
            (void)CollectPreparedResults();
            const auto free = std::ranges::find_if(preparedPool_, [](const auto &slot) {
                return !slot->id.IsValid();
            });
            RenderReadbackId id;
            detail::QueueAdmissionFailure failure{};
            if (!core_.TryAdmitPrepared(free != preparedPool_.end() && HasResultCapacity(descriptor.byteCount),
                                        [&descriptor, &free](RenderReadbackId request) noexcept {
                ReadbackRecord record{.id = request, .descriptor = descriptor};
                record.prepared = *free;
                return record;
            }, id, failure))
                return {Status::CapacityExceeded, {}};
            auto &slot = **free;
            slot.id = id;
            slot.count = descriptor.byteCount;
            slot.completion = {};
            core_.ChargePending(descriptor.byteCount);
            return {Status::Complete, id};
        }

        /** @brief Exposes only the exact nonterminal submitted request's aligned completion destination. */
        [[nodiscard]] RenderReadbackPreparedMapping WritablePrepared(RenderReadbackId id) noexcept {
            ReadbackRecord *record{};
            const auto status = FindPrepared(id, record);
            if (status != Status::Complete)
                return {status, {}};
            if (record->state != Submitted)
                return {Status::InvalidTransition, {}};
            return {Status::Complete, record->prepared->Writable()};
        }

        /** @brief Transfers pending charge into immutable result charge only after exact reached completion equality. */
        [[nodiscard]] Status CompletePrepared(RenderReadbackId id, RenderTimelinePoint reached) noexcept {
            ReadbackRecord *record{};
            const auto status = FindPrepared(id, record);
            if (status != Status::Complete)
                return status;
            if (!reached.IsValid() || reached != record->completion)
                return Status::InvalidTransition;
            if (record->state == Cancelled || record->state == TimedOut) {
                ReleasePending(*record);
                return Status::Complete;
            }
            if (record->state != Submitted)
                return Status::InvalidTransition;
            auto &slot = *record->prepared;
            slot.completion = reached;
            slot.charged = true;
            retained_->bytes.fetch_add(slot.count, std::memory_order_relaxed);
            ReleasePending(*record);
            record->state = Ready;
            return Status::Complete;
        }

        /** @brief Moves the immutable generation lease from the record; storage cannot be reused while consumers retain it. */
        [[nodiscard]] Status AcquirePrepared(RenderReadbackId id, std::shared_ptr<const detail::PreparedReadbackPayload> &output) noexcept {
            ReadbackRecord *record{};
            const auto status = FindPrepared(id, record);
            if (status != Status::Complete)
                return status;
            const auto state = PreparedState(record->state);
            if (state != Status::Complete)
                return state;
            if (record->preparedAcquired)
                return Status::InvalidTransition;
            output = record->prepared;
            record->preparedAcquired = true;
            return Status::Complete;
        }

        /** @brief Returns an exact issued retirement fact only after acquired-result consumer quiescence. */
        [[nodiscard]] Status RetirePreparedResult(RenderReadbackId id) noexcept {
            ReadbackRecord *record{};
            const auto status = FindPrepared(id, record);
            if (status != Status::Complete)
                return status;
            if (record->state != Ready || !record->preparedAcquired)
                return Status::InvalidTransition;
            auto &slot = *record->prepared;
            if (slot.leases.load(std::memory_order_acquire) != 0 || record->prepared.use_count() != 2)
                return Status::ResultPending;
            slot.ReleaseCharge();
            slot.id = {};
            slot.count = 0;
            slot.completion = {};
            core_.Erase(id);
            return Status::Complete;
        }

        [[nodiscard]] Result<RenderReadbackId> Request(const RenderReadbackDescriptor &descriptor) {
            if (!core_.OnOwnerThread())
                return Result<RenderReadbackId>::Failure(ReadbackError(RenderReadbackErrors::WrongThread));
            if (!core_.Accepting())
                return Result<RenderReadbackId>::Failure(ReadbackError(RenderReadbackErrors::Stopped, "Readback admission is stopped."));
            const RenderReadbackLimits &limits = core_.LimitsValue();
            if (!descriptor.IsValid() || SourceOwner(descriptor.source) != core_.Renderer() ||
                descriptor.byteCount > limits.maximumRequestBytes || descriptor.alignment > limits.maximumAlignment) {
                return Result<RenderReadbackId>::Failure(
                    ReadbackError(RenderReadbackErrors::InvalidDescriptor,
                                  "Readback descriptor exceeds the configured byte or alignment bounds."));
            }
            const std::size_t retainedBytes = retained_->bytes.load(std::memory_order_relaxed);
            const bool pendingCapacityAvailable = descriptor.byteCount <= limits.maximumPendingBytes - core_.PendingBytes();
            const bool retainedCapacityAvailable =
                retainedBytes <= limits.maximumRetainedResultBytes &&
                core_.PendingBytes() <= limits.maximumRetainedResultBytes - retainedBytes &&
                descriptor.byteCount <= limits.maximumRetainedResultBytes - retainedBytes - core_.PendingBytes();
            auto admitted = core_.Admit(pendingCapacityAvailable && retainedCapacityAvailable, [descriptor](const RenderReadbackId id) {
                return ReadbackRecord{.id = id, .descriptor = descriptor};
            }, ReadbackAdmissionError);
            if (admitted.HasValue())
                core_.ChargePending(descriptor.byteCount);
            return admitted;
        }

        [[nodiscard]] Result<void> MarkSubmitted(const RenderReadbackId request, const RenderTimelinePoint completion) {
            return operations_.Apply<Result<void>>(request, [completion, this](auto &record) {
                if (record.state != Pending || !completion.IsValid())
                    return InvalidTransition("Only a pending readback may receive a valid completion point.");
                record.state = Submitted;
                record.completion = completion;
                return Result<void>::Success();
            });
        }

        [[nodiscard]] Result<void> Complete(const RenderReadbackId request, const std::span<const std::byte> mappedBytes) {
            return operations_.Apply<Result<void>>(request, [this, mappedBytes](auto &record) {
                return CompleteRecord(record, mappedBytes);
            });
        }

        [[nodiscard]] Result<void> Fail(const RenderReadbackId request, const Error &error) {
            return operations_.Apply<Result<void>>(request, [this, &error](auto &record) {
                if (record.state == Cancelled || record.state == TimedOut) {
                    ReleasePending(record);
                    return Result<void>::Success();
                }
                if (record.state != Pending && record.state != Submitted)
                    return InvalidTransition("Only pending or submitted readback work may fail.");
                ReleasePending(record);
                record.failure = error;
                record.state = Failed;
                return Result<void>::Success();
            });
        }

        [[nodiscard]] Result<void> Cancel(const RenderReadbackId request) {
            return operations_.Apply<Result<void>>(request, [this](auto &record) {
                return detail::TransitionQueueRecord(record, Cancelled, Pending, Submitted, [this] {
                    return InvalidTransition("Only pending or submitted readback work may be cancelled.");
                }, [this](auto &pendingRecord) {
                    ReleasePending(pendingRecord);
                });
            });
        }

        [[nodiscard]] Result<void> Timeout(const RenderReadbackId request) {
            return operations_.Apply<Result<void>>(request, [this](auto &record) {
                return detail::TransitionQueueRecord(record, TimedOut, Pending, Submitted, [this] {
                    return InvalidTransition("Only pending or submitted readback work may time out.");
                }, [this](auto &pendingRecord) {
                    ReleasePending(pendingRecord);
                });
            });
        }

        [[nodiscard]] Result<void> Retire(const RenderReadbackId request) {
            return operations_.Apply<Result<void>>(request, [this](auto &record) {
                if (record.state != Cancelled && record.state != TimedOut)
                    return InvalidTransition("Only cancelled or timed-out submitted work may retire without publication.");
                ReleasePending(record);
                return Result<void>::Success();
            });
        }

        [[nodiscard]] Result<RenderReadbackState> State(const RenderReadbackId request) const {
            return operations_.Apply<Result<RenderReadbackState>>(request, [this](const auto &record) {
                return Result<RenderReadbackState>::Success(record.state);
            });
        }

        [[nodiscard]] Result<ReadyReadback> AcquireView(const RenderReadbackId request) const {
            return operations_.Apply<Result<ReadyReadback>>(request, [](const auto &record) {
                if (record.state == Pending || record.state == Submitted)
                    return Result<ReadyReadback>::Failure(
                        ReadbackError(RenderReadbackErrors::ResultPending, "Readback result has not completed."));
                if (record.state == Cancelled)
                    return Result<ReadyReadback>::Failure(ReadbackError(RenderReadbackErrors::Cancelled, "Readback was cancelled."));
                if (record.state == TimedOut)
                    return Result<ReadyReadback>::Failure(ReadbackError(RenderReadbackErrors::TimedOut, "Readback timed out."));
                if (record.state == Failed)
                    return Result<ReadyReadback>::Failure(*record.failure);
                if (record.preparedAcquired)
                    return Result<ReadyReadback>::Failure(
                        ReadbackError(RenderReadbackErrors::InvalidTransition,
                                      "Prepared result was already acquired; acknowledge retirement after its consumer lease releases."));
                return Result<ReadyReadback>::Success({record.id, record.completion, record.payload, record.prepared});
            });
        }

        void ConsumeReady(const RenderReadbackId request) noexcept {
            auto *record = core_.Find(request);
            if (record == nullptr || record->state != Ready)
                return;
            if (record->prepared)
                record->preparedAcquired = true;
            else
                core_.Erase(request);
        }

        [[nodiscard]] Result<void> Discard(const RenderReadbackId request) {
            const auto result = operations_.Apply<Result<void>>(request, [this](const auto &record) {
                if (!IsTerminal(record.state) || record.pendingBytesCharged)
                    return this->InvalidTransition("Only fully retired terminal readbacks may be discarded.");
                return Result<void>::Success();
            });
            if (result.HasValue())
                core_.Erase(request);
            return result;
        }

        [[nodiscard]] RenderReadbackSnapshot Snapshot() const noexcept {
            return detail::MakeQueueSnapshot<RenderReadbackSnapshot>(core_, Submitted, Ready, IsTerminal, [this](auto &snapshot) {
                snapshot.pendingBytes = core_.PendingBytes();
                snapshot.retainedResultBytes = retained_->bytes.load(std::memory_order_relaxed);
            });
        }

        void StopAdmission() noexcept {
            operations_.StopAdmission();
        }

        void Shutdown() noexcept {
            operations_.Shutdown();
            (void)CollectPreparedResults();
        }

    private:
        /** @brief Includes the single queue authority, reserved records, accounting and every actual prepared arena capacity. */
        [[nodiscard]] std::size_t PoolPayloadBytes(
            const std::vector<std::shared_ptr<detail::PreparedReadbackPayload>> &pool) const noexcept {
            std::size_t bytes = sizeof(RenderReadbackQueue) + sizeof(Impl) + sizeof(RetainedAccounting);
            if (!AddPayload(bytes, core_.RecordStoragePayloadBytes()))
                return SIZE_MAX;
            const auto inventory = pool.capacity();
            if (!AddPayload(bytes, inventory * sizeof(std::shared_ptr<detail::PreparedReadbackPayload>)))
                return SIZE_MAX;
            for (const auto &slot : pool)
                if (!AddPayload(bytes, sizeof(detail::PreparedReadbackPayload)) || !AddPayload(bytes, slot->storage.capacity()))
                    return SIZE_MAX;
            return bytes;
        }

        /** @brief Adds payload extents using subtraction bounds so accounting never wraps. */
        static bool AddPayload(std::size_t &bytes, std::size_t extra) noexcept {
            if (extra > SIZE_MAX - bytes)
                return false;
            bytes += extra;
            return true;
        }

        /** @brief Keeps the prepared reservation within existing request/alignment and retained-result policy. */
        [[nodiscard]] bool ValidStorage(RenderReadbackPreparedStorage s) const noexcept {
            const auto &l = core_.LimitsValue();
            return s.slotCount && s.slotCount <= l.maximumRequests && s.maximumBytesPerResult &&
                   s.maximumBytesPerResult <= l.maximumRequestBytes && s.alignment && (s.alignment & (s.alignment - 1)) == 0 &&
                   s.alignment <= l.maximumAlignment && s.slotCount <= l.maximumRetainedResultBytes / s.maximumBytesPerResult;
        }

        /** @brief Rejects configuration before allocation if known minimum payload already exceeds the host allowance. */
        [[nodiscard]] bool CanPrepareStorage(RenderReadbackPreparedStorage s, std::size_t maximum) const noexcept {
            if (!ValidStorage(s) || !core_.Accepting() || !preparedPool_.empty() || retained_->bytes.load(std::memory_order_relaxed) != 0)
                return false;
            if (core_.Count(Submitted, Ready, IsTerminal).requestCount != 0)
                return false;
            const auto base = RetainedPayloadBytes();
            if (base > maximum)
                return false;
            std::size_t perSlot = sizeof(detail::PreparedReadbackPayload) + sizeof(std::shared_ptr<detail::PreparedReadbackPayload>);
            if (!AddPayload(perSlot, s.maximumBytesPerResult) || !AddPayload(perSlot, s.alignment - 1))
                return false;
            return s.slotCount <= (maximum - base) / perSlot;
        }

        /** @brief Applies the same source owner, range and alignment policy before no-allocation prepared admission. */
        [[nodiscard]] bool ValidPreparedDescriptor(const RenderReadbackDescriptor &d) const noexcept {
            return d.IsValid() && SourceOwner(d.source) == core_.Renderer() && !preparedPool_.empty() &&
                   d.byteCount <= preparedStorage_.maximumBytesPerResult && d.alignment <= preparedStorage_.alignment;
        }

        /** @brief Shares pending-plus-retained backpressure across legacy and prepared requests. */
        [[nodiscard]] bool HasResultCapacity(std::size_t count) const noexcept {
            const auto &limits = core_.LimitsValue();
            const auto retained = retained_->bytes.load(std::memory_order_relaxed);
            if (count > limits.maximumPendingBytes - core_.PendingBytes())
                return false;
            if (retained > limits.maximumRetainedResultBytes || core_.PendingBytes() > limits.maximumRetainedResultBytes - retained)
                return false;
            return count <= limits.maximumRetainedResultBytes - retained - core_.PendingBytes();
        }

        /** @brief Resolves the existing generation-safe record and refuses legacy requests in the prepared route. */
        [[nodiscard]] Status FindPrepared(RenderReadbackId id, ReadbackRecord *&record) noexcept {
            if (!core_.OnOwnerThread())
                return Status::WrongThread;
            record = core_.Find(id);
            if (!record || !record->prepared)
                return Status::InvalidRequest;
            return Status::Complete;
        }

        /** @brief Maps lifecycle evidence to allocation-free acquisition outcomes without discarding original generic errors. */
        [[nodiscard]] static Status PreparedState(RenderReadbackState state) noexcept {
            switch (state) {
                case Pending:
                case Submitted:
                    return Status::ResultPending;
                case Ready:
                    return Status::Complete;
                case Cancelled:
                    return Status::Cancelled;
                case TimedOut:
                    return Status::TimedOut;
                case Failed:
                    return Status::Failed;
            }
            return Status::InvalidTransition;
        }

        [[nodiscard]] Result<void> CompleteRecord(ReadbackRecord &record, const std::span<const std::byte> mappedBytes) {
            if (record.state != Submitted && record.state != Cancelled && record.state != TimedOut)
                return InvalidTransition("Only submitted, cancelled, or timed-out backend work may complete.");
            if (record.state == Cancelled || record.state == TimedOut) {
                ReleasePending(record);
                return Result<void>::Success();
            }
            if (!record.completion.IsValid() || mappedBytes.size() != record.descriptor.byteCount)
                return Result<void>::Failure(ReadbackError(RenderReadbackErrors::MappingSizeMismatch,
                                                           "Mapped readback bytes do not match the exact admitted request size."));
            if (record.prepared)
                return InvalidTransition("Prepared readbacks require CompletePrepared without an intermediate mapping copy.");
            return CopyMappedResult(record, mappedBytes);
        }

        /** @brief Owns legacy mapping-copy allocation; the prepared route never enters this helper. */
        [[nodiscard]] Result<void> CopyMappedResult(ReadbackRecord &record, std::span<const std::byte> mappedBytes) {
            if (const std::size_t retainedBytes = retained_->bytes.load(std::memory_order_relaxed);
                retainedBytes > core_.LimitsValue().maximumRetainedResultBytes ||
                mappedBytes.size() > core_.LimitsValue().maximumRetainedResultBytes - retainedBytes)
                return Result<void>::Failure(
                    ReadbackError(RenderReadbackErrors::CapacityExceeded, "Consumer-retained readback results leave no result capacity."));
            try {
                std::vector<std::byte> owned(mappedBytes.begin(), mappedBytes.end());
                record.payload = std::make_shared<RetainedPayload>(retained_, std::move(owned));
            } catch (const std::bad_alloc &) {
                return Result<void>::Failure(
                    ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback result storage allocation failed."));
            } catch (const std::length_error &) {
                return Result<void>::Failure(
                    ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback result size cannot be represented."));
            }
            ReleasePending(record);
            record.state = Ready;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> InvalidTransition(std::string message) const {
            return Result<void>::Failure(ReadbackError(RenderReadbackErrors::InvalidTransition, std::move(message)));
        }

        void ReleasePending(ReadbackRecord &record) noexcept {
            core_.ReleasePending(record);
        }

        std::shared_ptr<RetainedAccounting> retained_{std::make_shared<RetainedAccounting>()};
        Core core_;
        OperationAdapter operations_;
        RenderReadbackPreparedStorage preparedStorage_;
        std::vector<std::shared_ptr<detail::PreparedReadbackPayload>> preparedPool_;
    };

    /** @copydoc RenderReadbackQueue::Create */
    Result<std::unique_ptr<RenderReadbackQueue>> RenderReadbackQueue::Create(const RenderResourceOwnerId renderer,
                                                                             const RenderReadbackLimits &limits) {
        if (!renderer.IsValid() || !limits.IsValid())
            return Result<std::unique_ptr<RenderReadbackQueue>>::Failure(
                ReadbackError(RenderReadbackErrors::InvalidConfiguration, "Readback owner identity or finite limits are invalid."));
        try {
            // std::make_unique cannot access RenderReadbackQueue's private constructor.
            return Result<std::unique_ptr<RenderReadbackQueue>>::Success(
                std::unique_ptr<RenderReadbackQueue>(new RenderReadbackQueue(std::make_unique<Impl>(renderer, limits))));  // NOSONAR
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<RenderReadbackQueue>>::Failure(
                ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback queue allocation failed."));
        } catch (const std::length_error &) {
            return Result<std::unique_ptr<RenderReadbackQueue>>::Failure(
                ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback metadata capacity cannot be represented."));
        }
    }

    RenderReadbackQueue::RenderReadbackQueue(std::unique_ptr<Impl> implementation) noexcept : implementation_(std::move(implementation)) {}

    RenderReadbackQueue::~RenderReadbackQueue() = default;

    /** @copydoc RenderReadbackQueue::PreparePayloadStorage */
    Result<std::size_t> RenderReadbackQueue::PreparePayloadStorage(RenderReadbackPreparedStorage storage, std::size_t maximum) {
        return implementation_->PreparePayloadStorage(storage, maximum);
    }

    /** @copydoc RenderReadbackQueue::RetainedPayloadBytes */
    std::size_t RenderReadbackQueue::RetainedPayloadBytes() const noexcept {
        return implementation_->RetainedPayloadBytes();
    }

    /** @copydoc RenderReadbackQueue::RequestPrepared */
    RenderReadbackPreparedRequest RenderReadbackQueue::RequestPrepared(const RenderReadbackDescriptor &d) noexcept {
        return implementation_->RequestPrepared(d);
    }

    /** @copydoc RenderReadbackQueue::WritablePrepared */
    RenderReadbackPreparedMapping RenderReadbackQueue::WritablePrepared(RenderReadbackId id) noexcept {
        return implementation_->WritablePrepared(id);
    }

    /** @copydoc RenderReadbackQueue::CompletePrepared */
    RenderReadbackPreparedStatus RenderReadbackQueue::CompletePrepared(RenderReadbackId id, RenderTimelinePoint reached) noexcept {
        return implementation_->CompletePrepared(id, reached);
    }

    /** @copydoc RenderReadbackQueue::AcquirePrepared */
    RenderReadbackPreparedStatus RenderReadbackQueue::AcquirePrepared(RenderReadbackId id, RenderReadbackResult &output) noexcept {
        std::shared_ptr<const detail::PreparedReadbackPayload> payload;
        const auto status = implementation_->AcquirePrepared(id, payload);
        if (status == RenderReadbackPreparedStatus::Complete)
            output = RenderReadbackResult(std::move(payload));
        return status;
    }

    /** @copydoc RenderReadbackQueue::RetirePreparedResult */
    RenderReadbackPreparedStatus RenderReadbackQueue::RetirePreparedResult(RenderReadbackId id) noexcept {
        return implementation_->RetirePreparedResult(id);
    }

    /** @copydoc RenderReadbackQueue::CollectPreparedResults */
    std::size_t RenderReadbackQueue::CollectPreparedResults() noexcept {
        return implementation_->CollectPreparedResults();
    }

    /** @copydoc RenderReadbackQueue::Request */
    Result<RenderReadbackId> RenderReadbackQueue::Request(const RenderReadbackDescriptor &descriptor) {
        return implementation_->Request(descriptor);
    }

    /** @copydoc RenderReadbackQueue::MarkSubmitted */
    Result<void> RenderReadbackQueue::MarkSubmitted(const RenderReadbackId request, const RenderTimelinePoint completion) {
        return implementation_->MarkSubmitted(request, completion);
    }

    /** @copydoc RenderReadbackQueue::Complete */
    Result<void> RenderReadbackQueue::Complete(const RenderReadbackId request, const std::span<const std::byte> mappedBytes) {
        return implementation_->Complete(request, mappedBytes);
    }

    /** @copydoc RenderReadbackQueue::Fail */
    Result<void> RenderReadbackQueue::Fail(const RenderReadbackId request, const Error &error) {
        return implementation_->Fail(request, error);
    }

    /** @copydoc RenderReadbackQueue::Cancel */
    Result<void> RenderReadbackQueue::Cancel(const RenderReadbackId request) {
        return implementation_->Cancel(request);
    }

    /** @copydoc RenderReadbackQueue::Timeout */
    Result<void> RenderReadbackQueue::Timeout(const RenderReadbackId request) {
        return implementation_->Timeout(request);
    }

    /** @copydoc RenderReadbackQueue::Retire */
    Result<void> RenderReadbackQueue::Retire(const RenderReadbackId request) {
        return implementation_->Retire(request);
    }

    /** @copydoc RenderReadbackQueue::State */
    Result<RenderReadbackState> RenderReadbackQueue::State(const RenderReadbackId request) const {
        return implementation_->State(request);
    }

    /** @copydoc RenderReadbackQueue::Acquire */
    Result<RenderReadbackResult> RenderReadbackQueue::Acquire(const RenderReadbackId request) {
        auto ready = implementation_->AcquireView(request);
        if (ready.HasError())
            return Result<RenderReadbackResult>::Failure(ready.ErrorValue());
        if (ready.Value().prepared) {
            RenderReadbackResult result(ready.Value().prepared);
            implementation_->ConsumeReady(request);
            return Result<RenderReadbackResult>::Success(std::move(result));
        }
        try {
            auto resultImplementation =
                std::make_unique<RenderReadbackResult::Impl>(ready.Value().id, ready.Value().completion, ready.Value().payload);
            RenderReadbackResult result(std::move(resultImplementation));
            implementation_->ConsumeReady(request);
            return Result<RenderReadbackResult>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Result<RenderReadbackResult>::Failure(
                ReadbackError(RenderReadbackErrors::CapacityExceeded, "Readback lease allocation failed."));
        }
    }

    /** @copydoc RenderReadbackQueue::Discard */
    Result<void> RenderReadbackQueue::Discard(const RenderReadbackId request) {
        return implementation_->Discard(request);
    }

    /** @copydoc RenderReadbackQueue::Snapshot */
    RenderReadbackSnapshot RenderReadbackQueue::Snapshot() const noexcept {
        return implementation_->Snapshot();
    }

    /** @copydoc RenderReadbackQueue::StopAdmission */
    void RenderReadbackQueue::StopAdmission() noexcept {
        implementation_->StopAdmission();
    }

    /** @copydoc RenderReadbackQueue::Shutdown */
    void RenderReadbackQueue::Shutdown() noexcept {
        implementation_->Shutdown();
    }
}  // namespace Horo::Render
