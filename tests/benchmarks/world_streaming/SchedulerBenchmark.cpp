/** @file SchedulerBenchmark.cpp @brief Bounded measurements of existing StreamingAuthorityRole operations. */
#include "Horo/WorldStreaming/StreamingFairQueue.h"
#include "Horo/WorldStreaming/StreamingSchedulerAdmission.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::WorldStreaming;
    using Clock = std::chrono::steady_clock;
    using Transition = StreamingCellOperationTransition;

    constexpr unsigned WarmupQuanta = 32;
    constexpr unsigned DefaultQuanta = 512;
    constexpr unsigned MaximumQuanta = 4096;
    constexpr unsigned PriorityBurst = 3;

    /** @brief Reports violated production invariants independently of the timing measurements. */
    void Check(const bool condition, const char *message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    /** @brief Surfaces the owning service's exact error instead of collecting invalid measurements. */
    template <typename T> T Take(Result<T> result) {
        if (result.HasError())
            throw std::runtime_error(result.ErrorValue().message);
        return std::move(result).Value();
    }

    /** @brief Leaves typed diagnostics visible when fixture setup or retirement fails. */
    void Accept(const Result<void> &result) {
        if (result.HasError())
            throw std::runtime_error(result.ErrorValue().message);
    }

    /** @brief Forms the benchmark's fixed, non-wrapping typed identities. */
    template <typename T> T Id(const std::uint64_t value = 1) {
        return Take(T::Create(value));
    }

    struct Timings {
        std::vector<std::int64_t> samples;

        void Record(const Clock::time_point begin, const bool keep) {
            const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
            if (keep)
                samples.push_back(duration);
        }

        void Write(const char *name) {
            std::ranges::sort(samples);
            Check(!samples.empty(), "Measurement phase had no samples.");
            const auto percentile = [&](const std::size_t percent) {
                return samples[(samples.size() * percent + 99) / 100 - 1];
            };
            std::cout << ",\"" << name << "\":{\"samples\":" << samples.size() << ",\"p50Ns\":" << percentile(50)
                      << ",\"p95Ns\":" << percentile(95) << ",\"p99Ns\":" << percentile(99) << ",\"maxNs\":" << samples.back() << '}';
        }
    };

    struct Measurements {
        Timings timer, select, commit, admit, completion, interruptedRetirement, deferredSelect, capacityDenial;

        explicit Measurements(const unsigned quanta) {
            for (auto *stage : {&timer, &select, &commit, &admit, &completion, &interruptedRetirement, &deferredSelect, &capacityDenial})
                stage->samples.reserve(quanta);
        }
    };

    /** @brief Times only the supplied owner call and retains its result for checks outside that interval. */
    template <typename Call> auto Measure(Timings &timing, const bool keep, Call call) {
        const auto begin = Clock::now();
        auto result = call();
        timing.Record(begin, keep);
        return result;
    }

    /** @brief Exercises the canonical terminal transition and exact reservation release. */
    StreamingCellOperationOutcome Retire(StreamingSchedulerAdmissionLedger &scheduler, const StreamingSchedulerReservation &reservation,
                                         const Transition reason) {
        auto terminal = Take(scheduler.Advance(reservation, reason));
        if (!terminal.IsTerminal())
            terminal = Take(scheduler.Advance(reservation, Transition::AcknowledgeRetirement));
        Accept(scheduler.Release(reservation));
        return terminal.Outcome();
    }

    /** @brief Owns one canonical queue and scheduler, with budget feasibility supplied at an authority safe point. */
    class SchedulerWorkload final {
    public:
        explicit SchedulerWorkload(const unsigned depth)
            : world_(Take(WorldPartitionId::Create(SerializedWorldPartitionId{1}))),
              queue_(
                  Take(StreamingFairQueue::Create({.id = queueId_,
                                                   .partition = world_,
                                                   .epoch = epoch_,
                                                   .maximumEntries = depth,
                                                   .maximumPriorityDispatches = PriorityBurst},
                                                  Take(StreamingPriorityPolicy::Create({.id = Id<StreamingPriorityPolicyId>(),
                                                                                        .revision = Id<StreamingPriorityPolicyRevision>(),
                                                                                        .maximumCandidates = depth}))))),
              scheduler_(Take(
                  StreamingSchedulerAdmissionLedger::Create(Id<StreamingSchedulerLedgerId>(),
                                                            {2, 1, {WorldPartitionProjectProfile::Standalone, concurrency_, 2, 1, 1}}))),
              admissions_(depth) {
            for (unsigned index = 0; index < depth; ++index)
                Enqueue(0.0F);
        }

        void Run(const unsigned samples) {
            Measurements timing(samples);
            unsigned admitted{};
            unsigned fair{};
            unsigned denied{};
            for (unsigned quantum = 0; quantum < WarmupQuanta + samples; ++quantum) {
                ++serviceTime_;
                const bool keep = quantum >= WarmupQuanta;
                const bool capacityFull = quantum % 8 == 0;
                timing.timer.Record(Clock::now(), keep);
                const auto pending = queue_.PendingEntries();
                Check(pending.size() == admissions_.size(), "Queue did not retain its declared occupancy.");
                for (std::size_t index = 0; index < pending.size(); ++index) {
                    const bool deferred = capacityFull || (index + quantum) % 4 == 0;
                    admissions_[index] = {pending[index].operation,
                                          deferred ? StreamingFairQueueEligibility::Deferred : StreamingFairQueueEligibility::Admissible};
                }
                if (capacityFull) {
                    RunSaturated(timing, keep, pending.front().operation);
                    denied += keep;
                } else {
                    const bool oldestAdmissible = Dispatch(timing, keep, quantum);
                    fair += keep && oldestAdmissible;
                    admitted += keep;
                }
                Check(scheduler_.ReservedCount() == 0 && scheduler_.ReservedCapacityUnits() == 0, "Scheduler leaked a reservation.");
            }
            Accept(queue_.Shutdown(Context()));
            scheduler_.BeginShutdown();
            Check(queue_.IsClosed() && scheduler_.State() == StreamingSchedulerAdmissionState::Closed && fair != 0,
                  "Workload did not finish with fair dispatch and exact shutdown.");
            std::cout << "{\"scope\":\"streaming_authority_owner_calls\",\"queueDepth\":" << admissions_.size()
                      << ",\"schedulerCapacityUnits\":1,\"priorityBurst\":" << PriorityBurst << ",\"warmupQuanta\":" << WarmupQuanta
                      << ",\"measuredQuanta\":" << samples << ",\"admitted\":" << admitted << ",\"oldestFirst\":" << fair
                      << ",\"capacityDenials\":" << denied;
            timing.timer.Write("steadyClockPair");
            timing.select.Write("selectEligible");
            timing.commit.Write("commitDispatch");
            timing.admit.Write("admit");
            timing.completion.Write("completeAndRelease");
            timing.interruptedRetirement.Write("interruptAcknowledgeAndRelease");
            timing.deferredSelect.Write("selectAllDeferred");
            timing.capacityDenial.Write("rejectAdmissionAtCapacity");
            std::cout << "}\n";
        }

    private:
        [[nodiscard]] StreamingFairQueueContext Context() const {
            return {queueId_, queue_.Revision(), serviceTime_};
        }

        [[nodiscard]] StreamingCellOperationHandle NextOperation() {
            const auto id = nextOperation_++;
            return {Id<StreamingCellOperationId>(id),
                    {world_,
                     epoch_,
                     {static_cast<std::int32_t>(id), 0, 0, 0, Take(StreamingLayerId::Create(1))},
                     Id<StreamingGeneration>()}};
        }

        void Enqueue(const float priority) {
            const auto operation = NextOperation();
            Accept(queue_.Enqueue(Context(), {.operation = operation,
                                              .priority = {.source = {.id = Id<StreamingSourceId>(operation.operation.Value()),
                                                                      .owner = {world_, epoch_, 0, 1},
                                                                      .intent = StreamingSourceIntent::Camera,
                                                                      .priority = Take(StreamingSourcePriority::Create(priority)),
                                                                      .revision = Id<StreamingSourceRevision>()},
                                                           .cell = operation.fence.cell}}));
        }

        void RunSaturated(Measurements &timing, const bool keep, const StreamingCellOperationHandle waiting) {
            const auto blocker = Take(StreamingCellOperation::Create(NextOperation(), StreamingCellOperationKind::Load));
            const auto held = Take(scheduler_.TryAdmit(blocker, 1, concurrency_));
            const auto selected = Take(Measure(timing.deferredSelect, keep, [&] {
                return queue_.Select(Context(), admissions_);
            }));
            Check(!selected, "A fully deferred queue bypassed capacity.");
            const auto waitingOperation = Take(StreamingCellOperation::Create(waiting, StreamingCellOperationKind::Load));
            const auto denied = Measure(timing.capacityDenial, keep, [&] {
                return scheduler_.TryAdmit(waitingOperation, 1, concurrency_);
            });
            Check(denied.HasError() && denied.ErrorValue().code.Value() == WorldStreamingErrors::SchedulerCapacityExceeded.code.Value() &&
                      scheduler_.ReservedCapacityUnits() == 1,
                  "Capacity rejection changed the canonical reservation.");
            Check(Retire(scheduler_, held, Transition::Cancel) == StreamingCellOperationOutcome::Cancelled,
                  "Capacity holder was not cancelled.");
        }

        bool Dispatch(Measurements &timing, const bool keep, const unsigned quantum) {
            const auto selected = Take(Measure(timing.select, keep, [&] {
                return queue_.Select(Context(), admissions_);
            }));
            Check(selected.has_value(), "Mixed eligibility did not select an admissible operation.");
            const auto operation = Take(StreamingCellOperation::Create(selected->operation, StreamingCellOperationKind::Load));
            const auto reservation = Take(Measure(timing.admit, keep, [&] {
                return scheduler_.TryAdmit(operation, 1, concurrency_);
            }));
            Accept(Measure(timing.commit, keep, [&] {
                return queue_.CommitDispatch(Context(), *selected);
            }));
            static_cast<void>(Take(scheduler_.Advance(reservation, Transition::BeginPreparation)));
            constexpr std::array transitions{Transition::Complete, Transition::Complete, Transition::Cancel, Transition::Fail};
            constexpr std::array outcomes{StreamingCellOperationOutcome::Succeeded, StreamingCellOperationOutcome::Succeeded,
                                          StreamingCellOperationOutcome::Cancelled, StreamingCellOperationOutcome::Failed};
            const auto scenario = quantum % transitions.size();
            const auto outcome = Measure(scenario < 2 ? timing.completion : timing.interruptedRetirement, keep, [&] {
                return Retire(scheduler_, reservation, transitions[scenario]);
            });
            Check(outcome == outcomes[scenario], "Canonical retirement returned a different terminal outcome.");
            Enqueue(1'000.0F);
            return selected->reason == StreamingFairQueueDispatchReason::OldestAdmissible;
        }

        const WorldPartitionId world_;
        const PartitionEpoch epoch_{Id<PartitionEpoch>()};
        const StreamingFairQueueId queueId_{Id<StreamingFairQueueId>()};
        const StreamingConcurrencyRevision concurrency_{Id<StreamingConcurrencyRevision>()};
        StreamingFairQueue queue_;
        StreamingSchedulerAdmissionLedger scheduler_;
        std::vector<StreamingFairQueueAdmission> admissions_;
        std::uint64_t serviceTime_{};
        std::uint64_t nextOperation_{1};
    };
}  // namespace

int main(int argc, char **argv) {
    try {
        unsigned samples = DefaultQuanta;
        if (argc == 2) {
            const std::string_view input{argv[1]};
            const auto parsed = std::from_chars(input.data(), input.data() + input.size(), samples);
            if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || samples < 16 || samples > MaximumQuanta)
                return 2;
        } else if (argc != 1)
            return 2;
        std::cout << "{\"buildMode\":\"" << HORO_BENCHMARK_BUILD_MODE << "\",\"compiler\":\"" << HORO_BENCHMARK_COMPILER
                  << "\",\"platform\":\"" << HORO_BENCHMARK_PLATFORM
                  << "\",\"clock\":\"steady_clock\",\"actualFrameTiming\":false,\"workerIo\":false}\n";
        SchedulerWorkload(16).Run(samples);
        SchedulerWorkload(128).Run(samples);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
