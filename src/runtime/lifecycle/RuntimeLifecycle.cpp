#include "Horo/Runtime/RuntimeLifecycle.h"

#include "RuntimeErrors.h"

#include <exception>
#include <utility>

namespace Horo::Runtime {
    namespace {
        [[nodiscard]] Result<void> StateFailure() {
            return Result<void>::Failure(MakeError(RuntimeErrors::InvalidLifecycleState));
        }

        template <typename Callback> [[nodiscard]] Result<void> ContainParticipantException(Callback &&callback) {
            try {
                return callback();
            } catch (const std::runtime_error &exception) {  // NOSONAR(cpp:S1181)
                return Result<void>::Failure(MakeError(RuntimeErrors::UnexpectedException, exception.what()));
            } catch (const std::logic_error &exception) {  // NOSONAR(cpp:S1181)
                return Result<void>::Failure(MakeError(RuntimeErrors::UnexpectedException, exception.what()));
            } catch (const std::bad_alloc &exception) {
                return Result<void>::Failure(MakeError(RuntimeErrors::UnexpectedException, exception.what()));
            } catch (const std::exception &exception) {  // NOSONAR(cpp:S1181)
                return Result<void>::Failure(MakeError(RuntimeErrors::UnexpectedException, exception.what()));
            } catch (...) {  // NOSONAR(cpp:S1181, cpp:S2738)
                return Result<void>::Failure(MakeError(RuntimeErrors::UnexpectedException));
            }
        }

    }  // namespace

    RuntimeLifecycle::~RuntimeLifecycle() {
        Shutdown();
    }

    /** @copydoc RuntimeLifecycle::AddParticipant */
    Result<void> RuntimeLifecycle::AddParticipant(std::unique_ptr<RuntimeLifecycleParticipant> participant) {
        if (state_ != RuntimeLifecycleState::Created)
            return StateFailure();
        if (!participant) {
            return Result<void>::Failure(MakeError(RuntimeErrors::NullParticipant));
        }
        participants_.push_back(std::move(participant));
        return Result<void>::Success();
    }

    /** @copydoc RuntimeLifecycle::Startup */
    Result<void> RuntimeLifecycle::Startup(const CancellationToken &cancellation) {
        if (state_ != RuntimeLifecycleState::Created)
            return StateFailure();
        state_ = RuntimeLifecycleState::Initializing;
        for (const auto &participant : participants_) {
            if (cancellation.IsCancellationRequested()) {
                state_ = RuntimeLifecycleState::Failed;
                Shutdown();
                return Result<void>::Failure(MakeError(RuntimeErrors::Cancelled));
            }
            if (Result<void> result = ContainParticipantException([&participant, &cancellation] {
                return participant->Startup(cancellation);
            });
                result.HasError()) {
                state_ = RuntimeLifecycleState::Failed;
                Shutdown();
                return result;
            }
            ++startedParticipantCount_;
        }
        state_ = RuntimeLifecycleState::Ready;
        state_ = RuntimeLifecycleState::Running;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeLifecycle::DispatchPhase */
    Result<void> RuntimeLifecycle::DispatchPhase(const RuntimePhase phase, const FrameContext &context) {
        if (state_ != RuntimeLifecycleState::Running && state_ != RuntimeLifecycleState::Suspended)
            return StateFailure();
        for (std::size_t index = 0; index < startedParticipantCount_; ++index) {
            if (Result<void> result = ContainParticipantException([this, index, phase, &context] {
                return participants_[index]->OnPhase(phase, context);
            });
                result.HasError())
                return result;
        }
        return Result<void>::Success();
    }

    /** @copydoc RuntimeLifecycle::DispatchFixedUpdate */
    Result<void> RuntimeLifecycle::DispatchFixedUpdate(const FixedStepContext &context) {
        if (state_ != RuntimeLifecycleState::Running)
            return StateFailure();
        for (std::size_t index = 0; index < startedParticipantCount_; ++index) {
            if (Result<void> result = ContainParticipantException([this, index, &context] {
                return participants_[index]->OnFixedUpdate(context);
            });
                result.HasError())
                return result;
        }
        return Result<void>::Success();
    }

    /** @copydoc RuntimeLifecycle::PrepareFixedCommit */
    Result<void> RuntimeLifecycle::PrepareFixedCommit(const FixedStepContext &context) {
        if (state_ != RuntimeLifecycleState::Running)
            return StateFailure();
        for (std::size_t index = 0; index < startedParticipantCount_; ++index) {
            if (auto prepared = ContainParticipantException([this, index, &context] {
                return participants_[index]->PrepareFixedCommit(context);
            });
                prepared.HasError())
                return prepared;
            if (state_ != RuntimeLifecycleState::Running)
                return StateFailure();
        }
        return Result<void>::Success();
    }

    /** @copydoc RuntimeLifecycle::CommitFixedUpdate */
    void RuntimeLifecycle::CommitFixedUpdate(const FixedStepContext &context) noexcept {
        for (std::size_t index = 0; index < startedParticipantCount_; ++index)
            participants_[index]->CommitFixedUpdate(context);
    }

    /** @copydoc RuntimeLifecycle::AbortFixedUpdate */
    void RuntimeLifecycle::AbortFixedUpdate(const FixedStepContext &context) noexcept {
        for (std::size_t index = startedParticipantCount_; index > 0; --index)
            participants_[index - 1]->AbortFixedUpdate(context);
    }

    /** @copydoc RuntimeLifecycle::Suspend */
    Result<void> RuntimeLifecycle::Suspend() {
        if (state_ != RuntimeLifecycleState::Running)
            return StateFailure();
        state_ = RuntimeLifecycleState::Suspended;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeLifecycle::Resume */
    Result<void> RuntimeLifecycle::Resume() {
        if (state_ != RuntimeLifecycleState::Suspended)
            return StateFailure();
        state_ = RuntimeLifecycleState::Running;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeLifecycle::MarkFailed */
    void RuntimeLifecycle::MarkFailed() noexcept {
        using enum RuntimeLifecycleState;
        if (state_ == Running || state_ == Suspended || state_ == Ready) {
            state_ = Failed;
        }
    }

    /** @copydoc RuntimeLifecycle::Shutdown */
    void RuntimeLifecycle::Shutdown() noexcept {
        using enum RuntimeLifecycleState;
        if (state_ == Stopped || state_ == Stopping)
            return;
        state_ = Stopping;
        while (startedParticipantCount_ > 0) {
            --startedParticipantCount_;
            participants_[startedParticipantCount_]->Shutdown();
        }
        state_ = Stopped;
    }

    /** @copydoc RuntimeLifecycle::State */
    RuntimeLifecycleState RuntimeLifecycle::State() const noexcept {
        return state_;
    }

}  // namespace Horo::Runtime
