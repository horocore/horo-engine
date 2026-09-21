#pragma once

#include "Horo/Foundation/ErrorCode.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderResource.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Render::detail {
    enum class QueueAdmissionFailure {
        Capacity,
        Identity,
        Allocation,
        Length,
    };

    struct QueueSnapshotCounts {
        std::uint32_t requestCount{0};
        std::uint32_t submittedCount{0};
        std::uint32_t readyCount{0};
        std::uint32_t terminalCount{0};
    };

    template <typename Snapshot>
    void PopulateQueueSnapshot(Snapshot &snapshot, const QueueSnapshotCounts counts, const std::uint64_t failedAdmissionCount,
                               const bool acceptingRequests) noexcept {
        snapshot.requestCount = counts.requestCount;
        snapshot.submittedCount = counts.submittedCount;
        snapshot.readyCount = counts.readyCount;
        snapshot.terminalCount = counts.terminalCount;
        snapshot.failedAdmissionCount = failedAdmissionCount;
        snapshot.acceptingRequests = acceptingRequests;
    }

    template <typename Snapshot, typename CoreType, typename State, typename TerminalPredicate, typename Customize>
    [[nodiscard]] Snapshot MakeQueueSnapshot(const CoreType &core, const State submitted, const State ready, TerminalPredicate terminal,
                                             Customize &&customize) noexcept {
        Snapshot snapshot{};
        std::forward<Customize>(customize)(snapshot);
        PopulateQueueSnapshot(snapshot, core.Count(submitted, ready, terminal), core.FailedAdmissionCount(), core.Accepting());
        return snapshot;
    }

    template <typename State> [[nodiscard]] Result<bool> QueueStateIsReady(const Result<State> &state, const State ready) {
        return state.HasError() ? Result<bool>::Failure(state.ErrorValue()) : Result<bool>::Success(state.Value() == ready);
    }

    template <typename Record, typename Id, typename Limits> class BoundedRenderQueueCore final {
    public:
        using RecordContainer = std::vector<Record>;

        BoundedRenderQueueCore(const RenderResourceOwnerId renderer, const Limits &limits) : renderer_(renderer), limits_(limits) {
            records_.reserve(limits.maximumRequests);
        }

        [[nodiscard]] bool OnOwnerThread() const noexcept {
            return ownerThread_ == std::this_thread::get_id();
        }

        [[nodiscard]] bool Accepting() const noexcept {
            return accepting_;
        }

        [[nodiscard]] RenderResourceOwnerId Renderer() const noexcept {
            return renderer_;
        }

        [[nodiscard]] const Limits &LimitsValue() const noexcept {
            return limits_;
        }

        [[nodiscard]] std::size_t PendingBytes() const noexcept {
            return pendingBytes_;
        }

        void ChargePending(const std::size_t bytes) noexcept {
            pendingBytes_ += bytes;
        }

        [[nodiscard]] std::uint64_t FailedAdmissionCount() const noexcept {
            return failedAdmissionCount_;
        }

        template <typename RecordFactory, typename ErrorFactory>
        [[nodiscard]] Result<Id> Admit(const bool extraCapacityAvailable, RecordFactory &&recordFactory, ErrorFactory &&errorFactory) {
            using enum QueueAdmissionFailure;
            if (!extraCapacityAvailable || records_.size() >= limits_.maximumRequests) {
                ++failedAdmissionCount_;
                return Result<Id>::Failure(errorFactory(Capacity));
            }
            if (nextId_ == std::numeric_limits<std::uint64_t>::max()) {
                ++failedAdmissionCount_;
                return Result<Id>::Failure(errorFactory(Identity));
            }
            try {
                const Id id{renderer_, nextId_++};
                records_.push_back(std::forward<RecordFactory>(recordFactory)(id));
                return Result<Id>::Success(id);
            } catch (const std::bad_alloc &) {
                ++failedAdmissionCount_;
                return Result<Id>::Failure(errorFactory(Allocation));
            } catch (const std::length_error &) {
                ++failedAdmissionCount_;
                return Result<Id>::Failure(errorFactory(Length));
            }
        }

        template <typename Request> [[nodiscard]] auto FindIterator(const Request request) {
            if (!request.IsValid() || request.renderer != renderer_)
                return records_.end();
            return std::ranges::find_if(records_, [request](const Record &record) {
                return record.id == request;
            });
        }

        template <typename Request> [[nodiscard]] auto FindIterator(const Request request) const {
            if (!request.IsValid() || request.renderer != renderer_)
                return records_.cend();
            return std::ranges::find_if(records_, [request](const Record &record) {
                return record.id == request;
            });
        }

        template <typename Request> [[nodiscard]] Record *Find(const Request request) {
            const auto found = FindIterator(request);
            return found == records_.end() ? nullptr : std::to_address(found);
        }

        template <typename Request> [[nodiscard]] const Record *Find(const Request request) const {
            const auto found = FindIterator(request);
            return found == records_.cend() ? nullptr : std::to_address(found);
        }

        template <typename ResultType, typename Request, typename WrongThreadFactory, typename InvalidRequestFactory, typename Operation>
        [[nodiscard]] ResultType Apply(const Request request, WrongThreadFactory &&wrongThread, InvalidRequestFactory &&invalidRequest,
                                       Operation &&operation) {
            return ApplyRecord<ResultType>(OnOwnerThread(), Find(request), std::forward<WrongThreadFactory>(wrongThread),
                                           std::forward<InvalidRequestFactory>(invalidRequest), std::forward<Operation>(operation));
        }

        template <typename ResultType, typename Request, typename WrongThreadFactory, typename InvalidRequestFactory, typename Operation>
        [[nodiscard]] ResultType Apply(const Request request, WrongThreadFactory &&wrongThread, InvalidRequestFactory &&invalidRequest,
                                       Operation &&operation) const {
            return ApplyRecord<ResultType>(OnOwnerThread(), Find(request), std::forward<WrongThreadFactory>(wrongThread),
                                           std::forward<InvalidRequestFactory>(invalidRequest), std::forward<Operation>(operation));
        }

        template <typename Request> void Erase(const Request request) {
            const auto found = FindIterator(request);
            if (found == records_.end())
                return;
            records_.erase(found);
        }

        template <typename State, typename TerminalPredicate>
        [[nodiscard]] QueueSnapshotCounts Count(const State submitted, const State ready, TerminalPredicate terminal) const noexcept {
            QueueSnapshotCounts counts{.requestCount = static_cast<std::uint32_t>(records_.size())};
            for (const Record &record : records_) {
                if (record.state == submitted)
                    ++counts.submittedCount;
                if (record.state == ready)
                    ++counts.readyCount;
                if (terminal(record.state))
                    ++counts.terminalCount;
            }
            return counts;
        }

        template <typename RecordType> void ReleasePending(RecordType &record) noexcept {
            if (!record.pendingBytesCharged)
                return;
            pendingBytes_ -= record.descriptor.byteCount;
            record.pendingBytesCharged = false;
        }

        void StopAdmission() noexcept {
            if (OnOwnerThread())
                accepting_ = false;
        }

        void Shutdown() noexcept {
            if (!OnOwnerThread())
                return;
            accepting_ = false;
            records_.clear();
            pendingBytes_ = 0;
        }

    private:
        template <typename ResultType, typename RecordPointer, typename WrongThreadFactory, typename InvalidRequestFactory,
                  typename Operation>
        [[nodiscard]] static ResultType ApplyRecord(const bool ownerThread, RecordPointer record, WrongThreadFactory &&wrongThread,
                                                    InvalidRequestFactory &&invalidRequest, Operation &&operation) {
            if (!ownerThread)
                return ResultType::Failure(std::forward<WrongThreadFactory>(wrongThread)());
            if (record == nullptr)
                return ResultType::Failure(std::forward<InvalidRequestFactory>(invalidRequest)());
            return std::forward<Operation>(operation)(*record);
        }

        RenderResourceOwnerId renderer_;
        Limits limits_;
        std::thread::id ownerThread_{std::this_thread::get_id()};
        RecordContainer records_;
        std::size_t pendingBytes_{0};
        std::uint64_t nextId_{1};
        std::uint64_t failedAdmissionCount_{0};
        bool accepting_{true};
    };

    template <typename ResultType, typename CoreType, typename Request, typename Operation>
    [[nodiscard]] ResultType ApplyQueueOperation(CoreType &core, const Request request, const ErrorCodeDescriptor &wrongThreadDescriptor,
                                                 const ErrorCodeDescriptor &invalidRequestDescriptor,
                                                 const std::string_view invalidRequestMessage, Operation &&operation) {
        return core.template Apply<ResultType>(request, [&wrongThreadDescriptor] {
            return MakeError(wrongThreadDescriptor);
        }, [&invalidRequestDescriptor, invalidRequestMessage] {
            return MakeError(invalidRequestDescriptor, std::string(invalidRequestMessage));
        }, std::forward<Operation>(operation));
    }

    template <typename Record, typename State, typename InvalidTransitionFactory, typename PendingCleanup>
    [[nodiscard]] Result<void> TransitionQueueRecord(Record &record, const State terminal, const State pending, const State submitted,
                                                     InvalidTransitionFactory &&invalidTransition, PendingCleanup &&pendingCleanup) {
        if (record.state == terminal)
            return Result<void>::Success();
        if (record.state != pending && record.state != submitted)
            return std::forward<InvalidTransitionFactory>(invalidTransition)();
        if (record.state == pending)
            std::forward<PendingCleanup>(pendingCleanup)(record);
        record.state = terminal;
        return Result<void>::Success();
    }

    template <typename CoreType> class QueueOperationAdapter final {
    public:
        QueueOperationAdapter(CoreType &core, const ErrorCodeDescriptor &wrongThreadDescriptor,
                              const ErrorCodeDescriptor &invalidRequestDescriptor, const std::string_view invalidRequestMessage)
            : core_(&core), wrongThreadDescriptor_(wrongThreadDescriptor), invalidRequestDescriptor_(invalidRequestDescriptor),
              invalidRequestMessage_(invalidRequestMessage) {}

        template <typename ResultType, typename Request, typename Operation>
        [[nodiscard]] ResultType Apply(const Request request, Operation &&operation) {
            return Dispatch<ResultType>(*core_, request, std::forward<Operation>(operation));
        }

        template <typename ResultType, typename Request, typename Operation>
        [[nodiscard]] ResultType Apply(const Request request, Operation &&operation) const {
            return Dispatch<ResultType>(static_cast<const CoreType &>(*core_), request, std::forward<Operation>(operation));
        }

        void StopAdmission() noexcept {
            core_->StopAdmission();
        }

        void Shutdown() noexcept {
            core_->Shutdown();
        }

    private:
        template <typename ResultType, typename CoreReference, typename Request, typename Operation>
        [[nodiscard]] ResultType Dispatch(CoreReference &core, const Request request, Operation &&operation) const {
            return ApplyQueueOperation<ResultType>(core, request, wrongThreadDescriptor_, invalidRequestDescriptor_, invalidRequestMessage_,
                                                   std::forward<Operation>(operation));
        }

        CoreType *core_;
        const ErrorCodeDescriptor &wrongThreadDescriptor_;
        const ErrorCodeDescriptor &invalidRequestDescriptor_;
        std::string_view invalidRequestMessage_;
    };
}  // namespace Horo::Render::detail
