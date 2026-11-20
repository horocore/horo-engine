#include "AgentHistorySession.h"

#include "Horo/Foundation/JobSystem.h"

#include <limits>

namespace Horo::Agent {
    namespace {
        using Phase = AgentHistorySessionPhase;
        using Availability = AgentHistoryAvailability;
        constexpr auto AdmissionRetry = std::chrono::milliseconds{250};

        /** @brief An allocated request object acts as the exact, unambiguous project incarnation fence. */
        struct Selection final {
            std::optional<AgentHistoryProject> project;
            AgentHistoryProjectRevision revision;
        };

        struct Display final {
            Availability availability{Availability::Unavailable};
            std::shared_ptr<const std::vector<AgentConversation>> conversations;
        };

        struct MutationPayload final {
            std::optional<AgentHistoryStartedTurn> started;
            std::optional<std::string> exportedText;
        };

        struct WorkResult final {
            Display display;
            MutationPayload payload;
        };

        using MutationResult = Result<MutationPayload>;

        /** @brief Keeps the typed store error for operations without a response payload. */
        MutationResult MutationOutcome(Result<void> applied) {
            if (applied.HasError())
                return MutationResult::Failure(std::move(applied).ErrorValue());
            return MutationResult::Success({});
        }

        struct TurnWork final {
            std::optional<AgentConversationId> conversation;
            std::string title;
            std::string userText;
            std::vector<AgentHistoryReference> references;
            std::uint64_t now;
            std::optional<std::string> answer;
            AgentHistoryUsage usage;

            /** @brief Commits the user prompt and reserves the exact assistant response together. */
            [[nodiscard]] MutationResult operator()(AgentConversationStore &store) const {
                const AgentHistoryTurnRequest request{conversation, title, userText, references, now};
                auto started = answer ? store.RecordCompleted(request, {*answer, usage}) : store.BeginTurn(request);
                if (started.HasError())
                    return MutationResult::Failure(std::move(started).ErrorValue());
                return MutationResult::Success({.started = std::move(started).Value()});
            }
        };

        struct CheckpointWork final {
            AgentConversationId conversation;
            AgentHistoryMessageId response;
            AgentHistoryMessageState state;
            std::string visibleText;
            std::vector<AgentHistoryReference> references;
            AgentHistoryUsage usage;
            std::uint64_t now;

            /** @brief Applies one visible response update without accepting any provider-owned envelope. */
            [[nodiscard]] MutationResult operator()(AgentConversationStore &store) const {
                return MutationOutcome(store.Checkpoint(conversation, response, state, visibleText, references, usage, now));
            }
        };

        /** @brief Shares the committed display state without allocating after irreversible writer publication. */
        Display CaptureDisplay(const AgentConversationStore &store) noexcept {
            return {store.Availability(), store.ConversationSnapshot()};
        }

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
                display = CaptureDisplay(opened);
                store.emplace(std::move(opened));
                return Result<void>::Success();
            }

            /** @brief Runs one accepted typed mutation against this worker's current store. */
            template <typename Mutation>
            [[nodiscard]] Result<void> Mutate(const Mutation &mutation, WorkResult &result, const CancellationToken &stop) {
                if (stop.IsCancellationRequested())
                    return JobCancelled();
                if (!store)
                    return Result<void>::Failure(MakeError(AgentHistoryErrors::Unavailable));
                auto applied = mutation(*store);
                if (applied.HasError())
                    return Result<void>::Failure(std::move(applied).ErrorValue());
                result.payload = std::move(applied).Value();
                result.display = CaptureDisplay(*store);
                return Result<void>::Success();
            }
        };
    }  // namespace

    struct AgentHistorySession::State final {
        struct Flight final {
            JobHandle job;
            std::shared_ptr<const Selection> selection;
            std::shared_ptr<WorkResult> result; /**< Written only by the worker before terminal publication. */
            std::optional<AgentHistoryOperationId> operation;
        };

        const JobSystem &jobs;
        std::shared_ptr<HistoryWorker> worker;
        std::shared_ptr<const Selection> desired;
        std::shared_ptr<const Selection> settled;
        std::optional<Flight> flight;
        std::chrono::steady_clock::time_point retryAt{};
        Phase phase{Phase::NoProject};
        Display display;
        std::optional<AgentHistoryMutationCompletion> mutation;
        std::uint64_t lastRevision{};
        bool shutdown{};

        /** @brief Records the exact terminal writer outcome. Failed writes keep the previous committed snapshot. */
        void SettleMutation(const Flight &completed, const JobTerminalResult &terminal) {
            auto &payload = completed.result->payload;
            mutation.emplace(desired->revision, *completed.operation, payload.started, terminal.error, std::move(payload.exportedText));
            if (terminal.state == JobState::Succeeded)
                display = std::move(completed.result->display);
        }

        /** @brief Reconciles the exact selected project after its load becomes terminal. */
        void SettleSelection(const Flight &completed, const JobTerminalResult &terminal) {
            settled = desired;
            if (!desired->project)
                return;
            phase = Phase::Settled;
            if (terminal.state == JobState::Succeeded)
                display = std::move(completed.result->display);
        }

        /** @brief Publish only this owner's latest exact selection after the associated job is terminal. */
        void Settle() {
            if (!flight)
                return;
            const auto snapshot = flight->job.Snapshot();
            if (!snapshot || !snapshot->terminalResult)
                return;
            if (flight->selection == desired) {
                if (flight->operation)
                    SettleMutation(*flight, *snapshot->terminalResult);
                else
                    SettleSelection(*flight, *snapshot->terminalResult);
            }
            flight.reset();
        }

        /** @brief Queue the newest coalesced request, with bounded retry under transient scheduler pressure. */
        void Dispatch() {
            if (flight || desired == settled || std::chrono::steady_clock::now() < retryAt)
                return;
            auto result = std::make_shared<WorkResult>();
            const JobProducerScope owner{JobProducerRole::MainEditor};
            auto submitted = jobs.SubmitResult({.priority = JobPriority::Background,
                                                .requirement = JobRequirement::Optional,
                                                .resource = JobResource::Io},
                                               [worker = worker, selection = desired, result](const CancellationToken &stop) {
                return worker->Switch(*selection, result->display, stop);
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

        /** @brief Rejects stale project authority, unopened stores, and pressure before retaining any caller text. */
        [[nodiscard]] Result<void> CheckMutation(const AgentHistoryProjectRevision project) const {
            if (shutdown || !desired || !desired->project || desired->revision != project)
                return Result<void>::Failure(MakeError(AgentHistoryErrors::Stale));
            if (flight || mutation)
                return Result<void>::Failure(MakeError(AgentHistoryErrors::Busy));
            using enum AgentHistoryAvailability;
            if (settled != desired ||
                (display.availability != Ready && display.availability != Recovered && display.availability != DurabilityUnconfirmed))
                return Result<void>::Failure(MakeError(AgentHistoryErrors::Unavailable));
            return Result<void>::Success();
        }

        /** @brief Submits one prevalidated owned command and returns its exact scheduler operation identity. */
        template <typename Mutation> [[nodiscard]] Result<AgentHistoryOperationId> SubmitMutation(Mutation command) {
            using Output = Result<AgentHistoryOperationId>;
            auto result = std::make_shared<WorkResult>();
            const JobProducerScope owner{JobProducerRole::MainEditor};
            auto submitted = jobs.SubmitResult({.priority = JobPriority::Interactive, .resource = JobResource::Io},
                                               [worker = worker, command = std::move(command), result](const CancellationToken &stop) {
                return worker->Mutate(command, *result, stop);
            });
            if (submitted.HasError())
                return Output::Failure(std::move(submitted).ErrorValue());
            const AgentHistoryOperationId operation{submitted.Value().Id()};
            flight.emplace(std::move(submitted).Value(), desired, std::move(result), operation);
            return Output::Success(operation);
        }

        /** @brief Admits one bounded, owned prompt and optional completed answer. */
        [[nodiscard]] Result<AgentHistoryOperationId> SubmitTurn(const AgentHistoryProjectRevision project,
                                                                 const AgentHistoryTurnRequest &request,
                                                                 const std::optional<AgentHistoryCompletedText> answer) {
            using Output = Result<AgentHistoryOperationId>;
            const auto admission = CheckMutation(project);
            if (admission.HasError())
                return Output::Failure(admission.ErrorValue());
            const auto &limits = worker->limits;
            const auto title = request.conversation ? std::string_view{} : request.title;
            if (title.size() > AgentHistoryLimits::MaximumTitleBytes || request.userText.size() > limits.textBytesPerMessage ||
                request.references.size() > limits.referencesPerMessage ||
                (answer && answer->assistant.size() > limits.textBytesPerMessage))
                return Output::Failure(MakeError(AgentHistoryErrors::Capacity));
            TurnWork work{request.conversation,
                          std::string{title},
                          std::string{request.userText},
                          {request.references.begin(), request.references.end()},
                          request.now,
                          {},
                          {}};
            if (answer) {
                work.answer.emplace(answer->assistant);
                work.usage = answer->usage;
            }
            return SubmitMutation(std::move(work));
        }

        /** @brief Schedules a conversation command with no caller text to validate or retain. */
        template <typename Command>
        [[nodiscard]] Result<AgentHistoryOperationId> SubmitConversation(const AgentHistoryProjectRevision project, Command command) {
            const auto admission = CheckMutation(project);
            if (admission.HasError())
                return Result<AgentHistoryOperationId>::Failure(admission.ErrorValue());
            return SubmitMutation(std::move(command));
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
        if (owner.lastRevision == std::numeric_limits<std::uint64_t>::max())
            project.reset();
        else
            ++owner.lastRevision;
        owner.desired = std::make_shared<const Selection>(Selection{std::move(project), {owner.lastRevision}});
        owner.phase = owner.desired->project ? Phase::Opening : Phase::NoProject;
        owner.display = {};
        owner.mutation.reset();
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
        const auto project = state_->desired && state_->desired->project ? state_->desired->revision : AgentHistoryProjectRevision{};
        const auto &committed = state_->display.conversations;
        if (state_->phase == Phase::Settled) {
            const auto conversations = committed ? std::span<const AgentConversation>{*committed} : std::span<const AgentConversation>{};
            return {Phase::Settled, state_->display.availability, conversations, project};
        }
        return {.phase = state_->phase, .project = project};
    }

    /** @copydoc AgentHistorySession::BeginTurn */
    Result<AgentHistoryOperationId> AgentHistorySession::BeginTurn(const AgentHistoryProjectRevision project,
                                                                   const AgentHistoryTurnRequest &request) {
        return state_->SubmitTurn(project, request, std::nullopt);
    }

    /** @copydoc AgentHistorySession::RecordCompleted */
    Result<AgentHistoryOperationId> AgentHistorySession::RecordCompleted(const AgentHistoryProjectRevision project,
                                                                         const AgentHistoryTurnRequest &request,
                                                                         const AgentHistoryCompletedText &answer) {
        return state_->SubmitTurn(project, request, answer);
    }

    /** @copydoc AgentHistorySession::Checkpoint */
    Result<AgentHistoryOperationId> AgentHistorySession::Checkpoint(const AgentHistoryProjectRevision project,
                                                                    const AgentHistoryCheckpointRequest &request) {
        using Output = Result<AgentHistoryOperationId>;
        const auto admission = state_->CheckMutation(project);
        if (admission.HasError())
            return Output::Failure(admission.ErrorValue());
        const auto &limits = state_->worker->limits;
        if (request.visibleText.size() > limits.textBytesPerMessage || request.references.size() > limits.referencesPerMessage)
            return Output::Failure(MakeError(AgentHistoryErrors::Capacity));
        return state_->SubmitMutation(CheckpointWork{request.conversation,
                                                     request.response,
                                                     request.state,
                                                     std::string{request.visibleText},
                                                     {request.references.begin(), request.references.end()},
                                                     request.usage,
                                                     request.now});
    }

    /** @copydoc AgentHistorySession::Rename */
    Result<AgentHistoryOperationId> AgentHistorySession::Rename(const AgentHistoryProjectRevision project,
                                                                const AgentConversationId conversation, const std::string_view title) {
        using Output = Result<AgentHistoryOperationId>;
        const auto admission = state_->CheckMutation(project);
        if (admission.HasError())
            return Output::Failure(admission.ErrorValue());
        if (title.size() > AgentHistoryLimits::MaximumTitleBytes)
            return Output::Failure(MakeError(AgentHistoryErrors::Capacity));
        return state_->SubmitMutation([conversation, title = std::string{title}](AgentConversationStore &store) {
            return MutationOutcome(store.Rename(conversation, title));
        });
    }

    /** @copydoc AgentHistorySession::SetArchived */
    Result<AgentHistoryOperationId> AgentHistorySession::SetArchived(const AgentHistoryProjectRevision project,
                                                                     const AgentConversationId conversation, const bool archived) {
        return state_->SubmitConversation(project, [conversation, archived](AgentConversationStore &store) {
            return MutationOutcome(store.SetArchived(conversation, archived));
        });
    }

    /** @copydoc AgentHistorySession::Delete */
    Result<AgentHistoryOperationId> AgentHistorySession::Delete(const AgentHistoryProjectRevision project,
                                                                const AgentConversationId conversation) {
        return state_->SubmitConversation(project, [conversation](AgentConversationStore &store) {
            return MutationOutcome(store.Delete(conversation));
        });
    }

    /** @copydoc AgentHistorySession::Export */
    Result<AgentHistoryOperationId> AgentHistorySession::Export(const AgentHistoryProjectRevision project,
                                                                const AgentConversationId conversation) {
        return state_->SubmitConversation(project, [conversation](const AgentConversationStore &store) {
            auto exported = store.Export(conversation);
            if (exported.HasError())
                return MutationResult::Failure(std::move(exported).ErrorValue());
            return MutationResult::Success({.exportedText = std::move(exported).Value()});
        });
    }

    /** @copydoc AgentHistorySession::TakeMutation */
    std::optional<AgentHistoryMutationCompletion> AgentHistorySession::TakeMutation() {
        return std::exchange(state_->mutation, std::nullopt);
    }

    /** @copydoc AgentHistorySession::Shutdown */
    void AgentHistorySession::Shutdown() noexcept {
        auto &owner = *state_;
        if (owner.shutdown)
            return;
        owner.shutdown = true;
        owner.phase = Phase::NoProject;
        owner.display = {};
        owner.mutation.reset();
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
