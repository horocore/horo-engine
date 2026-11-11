#include "Horo/Extensions/ExtensionErrors.h"
#include "JavaScriptModuleInternal.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace Horo::Extensions::JavaScriptModuleDetail {
    namespace {
        using Output = Result<std::unique_ptr<ScriptModulePreparedRebind>>;

        struct PreparationLane final {
            State &state;
            bool kept{};

            explicit PreparationLane(State &value) : state(value) {
                state.executing = true;
                state.rebindPrepared = true;
            }

            PreparationLane(const PreparationLane &) = delete;
            PreparationLane &operator=(const PreparationLane &) = delete;

            ~PreparationLane() {
                state.executing = false;
                if (!kept)
                    state.rebindPrepared = false;
            }
        };

        Result<void> Check(State &state, const std::shared_ptr<ScriptModuleContext> &next) {
            state.RequireIdle();
            if (state.closed.load(std::memory_order_acquire) || !next || next == state.authority)
                return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationContextRevoked));
            if (state.rebindPrepared || !state.PollPredecessor() || !ReapScriptModulePublications(state))
                return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationCapacityExceeded,
                                                       "One preparation or draining predecessor already owns this VM update slot."));
            if (state.bindingRevision == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationCapacityExceeded));
            return state.authority->ValidateReplacement(*next);
        }

        class Prepared final : public ScriptModuleRebindAccess::Implementation {
        public:
            Prepared(std::shared_ptr<State> state, std::shared_ptr<ScriptModuleContext> next,
                     std::vector<std::shared_ptr<const ScriptModuleBindingPlan>> plans)
                : state_(std::move(state)), next_(std::move(next)), plans_(std::move(plans)), revision_(state_->bindingRevision),
                  publication_(std::make_shared<const ScriptModulePublishedContexts>(next_, state_->authority)),
                  activeOnly_(std::make_shared<const ScriptModulePublishedContexts>(next_, nullptr)) {}

            ~Prepared() override {
                assert(state_->owner == std::this_thread::get_id());
                for (auto value : namespaces_)
                    JS_FreeValue(state_->context, value);
                if (!consumed_)
                    state_->rebindPrepared = false;
            }

            void Build() {
                namespaces_ = PrepareJavaScriptNamespaces(*state_, next_, plans_);
            }

            Result<void> Validate() override {
                try {
                    state_->RequireIdle();
                    validated_ = false;
                    if (consumed_ || state_->bindingRevision != revision_ || !state_->rebindPrepared)
                        return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
                    if (state_->closed.load(std::memory_order_acquire))
                        return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationContextRevoked));
                    if (auto valid = state_->authority->ValidateReplacement(*next_); valid.HasError())
                        return valid;
                    validated_ = true;
                    return Result<void>::Success();
                } catch (const ConversionFailure &error) {
                    return Result<void>::Failure(error.error);
                } catch (...) {
                    return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationAbandoned));
                }
            }

            bool Commit() noexcept override {
                if (state_->owner != std::this_thread::get_id())
                    return false;
                if (!validated_ || consumed_ || state_->bindingRevision != revision_)
                    return false;
                if (!ScriptModuleRebindAccess::CommitContext(*state_->authority, *next_))
                    return false;
                state_->predecessor = std::move(state_->authority);
                state_->authority = std::move(next_);
                state_->values.authority = state_->authority;
                state_->plans.swap(plans_);
                state_->imports.swap(namespaces_);
                state_->activeOnly = std::move(activeOnly_);
                state_->retiredPublications[0] =
                    std::atomic_exchange_explicit(&state_->published, std::move(publication_), std::memory_order_seq_cst);
                ++state_->bindingRevision;
                state_->rebindPrepared = false;
                consumed_ = true;
                if (state_->closed.load(std::memory_order_seq_cst))
                    std::atomic_load_explicit(&state_->published, std::memory_order_seq_cst)->Close();
                return true;
            }

        private:
            std::shared_ptr<State> state_;
            std::shared_ptr<ScriptModuleContext> next_;
            std::vector<std::shared_ptr<const ScriptModuleBindingPlan>> plans_;
            std::vector<JSValue> namespaces_;
            std::uint64_t revision_;
            std::shared_ptr<const ScriptModulePublishedContexts> publication_;
            std::shared_ptr<const ScriptModulePublishedContexts> activeOnly_;
            bool validated_{};
            bool consumed_{};
        };
    }  // namespace

    bool State::PendingFrom(const std::shared_ptr<ScriptModuleContext> &context) const noexcept {
        if (closed.load(std::memory_order_acquire))
            return false;
        return std::ranges::any_of(pending, [&](const auto &item) {
            return item.origin == context;
        });
    }

    bool State::PollPredecessor() {
        return PollScriptModulePredecessor(*this);
    }

    Result<std::unique_ptr<ScriptModulePreparedRebind>> PrepareJavaScriptRebind(std::shared_ptr<State> state,
                                                                                std::shared_ptr<ScriptModuleContext> next) {
        try {
            if (auto checked = Check(*state, next); checked.HasError())
                return Output::Failure(checked.ErrorValue());
            PreparationLane lane{*state};
            auto plans = PrepareScriptModulePlans(*next);
            if (plans.HasError())
                return Output::Failure(plans.ErrorValue());
            auto prepared = std::make_unique<Prepared>(state, std::move(next), std::move(plans).Value());
            prepared->Build();
            auto result = ScriptModuleRebindAccess::Create(std::move(prepared));
            lane.kept = true;
            return Output::Success(std::move(result));
        } catch (const ConversionFailure &error) {
            Value discarded{state->context, JS_GetException(state->context)};
            return Output::Failure(error.error);
        } catch (...) {
            Value discarded{state->context, JS_GetException(state->context)};
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationAbandoned));
        }
    }
}  // namespace Horo::Extensions::JavaScriptModuleDetail
