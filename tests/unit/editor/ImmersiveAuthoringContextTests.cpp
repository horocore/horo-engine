#include "../../../apps/HoroEditor/app/ImmersiveAuthoringContext.h"

#include <catch2/catch_test_macros.hpp>
#include <future>

namespace Horo::Editor {
    namespace {
        /** @brief Emits a deterministic completion, with a hook for an editor transition during inference. */
        struct PlanningModel final : Agent::IModelProvider {
            std::function<void()> duringInference;
            std::size_t calls{};
            std::optional<Agent::ModelToolIntent> tool;

            Agent::ProviderDiscovery Discover(std::stop_token) override {
                return {};
            }

            Agent::ModelOutcome Stream(const Agent::ModelRequest &, const Agent::ModelEventSink &sink, std::stop_token) override {
                ++calls;
                if (duringInference)
                    duringInference();
                if (tool && !sink({.kind = Agent::ModelEventKind::ToolIntent, .tool = *tool}))
                    return {Agent::ModelError{Agent::ModelErrorCode::Cancelled, "Stopped by callback", std::nullopt}};
                if (!sink({.kind = Agent::ModelEventKind::TextDelta, .text = "Plan"}) || !sink({.kind = Agent::ModelEventKind::Completed}))
                    return {Agent::ModelError{Agent::ModelErrorCode::Cancelled, "Stopped by callback", std::nullopt}};
                return {};
            }
        };

        /** @brief Creates a bounded payload with no automatic export of editor or XR state. */
        Agent::ModelRequest Prompt() {
            return {.model = "qualification", .messages = {{.role = Agent::ModelRole::User, .text = "Inspect my selection"}}};
        }

        ImmersiveContextIdentity Opening() {
            const XR::XRSessionId session{{XR::XRRuntimeGeneration::Create(1).Value(), {0, 1}}, {0, 1}};
            return {1,
                    1,
                    1,
                    session,
                    {session, {0, 1}},
                    1,
                    1,
                    1,
                    XR::XRCapabilityRevision::Create(1).Value(),
                    ImmersiveAuthoringMode::Edit,
                    0,
                    true,
                    true};
        }

        TEST_CASE("Immersive planning captures a bounded owned selection", "[agent][immersive]") {
            ImmersiveAuthoringContext owner;
            std::array<SceneSource::SceneObjectId, 2> selection{{{3}, {2}}};
            const auto first = owner.Publish(Opening(), selection);
            REQUIRE(first);
            const auto same = owner.Publish(Opening(), selection);
            REQUIRE(same);
            CHECK(same->Generation() == first->Generation());
            selection[0] = {4};
            CHECK(first->Selection()[0].value == 3);
            CHECK(owner.Admit(*first, ImmersiveRequestKind::Plan) == ImmersiveAdmission::Admitted);
            const auto next = owner.Publish(Opening(), selection);
            REQUIRE(next);
            CHECK(next->Generation() > first->Generation());
            CHECK_FALSE(first->IsCurrent());
            CHECK(owner.Admit(*first, ImmersiveRequestKind::Plan) == ImmersiveAdmission::StaleContext);
        }

        TEST_CASE("Immersive admission rejects another owner's context and changes of authority", "[agent][immersive]") {
            ImmersiveAuthoringContext owner;
            ImmersiveAuthoringContext other;
            auto identity = Opening();
            const auto first = owner.Publish(identity, {});
            const auto foreign = other.Publish(identity, {});
            REQUIRE(first);
            REQUIRE(foreign);
            CHECK(owner.Admit(*foreign, ImmersiveRequestKind::Plan) == ImmersiveAdmission::StaleContext);
            SECTION("project replacement") {
                ++identity.projectOpening;
            }
            SECTION("document replacement") {
                ++identity.documentOpening;
            }
            SECTION("document edit") {
                ++identity.documentRevision;
            }
            SECTION("XR session replacement") {
                ++identity.xrSession.slot.generation;
                identity.referenceSpace.session = identity.xrSession;
            }
            SECTION("reference space replacement") {
                ++identity.referenceSpace.slot.generation;
            }
            SECTION("user change") {
                ++identity.user;
            }
            SECTION("conversation change") {
                ++identity.conversation;
            }
            SECTION("tool change") {
                ++identity.toolContext;
            }
            SECTION("capability change") {
                identity.capabilities = XR::XRCapabilityRevision::Create(2).Value();
            }
            SECTION("mode transition") {
                identity.mode = ImmersiveAuthoringMode::Simulate;
                identity.worldGeneration = 2;
            }
            SECTION("permission withdrawal") {
                identity.planningAllowed = false;
            }
            REQUIRE(owner.Publish(identity, {}));
            CHECK_FALSE(first->IsCurrent());
            CHECK(owner.Admit(*first, ImmersiveRequestKind::Plan) == ImmersiveAdmission::StaleContext);
        }

        TEST_CASE("Immersive context rejects bad observations and fences Play planning", "[agent][immersive]") {
            ImmersiveAuthoringContext owner;
            auto identity = Opening();
            identity.mode = ImmersiveAuthoringMode::Play;
            identity.worldGeneration = 6;
            const auto play = owner.Publish(identity, {});
            REQUIRE(play);
            CHECK(owner.Admit(*play, ImmersiveRequestKind::Plan) == ImmersiveAdmission::ModeDenied);
            CHECK(owner.Admit(*play, ImmersiveRequestKind::SpeechCapture) == ImmersiveAdmission::ModeDenied);
            CHECK(owner.Admit(*play, ImmersiveRequestKind::Diagnostics) == ImmersiveAdmission::Admitted);
            SECTION("reference space belongs to a different session") {
                ++identity.referenceSpace.session.slot.generation;
                CHECK_FALSE(owner.Publish(identity, {}));
            }
            SECTION("selection too large") {
                std::array<SceneSource::SceneObjectId, ImmersivePlanningContext::MaximumSelection + 1> selection{};
                CHECK_FALSE(owner.Publish(identity, selection));
            }
            SECTION("duplicate selection") {
                const std::array<SceneSource::SceneObjectId, 2> selection{{{5}, {5}}};
                CHECK_FALSE(owner.Publish(identity, selection));
            }
            CHECK_FALSE(play->IsCurrent());
        }

        TEST_CASE("Local Play speech capture cannot acquire model or editing authority", "[agent][immersive]") {
            using enum ImmersiveRequestKind;
            ImmersiveAuthoringContext owner;
            auto identity = Opening();
            identity.mode = ImmersiveAuthoringMode::Play;
            identity.worldGeneration = 9;
            identity.user = 0;
            identity.conversation = 0;
            identity.toolContext = 0;
            identity.planningAllowed = false;
            identity.localSpeechOnly = true;
            const auto context = owner.Publish(identity, {});
            REQUIRE(context);
            CHECK(owner.Admit(*context, SpeechCapture) == ImmersiveAdmission::Admitted);
            CHECK(owner.Admit(*context, Plan) == ImmersiveAdmission::ModeDenied);
            CHECK_FALSE(owner.CaptureRequest(*context, Diagnostics, Prompt()));
            CHECK_FALSE(owner.CaptureRequest(*context, SpeechCapture, Prompt()));

            identity.planningAllowed = true;
            CHECK_FALSE(owner.Publish(identity, {}));
            CHECK(owner.Admit(*context, SpeechCapture) == ImmersiveAdmission::StaleContext);
        }

        TEST_CASE("Immersive observation does not grant authority without host admission", "[agent][immersive]") {
            ImmersiveAuthoringContext owner;
            auto identity = Opening();
            identity.planningAllowed = false;
            identity.diagnosticsAllowed = false;
            const auto context = owner.Publish(identity, {});
            REQUIRE(context);
            CHECK(owner.Admit(*context, ImmersiveRequestKind::Plan) == ImmersiveAdmission::PermissionDenied);
            CHECK(owner.Admit(*context, ImmersiveRequestKind::Diagnostics) == ImmersiveAdmission::PermissionDenied);
            CHECK(owner.Admit(*context, static_cast<ImmersiveRequestKind>(255)) == ImmersiveAdmission::InvalidRequest);
        }

        TEST_CASE("Immersive worker observes revocation after owner teardown", "[agent][immersive]") {
            auto owner = std::make_unique<ImmersiveAuthoringContext>();
            const auto context = owner->Publish(Opening(), {});
            REQUIRE(context);
            std::promise<void> teardown;
            auto worker = std::async(std::launch::async, [capture = *context, ready = teardown.get_future()] {
                ready.wait();
                return capture.IsCurrent();
            });
            owner.reset();
            teardown.set_value();
            CHECK_FALSE(worker.get());
        }

        TEST_CASE("Immersive inference requires an admitted owner context and excludes tools", "[agent][immersive][turn]") {
            ImmersiveAuthoringContext owner;
            ImmersiveAuthoringContext other;
            auto identity = Opening();
            identity.mode = ImmersiveAuthoringMode::Play;
            identity.worldGeneration = 1;
            const auto context = owner.Publish(identity, {});
            REQUIRE(context);
            CHECK_FALSE(other.CaptureRequest(*context, ImmersiveRequestKind::Diagnostics, Prompt()));
            CHECK_FALSE(owner.CaptureRequest(*context, ImmersiveRequestKind::Plan, Prompt()));
            auto toolPrompt = Prompt();
            toolPrompt.requiredFeatures.bits = static_cast<std::uint32_t>(Agent::ModelFeature::Tools);
            CHECK_FALSE(owner.CaptureRequest(*context, ImmersiveRequestKind::Diagnostics, std::move(toolPrompt)));
            auto toolHistory = Prompt();
            toolHistory.messages.insert(toolHistory.messages.begin(),
                                        {.role = Agent::ModelRole::Tool, .text = "Old result", .toolCallId = "previous"});
            CHECK_FALSE(owner.CaptureRequest(*context, ImmersiveRequestKind::Diagnostics, std::move(toolHistory)));
            CHECK_FALSE(owner.CaptureRequest(*context, ImmersiveRequestKind::Diagnostics, {}));

            auto request = owner.CaptureRequest(*context, ImmersiveRequestKind::Diagnostics, Prompt());
            REQUIRE(request);
            CHECK(request->Generation() == context->Generation());
            PlanningModel model;
            const auto result = request->Run(model, 1);
            REQUIRE(result.status == Agent::AgentTurnStatus::Completed);
            REQUIRE(result.messages.size() == 2);
            CHECK(result.messages.back().text == "Plan");
            CHECK(request->IsCurrent());
            CHECK(request->Run(model, 2).status == Agent::AgentTurnStatus::InvalidRequest);
            CHECK(model.calls == 1);
        }

        TEST_CASE("Immersive inference does not dispatch after project replacement", "[agent][immersive][turn]") {
            ImmersiveAuthoringContext owner;
            auto identity = Opening();
            const auto context = owner.Publish(identity, {});
            REQUIRE(context);
            auto request = owner.CaptureRequest(*context, ImmersiveRequestKind::Plan, Prompt());
            REQUIRE(request);
            PlanningModel model;
            ++identity.projectOpening;
            REQUIRE(owner.Publish(identity, {}));
            const auto result = request->Run(model, 1);
            CHECK(result.status == Agent::AgentTurnStatus::ContextDenied);
            CHECK(result.messages.empty());
            CHECK(model.calls == 0);
            CHECK_FALSE(request->IsCurrent());
        }

        TEST_CASE("Immersive plan review requires current authoring mode", "[agent][immersive][turn][plan]") {
            using enum Agent::AgentTurnStatus;
            ImmersiveAuthoringContext owner;
            auto identity = Opening();
            const auto context = owner.Publish(identity, {});
            REQUIRE(context);
            auto request = owner.CapturePlan(*context, Prompt());
            REQUIRE(request);
            PlanningModel model;
            CHECK(request->Run(model, 1).status == InvalidRequest);
            const auto validate = [](const Agent::ModelToolIntent &) {
                return Agent::AgentPlanToolInspection{ToolDenied};
            };
            const auto valid = request->Plan(model, 2, {}, validate);
            REQUIRE(valid.Status() == Completed);
            const auto *draft = valid.Read(*context);
            REQUIRE(draft);
            REQUIRE(draft->draft);
            CHECK(draft->draft->text == "Plan");
            ImmersiveAuthoringContext other;
            const auto foreign = other.Publish(identity, {});
            REQUIRE(foreign);
            CHECK_FALSE(valid.Read(*foreign));
            auto old = owner.CapturePlan(*context, Prompt());
            REQUIRE(old);
            identity.mode = ImmersiveAuthoringMode::Play;
            identity.worldGeneration = 1;
            const auto play = owner.Publish(identity, {});
            REQUIRE(play);
            CHECK_FALSE(owner.CapturePlan(*play, Prompt()));
            CHECK(valid.Status() == ContextDenied);
            CHECK_FALSE(valid.Read(*context));
            const auto rejected = old->Plan(model, 3, {}, validate);
            CHECK(rejected.Status() == ContextDenied);
            CHECK_FALSE(rejected.Read(*play));
            CHECK(model.calls == 1);
        }

        TEST_CASE("Immersive plan is withdrawn when the worker is cancelled before adoption", "[agent][immersive][turn][plan]") {
            using enum Agent::AgentTurnStatus;
            ImmersiveAuthoringContext owner;
            const auto context = owner.Publish(Opening(), {});
            REQUIRE(context);
            auto request = owner.CapturePlan(*context, Prompt());
            REQUIRE(request);
            PlanningModel model;
            std::stop_source stop;
            const auto review = request->Plan(model, 1, {}, [](const Agent::ModelToolIntent &) {
                return Agent::AgentPlanToolInspection{ToolDenied};
            }, stop.get_token());
            REQUIRE(review.Read(*context));
            stop.request_stop();
            CHECK(review.Status() == Cancelled);
            CHECK_FALSE(review.Read(*context));
        }

        TEST_CASE("Immersive planning rejects object targets from a different scene revision", "[agent][immersive][turn][plan]") {
            using enum Agent::AgentTurnStatus;
            ImmersiveAuthoringContext owner;
            const auto context = owner.Publish(Opening(), {});
            REQUIRE(context);
            auto prompt = Prompt();
            prompt.tools.emplace_back("scene.change", "Propose a scene change", R"({"type":"object"})");
            prompt.requiredFeatures.bits |= static_cast<std::uint32_t>(Agent::ModelFeature::Tools);
            auto pending = owner.CapturePlan(*context, std::move(prompt));
            REQUIRE(pending);
            PlanningModel model;
            model.tool = Agent::ModelToolIntent{"change", "scene.change", "{}"};
            auto scope = Agent::AgentPlanObjectScope{context->Identity().documentOpening, context->Identity().documentRevision, {42}};
            auto expected = Completed;
            SECTION("the exact scope remains available") {}
            SECTION("a different document opening is denied") {
                ++scope.opening;
                expected = ContextDenied;
            }
            SECTION("an outdated revision is denied") {
                ++scope.revision;
                expected = ContextDenied;
            }
            const auto result = pending->Plan(model, 1, {}, [&](const Agent::ModelToolIntent &) {
                return Agent::AgentPlanToolInspection{Completed, Agent::AgentPlannedTool{Agent::AgentPlannedEffect::Presentation, scope}};
            });
            CHECK(result.Status() == expected);
            CHECK_FALSE(result.Read(*context));
            CHECK_FALSE(result.Read(*context, [](const auto &) {
                return false;
            }));
            CHECK((result.Read(*context, [&](const auto &targets) {
                CHECK(targets.objects == scope.objects);
                return true;
            }) != nullptr) == (expected == Completed));
        }

        TEST_CASE("Immersive inference suppresses a worker reply after XR session retirement", "[agent][immersive][turn]") {
            ImmersiveAuthoringContext owner;
            const auto context = owner.Publish(Opening(), {});
            REQUIRE(context);
            auto request = owner.CaptureRequest(*context, ImmersiveRequestKind::Plan, Prompt());
            REQUIRE(request);
            std::promise<void> dispatched;
            std::promise<void> resume;
            auto resumeSignal = resume.get_future();
            auto worker = std::async(std::launch::async, [request = std::move(*request), &dispatched, &resumeSignal]() mutable {
                PlanningModel model;
                model.duringInference = [&] {
                    dispatched.set_value();
                    resumeSignal.wait();
                };
                return request.Run(model, 1);
            });
            const auto ready = dispatched.get_future().wait_for(std::chrono::seconds{2});
            owner.Revoke();
            resume.set_value();
            const auto result = worker.get();
            CHECK(ready == std::future_status::ready);
            CHECK(result.status == Agent::AgentTurnStatus::ContextDenied);
            CHECK(result.messages.empty());
        }
    }  // namespace
}  // namespace Horo::Editor
