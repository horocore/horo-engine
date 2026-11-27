#include "../../../apps/HoroEditor/app/ImmersiveAuthoringContext.h"
#include "AgentTurnTestTools.h"
#include "EditorCommandMcpSession.h"
#include "EditorInspectionMcpTools.h"
#include "Horo/Mcp/McpErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <future>
#include <mutex>

namespace Horo::Editor {
    namespace {
        /** @brief Emits a deterministic completion, with a hook for an editor transition during inference. */
        struct PlanningModel final : Agent::IModelProvider {
            std::function<void()> duringInference;
            std::function<void(std::stop_token)> quietInference;
            std::size_t calls{};
            std::vector<Agent::ModelToolIntent> tools;
            std::optional<Agent::ModelErrorCode> failure;
            std::vector<Agent::ModelTool> advertised;

            Agent::ProviderDiscovery Discover(std::stop_token) override {
                return {};
            }

            Agent::ModelOutcome Stream(const Agent::ModelRequest &request, const Agent::ModelEventSink &sink, std::stop_token stop) override {
                ++calls;
                advertised = request.tools;
                if (duringInference)
                    duringInference();
                if (quietInference)
                    quietInference(stop);
                for (const auto &tool : tools)
                    if (!sink({.kind = Agent::ModelEventKind::ToolIntent, .tool = tool}))
                        return {Agent::ModelError{Agent::ModelErrorCode::Cancelled, "Stopped by callback", std::nullopt}};
                if (!sink({.kind = Agent::ModelEventKind::TextDelta, .text = "Plan"}))
                    return {Agent::ModelError{Agent::ModelErrorCode::Cancelled, "Stopped by callback", std::nullopt}};
                if (failure)
                    return {Agent::ModelError{*failure, "Fixture failure", std::nullopt}};
                if (!sink({.kind = Agent::ModelEventKind::Completed}))
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

        TEST_CASE("Immersive inference follows upstream document cancellation without republishing", "[agent][immersive][turn]") {
            ImmersiveAuthoringContext owner;
            CancellationSource document;
            const auto context = owner.Publish(Opening(), {}, document.Token());
            REQUIRE(context);
            const auto same = owner.Publish(Opening(), {}, document.Token());
            REQUIRE(same);
            CHECK(same->SharesGeneration(*context));
            auto request = owner.CaptureRequest(*context, ImmersiveRequestKind::Diagnostics, Prompt());
            REQUIRE(request);
            PlanningModel model;
            SECTION("document cancelled during inference") {
                model.duringInference = [&] {
                    document.RequestCancellation();
                };
                CHECK(request->Run(model, 1).status == Agent::AgentTurnStatus::ContextDenied);
                CHECK(model.calls == 1);
                CHECK_FALSE(owner.Publish(Opening(), {}, document.Token()));
            }
            SECTION("same identity with a new upstream scope") {
                CancellationSource replacement;
                const auto next = owner.Publish(Opening(), {}, replacement.Token());
                REQUIRE(next);
                CHECK_FALSE(next->SharesGeneration(*context));
                CHECK(request->Run(model, 1).status == Agent::AgentTurnStatus::ContextDenied);
                CHECK(model.calls == 0);
            }
            CHECK_FALSE(request->IsCurrent());
            CHECK(owner.Admit(*context, ImmersiveRequestKind::Diagnostics) == ImmersiveAdmission::StaleContext);
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

        TEST_CASE("Immersive planning reports a scoped rate limit without retaining a draft", "[agent][immersive][turn][plan]") {
            ImmersiveAuthoringContext owner;
            const auto context = owner.Publish(Opening(), {});
            REQUIRE(context);
            auto request = owner.CapturePlan(*context, Prompt());
            REQUIRE(request);
            PlanningModel model;
            model.failure = Agent::ModelErrorCode::RateLimited;
            std::stop_source stop;
            const auto review = request->Plan(model, 1, {}, [](const auto &) {
                return Agent::AgentPlanToolInspection{Agent::AgentTurnStatus::ToolDenied};
            }, stop.get_token());

            CHECK(review.Status() == Agent::AgentTurnStatus::ProviderFailed);
            CHECK(review.ProviderFailure(*context) == Agent::ModelErrorCode::RateLimited);
            CHECK_FALSE(review.Read(*context));
            CHECK(model.calls == 1);

            SECTION("another owner cannot read the reason") {
                ImmersiveAuthoringContext other;
                const auto foreign = other.Publish(Opening(), {});
                REQUIRE(foreign);
                CHECK_FALSE(review.ProviderFailure(*foreign));
            }
            SECTION("the original scope is revoked") {
                owner.Revoke();
                CHECK_FALSE(review.ProviderFailure(*context));
            }
            SECTION("the worker is cancelled before presentation") {
                stop.request_stop();
                CHECK_FALSE(review.ProviderFailure(*context));
            }
        }

        TEST_CASE("Immersive planning requires the exact scene scope for proposed writes", "[agent][immersive][turn][plan]") {
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
            model.tools = {{"change", "scene.change", "{}"}};
            auto scope = Agent::AgentPlanObjectScope{context->Identity().documentOpening, context->Identity().documentRevision, {42}};
            bool hasScope = true;
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
            SECTION("an unscoped write is denied") {
                hasScope = false;
                expected = ContextDenied;
            }
            const auto result = pending->Plan(model, 1, {}, [&](const Agent::ModelToolIntent &) {
                return Agent::AgentPlanToolInspection{
                    Completed, Agent::AgentPlannedTool{Agent::AgentPlannedEffect::Mutation, hasScope ? std::optional{scope} : std::nullopt}};
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

        TEST_CASE("Immersive review stops inspecting targets when the first inspection revokes authority", "[agent][immersive][turn][plan]") {
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
            model.tools = {{"first", "scene.change", "{}"}, {"second", "scene.change", "{}"}};
            const auto &identity = context->Identity();
            const auto review = pending->Plan(model, 1, {}, [&](const Agent::ModelToolIntent &intent) {
                const Agent::AgentPlanObjectScope scope{identity.documentOpening, identity.documentRevision,
                                                       {intent.callId == "first" ? 42U : 43U}};
                return Agent::AgentPlanToolInspection{Completed, Agent::AgentPlannedTool{Agent::AgentPlannedEffect::Presentation, scope}};
            });
            REQUIRE(review.Status() == Completed);
            std::size_t inspected{};
            CHECK_FALSE(review.Read(*context, [&](const auto &) {
                ++inspected;
                owner.Revoke();
                return true;
            }));
            CHECK(inspected == 1);
            CHECK(review.Status() == ContextDenied);
        }

        TEST_CASE("Immersive planning uses the live editor session's project and tool grants", "[agent][immersive][turn][plan]") {
            using enum Agent::AgentTurnStatus;
            constexpr std::string_view projectIdentity = "project-one";
            const auto identity = Opening();
            ImmersiveAuthoringContext owner;
            const auto context = owner.Publish(identity, {});
            REQUIRE(context);

            std::size_t staged{};
            std::function<bool()> viewportAuthority;
            bool targetsAvailable = true;
            const auto endpoint = std::make_shared<const EditorInspectionCommandEndpoint>(
                EditorInspectionCommandEndpoint{.project = std::string{projectIdentity},
                                                .authorizationRevision = 3,
                                                .owner = std::this_thread::get_id(),
                                                .readTransforms =
                                                    [](const ObjectInspectionRequest &) {
                return Result<TransformInspectionPage>::Success({});
            },
                                                .proposeEdits =
                                                    [&](const InspectionSceneRevision revision, auto transforms, auto renames, auto lights,
                                                        std::function<bool()> authorized) {
                CHECK((revision == InspectionSceneRevision{identity.documentOpening, identity.documentRevision}));
                CHECK(transforms.empty());
                CHECK(lights.empty());
                REQUIRE(renames.size() == 1);
                CHECK(renames.front().object == 42);
                CHECK(renames.front().name == "Reviewed");
                viewportAuthority = std::move(authorized);
                ++staged;
                return Result<std::uint64_t>::Success(1);
            },
                                                .inspectEditTargets = [&](InspectionSceneRevision revision,
                                                                          std::span<const std::uint64_t> objects) {
                CHECK((revision == InspectionSceneRevision{identity.documentOpening, identity.documentRevision}));
                REQUIRE(objects.size() == 1);
                CHECK(objects.front() == 42);
                return targetsAvailable ? Result<void>::Success() : Result<void>::Failure(MakeError(Mcp::McpErrors::OwnerUnavailable));
            }});
            auto opened = EditorCommandMcpSession::Create({.inspection = endpoint},
                                                          {"immersive-fixture",
                                                           std::string{projectIdentity},
                                                           3,
                                                           {std::string{EditorSceneProposalGrant}}},
                                                          Mcp::Test::Authorization());
            REQUIRE(opened.HasValue());
            auto &session = *opened.Value();
            const std::array tools{std::string{"scene.propose_edits"}};
            const std::array ungranted{std::string{"inspect.transforms"}};
            CancellationSource toolApproval;
            CHECK_FALSE(owner.CaptureMcpCatalog(*context, session, "other-project", tools, toolApproval.Token()));
            CHECK_FALSE(owner.CaptureMcpCatalog(*context, session, projectIdentity, ungranted, toolApproval.Token()));
            CHECK_FALSE(owner.CaptureMcpCatalog(*context, session, projectIdentity, tools, {}));
            const auto catalog = owner.CaptureMcpCatalog(*context, session, projectIdentity, tools, toolApproval.Token());
            REQUIRE(catalog);
            CHECK_FALSE(owner.CaptureMcpPlan(*context, Prompt(), *catalog, "other-project"));
            auto pending = owner.CaptureMcpPlan(*context, Prompt(), *catalog);
            REQUIRE(pending);
            PlanningModel model;
            CHECK(pending->Plan(model, 1, {}).Status() == ContextDenied);
            CHECK(model.calls == 0);

            const Platform::ProjectReadContext project{std::string{projectIdentity},
                                                       identity.projectOpening,
                                                       {},
                                                       std::chrono::steady_clock::now() + std::chrono::seconds{10},
                                                       {}};
            Agent::AgentContextReaders readers;
            readers.isCurrent = [](const auto &) {
                return true;
            };
            readers.redact = [](const auto &, std::string_view value) {
                return Result<std::string>::Success(std::string{value});
            };
            const std::array<Agent::AgentContextSelection, 1> attachment{
                Agent::AgentContextAttachment{Agent::AgentContextAttachmentKind::Selection, "selection", "r1", "object 42"}};
            const auto preview =
                Agent::AgentContextPreview::Capture(attachment, readers, project, Security::AgentProviderResidence::Local, 1, 1);
            REQUIRE(preview.HasValue());
            Security::AgentPolicy policy;
            REQUIRE(policy.SetProjectTrust(true, 1));
            REQUIRE(preview.Value().Grant(policy).Allowed());
            const Agent::AgentTurnContext approved{preview.Value(), policy, project, readers.isCurrent,
                                                   Security::AgentProviderResidence::Local};
            model.tools = {
                {"scene-edit", tools.front(), R"({"scene":{"epoch":1,"revision":1},"renames":[{"object":42,"name":"Reviewed"}]})"}};
            const auto review = pending->Plan(model, 1, {}, {}, {}, &approved);
            REQUIRE(review.Status() == Completed);
            REQUIRE(model.advertised.size() == 1);
            CHECK(model.advertised.front().name == tools.front());
            const auto inspect = [](const Agent::AgentPlanObjectScope &scope) {
                return scope.opening == 1 && scope.revision == 1 && scope.objects == std::vector<std::uint64_t>{42};
            };
            CHECK(review.Read(*context, inspect));
            CHECK(staged == 0);
            targetsAvailable = false;
            CHECK(review.StageScenePreview(*context).HasError());
            CHECK(staged == 0);
            targetsAvailable = true;
            const auto stagedReview = review.StageScenePreview(*context);
            REQUIRE(stagedReview.HasValue());
            CHECK(stagedReview.Value() == 1);
            CHECK(staged == 1);
            REQUIRE(viewportAuthority);
            CHECK(viewportAuthority());

            SECTION("closing the command session withdraws the retained plan") {
                REQUIRE(session.Close().HasValue());
                CHECK_FALSE(owner.CaptureMcpCatalog(*context, session, projectIdentity, tools, toolApproval.Token()));
            }
            SECTION("withdrawing plan consent retires the draft while the command session stays live") {
                toolApproval.RequestCancellation();
                CHECK(session.SessionKey().principal != 0);
            }
            SECTION("switching to Simulate withdraws the viewport review") {
                auto simulate = identity;
                simulate.mode = ImmersiveAuthoringMode::Simulate;
                simulate.worldGeneration = 1;
                const auto switched = owner.Publish(simulate, {});
                REQUIRE(switched);
                CHECK(review.StageScenePreview(*switched).HasError());
            }
            CHECK(review.Status() == ContextDenied);
            CHECK_FALSE(review.Read(*context, inspect));
            CHECK_FALSE(viewportAuthority());
            CHECK(review.StageScenePreview(*context).HasError());
            CHECK(staged == 1);
            CHECK_FALSE(owner.CaptureMcpPlan(*context, Prompt(), *catalog));
        }

        TEST_CASE("Immersive plan schemas and review share one revocable MCP catalog", "[agent][immersive][turn][plan]") {
            using enum Agent::AgentTurnStatus;
            const std::vector<std::string> capabilities{"editor.scene.inspect"};
            Agent::TestSupport::Tools tools{Mcp::McpToolEffect::Query, capabilities};
            const std::array approved{std::string{"scene.inspect"}};
            CancellationSource approval;
            ImmersiveAuthoringContext owner;
            const auto context = owner.Publish(Opening(), {});
            REQUIRE(context);
            CHECK_FALSE(owner.CaptureMcpCatalog(*context, tools.registry, capabilities, approved, {}));
            CHECK_FALSE(owner.CaptureMcpCatalog(*context, tools.registry, {}, approved, approval.Token()));
            const std::array unregistered{std::string{"scene.unregistered"}};
            CHECK_FALSE(owner.CaptureMcpCatalog(*context, tools.registry, capabilities, unregistered, approval.Token()));
            const auto catalog = owner.CaptureMcpCatalog(*context, tools.registry, capabilities, approved, approval.Token());
            REQUIRE(catalog);
            ImmersiveAuthoringContext anotherOwner;
            const auto foreign = anotherOwner.Publish(Opening(), {});
            REQUIRE(foreign);
            CHECK_FALSE(anotherOwner.CaptureMcpPlan(*foreign, Prompt(), *catalog));
            CHECK_FALSE(owner.CaptureMcpCatalog(*foreign, tools.registry, capabilities, approved, approval.Token()));
            auto request = owner.CaptureMcpPlan(*context, Prompt(), *catalog);
            auto pending = owner.CaptureMcpPlan(*context, Prompt(), *catalog);
            REQUIRE(request);
            REQUIRE(pending);
            PlanningModel model;
            model.tools = {{"inspect", approved.front(), "{}"}};
            const auto review = request->Plan(model, 1, {});
            REQUIRE(review.Read(*context));
            REQUIRE(model.advertised.size() == 1);
            CHECK(model.advertised.front().name == approved.front());
            CHECK(model.advertised.front().parametersJson == R"({"additionalProperties":false,"type":"object"})");
            SECTION("replacing the registry withdraws both requests") {
                REQUIRE(tools.registry->Publish({}).HasValue());
            }
            SECTION("withdrawing only the tool approval also retires both requests") {
                const auto original = tools.registry->Read();
                approval.RequestCancellation();
                CHECK(tools.registry->Read() == original);
                CHECK_FALSE(owner.CaptureMcpCatalog(*context, tools.registry, capabilities, approved, approval.Token()));
                CHECK_FALSE(owner.CaptureMcpPlan(*context, Prompt(), *catalog));
            }
            SECTION("the same registry cannot carry an old catalog into another authoring generation") {
                auto changed = Opening();
                SECTION("project is replaced") { ++changed.projectOpening; }
                SECTION("scene document is edited") { ++changed.documentRevision; }
                SECTION("editor enters Simulate") {
                    changed.mode = ImmersiveAuthoringMode::Simulate;
                    changed.worldGeneration = 1;
                }
                const auto original = tools.registry->Read();
                const auto current = owner.Publish(changed, {});
                REQUIRE(current);
                CHECK(tools.registry->Read() == original);
                CHECK_FALSE(owner.CaptureMcpPlan(*current, Prompt(), *catalog));
                const auto admitted = owner.CaptureMcpCatalog(*current, tools.registry, capabilities, approved, approval.Token());
                REQUIRE(admitted);
                CHECK(owner.CaptureMcpPlan(*current, Prompt(), *admitted));
            }
            CHECK_FALSE(review.Read(*context));
            CHECK(pending->Plan(model, 2, {}).Status() == ContextDenied);
            CHECK(model.calls == 1);
            CHECK(tools.scene->calls == 0);
        }

        TEST_CASE("Immersive planning stops a quiet provider when its authoring authority is withdrawn", "[agent][immersive][turn][plan]") {
            using enum Agent::AgentTurnStatus;
            const std::vector<std::string> capabilities{"editor.scene.inspect"};
            Agent::TestSupport::Tools tools{Mcp::McpToolEffect::Query, capabilities};
            const std::array approved{std::string{"scene.inspect"}};
            CancellationSource approval;
            ImmersiveAuthoringContext owner;
            CancellationSource document;
            const auto context = owner.Publish(Opening(), {}, document.Token());
            REQUIRE(context);
            const auto catalog = owner.CaptureMcpCatalog(*context, tools.registry, capabilities, approved, approval.Token());
            REQUIRE(catalog);
            auto request = owner.CaptureMcpPlan(*context, Prompt(), *catalog);
            REQUIRE(request);
            PlanningModel model;
            model.tools = {{"inspect", approved.front(), "{}"}};
            SECTION("editor XR generation retires") {
                model.duringInference = [&] { owner.Revoke(); };
            }
            SECTION("document lifetime is withdrawn") {
                model.duringInference = [&] { document.RequestCancellation(); };
            }
            SECTION("MCP registry is replaced") {
                model.duringInference = [&] { REQUIRE(tools.registry->Publish({}).HasValue()); };
            }
            SECTION("MCP tool approval is withdrawn without replacing the registry") {
                model.duringInference = [&] { approval.RequestCancellation(); };
            }
            bool observedStop{};
            model.quietInference = [&](const std::stop_token stop) {
                std::mutex mutex;
                std::condition_variable_any wake;
                std::unique_lock lock{mutex};
                wake.wait_for(lock, stop, std::chrono::seconds{2}, [] { return false; });
                observedStop = stop.stop_requested();
            };
            const auto review = request->Plan(model, 3, Agent::AgentTurnLimits{.timeout = std::chrono::seconds{5}});
            CHECK(observedStop);
            CHECK(review.Status() == ContextDenied);
            CHECK_FALSE(review.Read(*context));
            CHECK(model.calls == 1);
            CHECK(tools.scene->calls == 0);
        }

        TEST_CASE("Immersive text inference stops a quiet provider when its context is retired", "[agent][immersive][turn]") {
            ImmersiveAuthoringContext owner;
            CancellationSource document;
            auto identity = Opening();
            const auto context = owner.Publish(identity, {}, document.Token());
            REQUIRE(context);
            auto request = owner.CaptureRequest(*context, ImmersiveRequestKind::Diagnostics, Prompt());
            REQUIRE(request);
            PlanningModel model;
            SECTION("The document lifetime ends") {
                model.duringInference = [&] { document.RequestCancellation(); };
            }
            SECTION("The project is replaced") {
                model.duringInference = [&] {
                    ++identity.projectOpening;
                    REQUIRE(owner.Publish(identity, {}));
                };
            }
            bool observedStop{};
            model.quietInference = [&](const std::stop_token stop) {
                std::mutex mutex;
                std::condition_variable_any wake;
                std::unique_lock lock{mutex};
                wake.wait_for(lock, stop, std::chrono::seconds{2}, [] { return false; });
                observedStop = stop.stop_requested();
            };
            const auto result = request->Run(model, 1);
            CHECK(observedStop);
            CHECK(model.calls == 1);
            CHECK(result.status == Agent::AgentTurnStatus::ContextDenied);
            CHECK(result.messages.empty());
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
