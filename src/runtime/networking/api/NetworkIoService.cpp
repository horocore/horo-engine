#include "Horo/Network/NetworkIoService.h"

#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/NetworkMetrics.h"

#include <atomic>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Network {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Fail(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool ValidLimits(const NetworkIoServiceLimits &limits) noexcept {
            return limits.maximumQueuedCompletions > 0 && limits.maximumQueuedCompletions <= MaximumNetworkIoQueuedCompletions &&
                   limits.maximumCompletionsPerPoll > 0 && limits.maximumCompletionsPerPoll <= MaximumNetworkIoCompletionsPerPoll &&
                   limits.maximumCompletionsPerDrain > 0 && limits.maximumCompletionsPerDrain <= MaximumNetworkIoCompletionsPerDrain;
        }

        [[nodiscard]] bool FailureMatchesConnection(const NetworkTerminalRecord &failure, const ConnectionHandle connection) noexcept {
            for (const auto &entry : failure.Context()) {
                if (entry.key == NetworkFailureContextKey::Connection)
                    return std::get<TransportHandleDiagnostic>(entry.value) == connection.Diagnostic();
            }
            return true;
        }
    }  // namespace

    class NetworkIoServiceState final {
    public:
        explicit NetworkIoServiceState(const NetworkIoServiceLimits &configuredLimits,
                                       std::vector<std::optional<NetworkIoCompletion>> preparedRecords,
                                       std::shared_ptr<std::atomic<bool>> admissionGate) noexcept
            : limits(configuredLimits), records(std::move(preparedRecords)), metricAdmission(std::move(admissionGate)) {}

        [[nodiscard]] Result<void> Publish(NetworkIoCompletion completion, const std::uint64_t observedPollGeneration) {
            std::scoped_lock lock{queueMutex};
            if (shuttingDown.load())
                return Fail<void>(NetworkErrors::TransportShuttingDown);
            if (!pollActive || observedPollGeneration != pollGeneration)
                return Fail<void>(NetworkErrors::NetworkIoPollStale);
            if (remainingPollCompletions == 0 || size == limits.maximumQueuedCompletions) {
                if (metricAdmission && metricAdmission->load()) {
                    auto observed = capacityDrops.load();
                    while (observed != std::numeric_limits<std::uint64_t>::max() &&
                           !capacityDrops.compare_exchange_weak(observed, observed + 1)) {
                        // The owner may exchange the counter while this producer retries.
                    }
                }
                return Fail<void>(NetworkErrors::NetworkIoCompletionQueueFull);
            }
            if (nextSequence == 0 || nextSequence == std::numeric_limits<std::uint64_t>::max())
                return Fail<void>(NetworkErrors::NetworkIoSequenceExhausted);
            completion.sequence_ = nextSequence++;
            if (completion.Kind() == NetworkIoCompletionKind::PacketReceived)
                ++inboundPackets;
            records[tail].emplace(std::move(completion));
            tail = (tail + 1) % limits.maximumQueuedCompletions;
            ++size;
            --remainingPollCompletions;
            return Result<void>::Success();
        }

    private:
        friend class NetworkIoService;

        NetworkIoServiceLimits limits;
        std::vector<std::optional<NetworkIoCompletion>> records;
        std::shared_ptr<std::atomic<bool>> metricAdmission;
        std::atomic<std::uint64_t> capacityDrops{};
        const std::thread::id ownerThread{std::this_thread::get_id()};
        mutable std::mutex queueMutex;
        std::atomic<bool> shuttingDown{false};
        std::size_t head{};
        std::size_t tail{};
        std::size_t size{};
        std::size_t inboundPackets{};
        std::uint64_t nextSequence{1};
        std::uint64_t pollGeneration{};
        std::size_t remainingPollCompletions{};
        bool pollActive{};
    };

    NetworkIoCompletion::NetworkIoCompletion(const NetworkIoCompletionKind kind, const ConnectionHandle connection) noexcept
        : kind_(kind), connection_(connection) {}

    /** @copydoc NetworkIoCompletion::MakeOperation */
    Result<NetworkIoCompletion> NetworkIoCompletion::MakeOperation(const NetworkIoCompletionKind kind, const ConnectionHandle connection) {
        if (!connection.IsValid() ||
            (kind != NetworkIoCompletionKind::OperationSucceeded && kind != NetworkIoCompletionKind::OperationCancelled))
            return Fail<NetworkIoCompletion>(NetworkErrors::NetworkIoCompletionInvalid);
        return Result<NetworkIoCompletion>::Success(NetworkIoCompletion{kind, connection});
    }

    /** @copydoc NetworkIoCompletion::MakePacket */
    Result<NetworkIoCompletion> NetworkIoCompletion::MakePacket(const ConnectionHandle connection, const ChannelId channel,
                                                                PacketBuffer payload) {
        if (!connection.IsValid() || !payload.IsValid())
            return Fail<NetworkIoCompletion>(NetworkErrors::NetworkIoCompletionInvalid);
        NetworkIoCompletion completion{NetworkIoCompletionKind::PacketReceived, connection};
        completion.channel_ = channel;
        completion.payload_ = std::move(payload);
        return Result<NetworkIoCompletion>::Success(std::move(completion));
    }

    /** @copydoc NetworkIoCompletion::MakeFailure */
    Result<NetworkIoCompletion> NetworkIoCompletion::MakeFailure(const ConnectionHandle connection, const NetworkTerminalRecord &failure) {
        if (!connection.IsValid() || !FailureMatchesConnection(failure, connection))
            return Fail<NetworkIoCompletion>(NetworkErrors::NetworkIoCompletionInvalid);
        NetworkIoCompletion completion{NetworkIoCompletionKind::OperationFailed, connection};
        completion.failure_ = failure;
        return Result<NetworkIoCompletion>::Success(std::move(completion));
    }

    /** @copydoc NetworkIoCompletion::Failure */
    const NetworkTerminalRecord *NetworkIoCompletion::Failure() const noexcept {
        return failure_.has_value() ? &*failure_ : nullptr;
    }

    NetworkIoCompletionProducer::NetworkIoCompletionProducer(std::shared_ptr<NetworkIoServiceState> state,
                                                             const std::uint64_t pollGeneration) noexcept
        : state_(std::move(state)), pollGeneration_(pollGeneration) {}

    /** @copydoc NetworkIoCompletionProducer::Publish */
    Result<void> NetworkIoCompletionProducer::Publish(NetworkIoCompletion completion) const {
        if (!state_)
            return Fail<void>(NetworkErrors::NetworkIoPollStale);
        return state_->Publish(std::move(completion), pollGeneration_);
    }

    NetworkIoService::NetworkIoService(ConstructionKey, std::unique_ptr<INetworkIoPollSource> backend,
                                       std::shared_ptr<NetworkIoServiceState> state, NetworkMetrics *metrics) noexcept
        : backend_(std::move(backend)), state_(std::move(state)), metrics_(metrics) {}

    /** @copydoc NetworkIoService::Create */
    Result<std::unique_ptr<NetworkIoService>> NetworkIoService::Create(std::unique_ptr<INetworkIoPollSource> backend,
                                                                       const NetworkIoServiceLimits &limits, NetworkMetrics *metrics) {
        if (!backend || !ValidLimits(limits))
            return Fail<std::unique_ptr<NetworkIoService>>(NetworkErrors::NetworkIoServiceInvalid);
        try {
            std::vector<std::optional<NetworkIoCompletion>> records(limits.maximumQueuedCompletions);
            auto state = std::make_shared<NetworkIoServiceState>(limits, std::move(records), metrics ? metrics->admission_ : nullptr);
            return Result<std::unique_ptr<NetworkIoService>>::Success(
                std::make_unique<NetworkIoService>(ConstructionKey{}, std::move(backend), std::move(state), metrics));
        } catch (const std::bad_alloc &) {
            return Fail<std::unique_ptr<NetworkIoService>>(NetworkErrors::NetworkIoServiceCapacityExceeded);
        }
    }

    NetworkIoService::~NetworkIoService() {
        Shutdown();
    }

    /** @copydoc NetworkIoService::PollBackend */
    Result<void> NetworkIoService::PollBackend(const std::size_t maximumCompletions, const CancellationToken &cancellation) {
        if (maximumCompletions == 0 || maximumCompletions > state_->limits.maximumCompletionsPerPoll)
            return Fail<void>(NetworkErrors::NetworkIoServiceInvalid);
        if (cancellation.IsCancellationRequested())
            return Fail<void>(NetworkErrors::TransportOperationCancelled);
        if (state_->shuttingDown.load())
            return Fail<void>(NetworkErrors::TransportShuttingDown);

        std::unique_lock pollGuard{pollMutex_, std::defer_lock};
        if (const bool acquired = pollGuard.try_lock(); !acquired)
            return Fail<void>(NetworkErrors::NetworkIoPollBusy);

        std::uint64_t generation{};
        {
            std::scoped_lock queueGuard{state_->queueMutex};
            if (state_->shuttingDown.load())
                return Fail<void>(NetworkErrors::TransportShuttingDown);
            if (state_->pollGeneration == std::numeric_limits<std::uint64_t>::max())
                return Fail<void>(NetworkErrors::NetworkIoSequenceExhausted);
            generation = ++state_->pollGeneration;
            state_->remainingPollCompletions = maximumCompletions;
            state_->pollActive = true;
        }

        const NetworkIoCompletionProducer producer{state_, generation};
        Result<void> result = backend_->Poll(producer, maximumCompletions, cancellation);
        {
            std::scoped_lock queueGuard{state_->queueMutex};
            state_->pollActive = false;
            state_->remainingPollCompletions = 0;
        }
        return result;
    }

    /** @copydoc NetworkIoService::DrainOwnerThread */
    Result<std::size_t> NetworkIoService::DrainOwnerThread(INetworkIoCompletionConsumer &consumer,
                                                           const std::size_t maximumCompletions) const {
        if (std::this_thread::get_id() != state_->ownerThread)
            return Fail<std::size_t>(NetworkErrors::NetworkIoWrongThread);
        if (maximumCompletions == 0 || maximumCompletions > state_->limits.maximumCompletionsPerDrain)
            return Fail<std::size_t>(NetworkErrors::NetworkIoServiceInvalid);
        if (state_->shuttingDown.load())
            return Fail<std::size_t>(NetworkErrors::TransportShuttingDown);

        std::size_t drained{};
        while (drained < maximumCompletions) {
            std::optional<NetworkIoCompletion> completion;
            {
                std::scoped_lock lock{state_->queueMutex};
                if (state_->size == 0)
                    break;
                if (state_->records[state_->head]->Kind() == NetworkIoCompletionKind::PacketReceived)
                    --state_->inboundPackets;
                completion.emplace(std::move(*state_->records[state_->head]));
                state_->records[state_->head].reset();
                state_->head = (state_->head + 1) % state_->limits.maximumQueuedCompletions;
                --state_->size;
            }
            if (metrics_ && completion->Kind() == NetworkIoCompletionKind::OperationFailed)
                (void)metrics_->RecordFailure(NetworkMetricFailure::Transport);
            consumer.Consume(std::move(*completion));
            ++drained;
        }
        if (metrics_ && metrics_->IsCollecting()) {
            (void)metrics_->RecordDrop(NetworkMetricDrop::Capacity, state_->capacityDrops.exchange(0));
            std::size_t completionDepth{};
            std::size_t inboundDepth{};
            {
                std::scoped_lock lock{state_->queueMutex};
                completionDepth = state_->size;
                inboundDepth = state_->inboundPackets;
            }
            (void)metrics_->SetQueueDepth(NetworkMetricQueue::Completion, completionDepth);
            (void)metrics_->SetQueueDepth(NetworkMetricQueue::Inbound, inboundDepth);
        }
        return Result<std::size_t>::Success(drained);
    }

    /** @copydoc NetworkIoService::Shutdown */
    void NetworkIoService::Shutdown() noexcept {
        if (state_->shuttingDown.exchange(true))
            return;
        backend_->RequestStop();
        {
            std::scoped_lock pollGuard{pollMutex_};
            backend_->Shutdown();
        }
        std::scoped_lock queueGuard{state_->queueMutex};
        state_->pollActive = false;
        state_->remainingPollCompletions = 0;
        while (state_->size != 0) {
            state_->records[state_->head].reset();
            state_->head = (state_->head + 1) % state_->limits.maximumQueuedCompletions;
            --state_->size;
        }
        state_->inboundPackets = 0;
    }

    /** @copydoc NetworkIoService::QueuedCompletions */
    std::size_t NetworkIoService::QueuedCompletions() const noexcept {
        std::scoped_lock lock{state_->queueMutex};
        return state_->size;
    }

    /** @copydoc NetworkIoService::IsShuttingDown */
    bool NetworkIoService::IsShuttingDown() const noexcept {
        return state_->shuttingDown.load();
    }
}  // namespace Horo::Network
