#include "AgentHistorySession.h"

#include "Horo/Foundation/JobSystem.h"

namespace Horo::Agent {
    namespace {
        using Phase = AgentHistorySessionPhase;
        using Availability = AgentHistoryAvailability;
        constexpr auto AdmissionRetry = std::chrono::milliseconds{250};

        /** @brief An allocated request object acts as the exact, unambiguous project incarnation fence. */
        struct Selection final {
            std::optional<AgentHistoryProject> project;
        };

        struct Display final {
            Availability availability{Availability::Unavailable};
            std::vector<AgentConversation> conversations;
        };

        /** @brief Only the one admitted I/O operation may inspect or replace this store. */
        struct HistoryWorker final {
            DurableFileSystem &files;
            AgentHistoryTextFilter filter;
            AgentHistoryLimits limits;
            std::optional<AgentConversationStore> store;

            [[nodiscard]] Result<void> Switch(const Selection &selection, Display &display, const CancellationToken &stop) {
                store.reset();
                if (stop.IsCancellationRequested())
                    return JobCancelled();
                if (!selection.project)
                    return Result<void>::Success();

                const auto &[root, id] = *selection.project;
                auto opened = AgentConversationStore::Open(files, root, id, filter, limits);
                if (stop.IsCancellationRequested())
                    return JobCancelled();
                display.availability = opened.Availability();
                const auto conversations = opened.Conversations();
                display.conversations.assign(conversations.begin(), conversations.end());
                store.emplace(std::move(opened));
                return Result<void>::Success();
            }
        };
    }  // namespace

    struct AgentHistorySession::State final {
        struct Flight final {
            JobHandle job;
            std::shared_ptr<const Selection> selection;
            std::shared_ptr<Display> result; /**< Written only by the worker before terminal publication. */
        };

        const JobSystem &jobs;
        std::shared_ptr<HistoryWorker> worker;
        std::shared_ptr<const Selection> desired;
        std::shared_ptr<const Selection> settled;
        std::optional<Flight> flight;
        std::chrono::steady_clock::time_point retryAt{};
        Phase phase{Phase::NoProject};
        Display display;
        bool shutdown{};

        /** @brief Publish only this owner's latest exact selection after the associated job is terminal. */
        void Settle() {
            if (!flight)
                return;
            const auto snapshot = flight->job.Snapshot();
            if (!snapshot || !snapshot->terminalResult)
                return;
            if (flight->selection == desired) {
                settled = desired;
                if (desired->project) {
                    phase = Phase::Settled;
                    if (snapshot->terminalResult->state == JobState::Succeeded)
                        display = std::move(*flight->result);
                }
            }
            flight.reset();
        }

        /** @brief Queue the newest coalesced request, with bounded retry under transient scheduler pressure. */
        void Dispatch() {
            if (flight || desired == settled || std::chrono::steady_clock::now() < retryAt)
                return;
            auto result = std::make_shared<Display>();
            const JobProducerScope owner{JobProducerRole::MainEditor};
            auto submitted = jobs.SubmitResult({.priority = JobPriority::Background,
                                                .requirement = JobRequirement::Optional,
                                                .resource = JobResource::Io},
                                               [worker = worker, selection = desired, result](const CancellationToken &stop) {
                return worker->Switch(*selection, *result, stop);
            });
            if (submitted.HasError()) {
                retryAt = std::chrono::steady_clock::now() + AdmissionRetry;
                if (desired->project)
                    phase = Phase::Settled;
                return;
            }
            if (desired->project)
                phase = Phase::Opening;
            flight.emplace(std::move(submitted).Value(), desired, std::move(result));
        }
    };

    /** @copydoc AgentHistorySession::AgentHistorySession */
    AgentHistorySession::AgentHistorySession(const JobSystem &jobs, DurableFileSystem &files, AgentHistoryTextFilter filter,
                                             const AgentHistoryLimits &limits)
        : state_(std::make_unique<State>(State{jobs, std::make_shared<HistoryWorker>(HistoryWorker{files, std::move(filter), limits})})) {}

    AgentHistorySession::~AgentHistorySession() {
        Shutdown();
    }

    /** @copydoc AgentHistorySession::SelectProject */
    void AgentHistorySession::SelectProject(std::optional<AgentHistoryProject> project) {
        auto &owner = *state_;
        if (owner.shutdown || (owner.desired ? owner.desired->project == project : !project))
            return;
        owner.desired = std::make_shared<const Selection>(Selection{std::move(project)});
        owner.phase = owner.desired->project ? Phase::Opening : Phase::NoProject;
        owner.display = {};
        owner.retryAt = {};
        if (owner.flight)
            static_cast<void>(owner.flight->job.RequestCancel());
        Poll();
    }

    /** @copydoc AgentHistorySession::Poll */
    void AgentHistorySession::Poll() {
        if (state_->shutdown)
            return;
        state_->Settle();
        state_->Dispatch();
    }

    /** @copydoc AgentHistorySession::View */
    AgentHistorySessionView AgentHistorySession::View() const noexcept {
        if (state_->phase == Phase::Settled)
            return {Phase::Settled, state_->display.availability, state_->display.conversations};
        return {state_->phase};
    }

    /** @copydoc AgentHistorySession::Shutdown */
    void AgentHistorySession::Shutdown() noexcept {
        auto &owner = *state_;
        if (owner.shutdown)
            return;
        owner.shutdown = true;
        owner.phase = Phase::NoProject;
        owner.display = {};
        owner.desired.reset();
        owner.settled.reset();
        if (owner.flight) {
            static_cast<void>(owner.flight->job.RequestCancel());
            static_cast<void>(owner.flight->job.Wait());
            owner.flight.reset();
        }
        if (owner.worker->store) {
            try {
                auto close = owner.jobs.SubmitResult({.priority = JobPriority::Background, .resource = JobResource::Io},
                                                     [worker = owner.worker](const CancellationToken &) {
                    worker->store.reset();
                    return Result<void>::Success();
                });
                if (close.HasValue())
                    static_cast<void>(close.Value().Wait());
            } catch (const std::bad_alloc &) {
                // The quiescent store can release its native lock on this cold boundary if the worker cannot be reached.
            }
        }
        owner.worker.reset();
    }
}  // namespace Horo::Agent
