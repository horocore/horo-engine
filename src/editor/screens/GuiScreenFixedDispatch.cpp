#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Runtime/FrameScheduler.h"
#include "NavigationErrors.h"

#include <exception>

namespace Horo::Editor {
    namespace {
        class ScreenCallbackScope final {
        public:
            explicit ScreenCallbackScope(bool &active) noexcept : active_(active), previous_(active) {
                active_ = true;
            }

            ~ScreenCallbackScope() {
                active_ = previous_;
            }

            ScreenCallbackScope(const ScreenCallbackScope &) = delete;
            ScreenCallbackScope &operator=(const ScreenCallbackScope &) = delete;

        private:
            bool &active_;
            const bool previous_;
        };

        bool MatchesContext(const Runtime::FixedStepContext &context, const Runtime::RuntimeDispatchFacts &facts) noexcept {
            return context.simulationTick == facts.fixedTick && context.attemptNumber == facts.fixedAttempt &&
                   context.frameNumber == facts.frame && context.fixedDelta == facts.fixedDuration;
        }

        Result<void> InvalidFixedDispatch() {
            return Result<void>::Failure(MakeError(NavigationErrors::FixedDispatchInvalid));
        }
    }  // namespace

    Result<void> GuiScreen::OnRuntimeFixedUpdate(const Runtime::FixedStepContext &context) {
        OnFixedUpdate(context.simulationTick, static_cast<double>(context.fixedDelta.ToNanoseconds()) / 1'000'000'000.0);
        return Result<void>::Success();
    }

    Result<void> GuiScreenHost::BindRuntimeDispatch(Runtime::RuntimeDispatchSource source) {
        if (source.BindingStatus() != Runtime::RuntimeDispatchStatus::Valid)
            return InvalidFixedDispatch();
        if (dispatchSource_.IsValid() || shutdown_ || shutdownPending_ || fixedScreen_)
            return InvalidFixedDispatch();
        if (activeScreen_) {
            if (auto bound = activeScreen_->BindRuntimeDispatch(source); bound.HasError())
                return bound;
        }
        dispatchSource_ = std::move(source);
        return Result<void>::Success();
    }

    Result<void> GuiScreenHost::OnRuntimeFixedUpdate(const Runtime::FixedStepContext &context) {
        Runtime::RuntimeDispatchFacts facts;
        if (context.dispatchEvidence.Read(dispatchSource_, Runtime::RuntimePhase::FixedUpdate, facts) !=
            Runtime::RuntimeDispatchStatus::Valid)
            return InvalidFixedDispatch();
        if (!MatchesContext(context, facts) || facts.fixedTick == 0 || facts.committedTick >= facts.fixedTick)
            return InvalidFixedDispatch();
        if (shutdown_ || shutdownPending_ || fixedScreen_ || !activeScreen_)
            return InvalidFixedDispatch();
        fixedEvidence_ = context.dispatchEvidence;
        fixedFacts_ = facts;
        fixedScreen_ = activeScreen_.get();
        fixedPrepared_ = false;
        // The reservation precedes user code, so scheduler exception containment can still abort it.
        ScreenCallbackScope callback(isScreenCallbackActive_);
        return fixedScreen_->OnRuntimeFixedUpdate(context);
    }

    bool GuiScreenHost::MatchesFixedAttempt(const Runtime::FixedStepContext &context) const noexcept {
        return fixedScreen_ && fixedEvidence_.RefersToSameDispatch(context.dispatchEvidence) && MatchesContext(context, fixedFacts_);
    }

    Result<void> GuiScreenHost::PrepareRuntimeFixedCommit(const Runtime::FixedStepContext &context) {
        Runtime::RuntimeDispatchFacts facts;
        if (context.dispatchEvidence.Read(dispatchSource_, Runtime::RuntimePhase::FixedUpdate, facts) !=
            Runtime::RuntimeDispatchStatus::Valid)
            return InvalidFixedDispatch();
        if (!MatchesFixedAttempt(context) || fixedPrepared_ || shutdownPending_ || facts.committedTick >= facts.fixedTick)
            return InvalidFixedDispatch();
        ScreenCallbackScope callback(isScreenCallbackActive_);
        auto result = fixedScreen_->PrepareRuntimeFixedCommit(context);
        if (shutdownPending_)
            return InvalidFixedDispatch();
        if (result.HasValue())
            fixedPrepared_ = true;
        return result;
    }

    void GuiScreenHost::CommitRuntimeFixedUpdate(const Runtime::FixedStepContext &context) noexcept {
        Runtime::RuntimeDispatchFacts facts;
        if (context.dispatchEvidence.Read(dispatchSource_, Runtime::RuntimePhase::FixedUpdate, facts) !=
            Runtime::RuntimeDispatchStatus::Valid)
            std::terminate();
        if (!MatchesFixedAttempt(context) || !fixedPrepared_ || facts.committedTick != fixedFacts_.fixedTick ||
            facts.committedAttempt != fixedFacts_.fixedAttempt || facts.committedFrame != fixedFacts_.frame ||
            facts.committedDuration != fixedFacts_.fixedDuration)
            std::terminate();
        ScreenCallbackScope callback(isScreenCallbackActive_);
        fixedScreen_->CommitRuntimeFixedUpdate(context);
        ReleaseFixedAttempt();
    }

    void GuiScreenHost::AbortRuntimeFixedUpdate(const Runtime::FixedStepContext &context) noexcept {
        Runtime::RuntimeDispatchFacts ignored;
        const auto status = context.dispatchEvidence.Read(dispatchSource_, Runtime::RuntimePhase::FixedUpdate, ignored);
        if (status != Runtime::RuntimeDispatchStatus::Valid && status != Runtime::RuntimeDispatchStatus::Retired)
            return;
        if (!MatchesFixedAttempt(context))
            return;
        ScreenCallbackScope callback(isScreenCallbackActive_);
        fixedScreen_->AbortRuntimeFixedUpdate(context);
        ReleaseFixedAttempt();
    }

    void GuiScreenHost::ReleaseFixedAttempt() noexcept {
        fixedScreen_ = nullptr;
        fixedPrepared_ = false;
        fixedEvidence_ = {};
        fixedFacts_ = {};
    }
}  // namespace Horo::Editor
