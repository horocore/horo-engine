#include "Horo/Extensions/ExtensionErrors.h"
#include "JavaScriptModuleInternal.h"

#include <cassert>
#include <stdexcept>
#ifndef HORO_QUICKJS_SCRIPT_SOURCE_SHA256
#error The JavaScript adapter must be bound to the actual pinned VM source by its owning build target.
#endif
#ifndef HORO_SCRIPT_MODULE_BINDING_SHA256
#error The adapter target must identify the actual binding-source snapshot.
#endif
#ifndef HORO_SCRIPT_MODULE_BUILD_PROFILE
#error The adapter target must identify its actual platform, architecture and build profile.
#endif
namespace Horo::Extensions {
    namespace {
        using namespace JavaScriptModuleDetail;

        Error Unexpected() {
            return MakeError(ExtensionErrors::ScriptInvocationAbandoned, "Module operation allocation or initialization failed.");
        }

        class Runtime final : public IScriptModuleRuntime {
        public:
            explicit Runtime(std::unique_ptr<State> state) : state_(std::move(state)) {}

            ~Runtime() override {
                assert(state_->owner == std::this_thread::get_id());
                Close();
            }

            Result<void> Start(std::string_view source) override {
                const auto state = state_;
                try {
                    state->Prepare(source);
                    state->Start();
                    return Result<void>::Success();
                } catch (const ConversionFailure &error) {
                    return Result<void>::Failure(error.error);
                } catch (...) {
                    return Result<void>::Failure(Unexpected());
                }
            }

            Result<void> Poll() override {
                const auto state = state_;
                try {
                    state->Poll();
                    return Result<void>::Success();
                } catch (const ConversionFailure &error) {
                    return Result<void>::Failure(error.error);
                } catch (...) {
                    return Result<void>::Failure(Unexpected());
                }
            }

            std::optional<ScriptCallResult> ResultSnapshot() const override {
                state_->RequireOwner();
                return state_->result;
            }

            Result<std::unique_ptr<ScriptModulePreparedRebind>> PrepareRebind(std::shared_ptr<ScriptModuleContext> next) override {
                return JavaScriptModuleDetail::PrepareJavaScriptRebind(state_, std::move(next));
            }

            Result<bool> PollRebindRetirement() override {
                try {
                    state_->RequireIdle();
                    return Result<bool>::Success(state_->PollPredecessor());
                } catch (const ConversionFailure &error) {
                    return Result<bool>::Failure(error.error);
                } catch (...) {
                    return Result<bool>::Failure(Unexpected());
                }
            }

            Result<void> ReplaceCompatible(std::shared_ptr<ScriptModuleContext> next, std::string_view source) override {
                try {
                    state_->RequireIdle();
                    if (source.size() != state_->sourceBytes ||
                        ComputeSha256(std::as_bytes(std::span{source.data(), source.size()})) != state_->sourceDigest)
                        return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid,
                                                               "A provider binding refresh cannot replace or rerun source."));
                    auto prepared = PrepareRebind(std::move(next));
                    if (prepared.HasError())
                        return Result<void>::Failure(prepared.ErrorValue());
                    auto ready = prepared.Value()->Validate();
                    if (ready.HasError())
                        return ready;
                    if (!prepared.Value()->Commit())
                        return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
                    return Result<void>::Success();
                } catch (const ConversionFailure &error) {
                    return Result<void>::Failure(error.error);
                } catch (...) {
                    return Result<void>::Failure(Unexpected());
                }
            }

            void Close() noexcept override {
                state_->Cancel();
            }

        private:
            std::shared_ptr<State> state_;
        };
    }  // namespace

    ScriptModuleRuntimeIdentity JavaScriptModuleRuntimeIdentity(std::string profile) {
        if (profile != "headless" && profile != "editor-play")
            throw std::invalid_argument{"Unsupported module operation profile"};
        const std::string input = std::string{"horo.module-binding.v1|quickjs-ng|"} + JS_GetVersion() + "|" +
                                  HORO_QUICKJS_SCRIPT_SOURCE_SHA256 + "|" + HORO_SCRIPT_MODULE_BINDING_SHA256 + "|" +
                                  HORO_SCRIPT_MODULE_BUILD_PROFILE + "|" + profile;
        return {ScriptModuleRuntimeKind::JavaScriptNg, JS_GetVersion(), std::move(profile),
                ComputeSha256(std::as_bytes(std::span{input.data(), input.size()}))};
    }

    Result<std::unique_ptr<IScriptModuleRuntime>> CreateJavaScriptModuleRuntime(std::shared_ptr<ScriptModuleContext> context) {
        using Output = Result<std::unique_ptr<IScriptModuleRuntime>>;
        try {
            if (!context || !context->IsUsable())
                return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationContextRevoked));
            if (context->Runtime().targetProfile != "headless" && context->Runtime().targetProfile != "editor-play")
                return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid, "Unsupported module operation profile."));
            if (context->Runtime() != JavaScriptModuleRuntimeIdentity(context->Runtime().targetProfile))
                return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid,
                                                 "Context does not match this actual runtime/build/profile identity."));

            auto state = std::make_unique<JavaScriptModuleDetail::State>(std::move(context));
            state->Initialize();
            return Output::Success(std::make_unique<Runtime>(std::move(state)));
        } catch (const JavaScriptModuleDetail::ConversionFailure &error) {
            return Output::Failure(error.error);
        } catch (...) {
            return Output::Failure(Unexpected());
        }
    }
}  // namespace Horo::Extensions
