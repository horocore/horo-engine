#include "NpcDialogueHttpsProvider.h"
#include "NpcDialogueProjectActivation.h"
#include "NpcDialogueScenePresentation.h"

#include <array>
#include <chrono>
#include <future>
#include <string_view>
#include <thread>

using namespace Horo;
using namespace Horo::AI;
using namespace std::chrono_literals;

namespace {
    using Runtime::Ui::CaptionTimeline;

    /** @brief Waits only within the test budget for owner-lane caption publication. */
    bool PublishCaption(App::NpcDialogueScenePresentation &scene, const NpcDialogueGrant &grant,
                        std::chrono::steady_clock::time_point stop) {
        while (std::chrono::steady_clock::now() < stop) {
            if (const auto admission = scene.Pump(grant, 2))
                return *admission == Accessibility::CaptionAdmission::Accepted;
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    /** @brief Supplies the same semantic queue contract consumed by the HUD. */
    auto MakeQueue() {
        using namespace Runtime::Ui;
        const auto fallbackKey = UiMessageKey::Create("game.dialogue", "offline").Value();
        const auto approvedKey = UiMessageKey::Create("game.dialogue", "approved").Value();
        auto fallback = LocalizedMessageRef::Create(fallbackKey, {}, "Come back later.");
        auto approved = LocalizedMessageRef::Create(approvedKey, {}, "Welcome, traveler.");
        return CaptionCueQueue::Create(9, {std::move(fallback).Value(), std::move(approved).Value()});
    }

    /** @brief Checks the one authored caption on the UI-side timeline. */
    bool CheckTimeline(App::NpcDialogueScenePresentation &scene, bool retired, std::uint32_t text) {
        CaptionTimeline timeline;
        if (timeline.Reset(9).HasError())
            return false;
        timeline.BeginFrame(2);
        static_cast<void>(scene.Drain(timeline));
        const auto cues = timeline.Cues();
        if (retired)
            return cues.empty();
        return cues.size() == 1 && cues.front().cue.text == text && cues.front().cue.id == Accessibility::CaptionCueId{9, 1} &&
               cues.front().cue.startTick == 2 && cues.front().cue.durationTicks == 20;
    }

    /** @brief Holds the only I/O worker while the test retires a queued Scene request. */
    Result<JobHandle> HoldIo(JobSystem &jobs, std::promise<void> &release) {
        return jobs.Submit({.resource = JobResource::Io}, [ready = release.get_future().share()](const CancellationToken &) {
            static_cast<void>(ready.wait_for(2s));
        });
    }

    /** @brief Publishes an authored NPC with the runtime's normal Behavior Tree admission path. */
    std::unique_ptr<AiSceneActivationCandidate> PublishAgent(AiSceneRuntime &runtime) {
        const std::array keys{BlackboardKeyDescriptor{.key = BlackboardKeyId::Create(1).Value(),
                                                       .kind = BlackboardValueKind::Boolean,
                                                       .cardinality = BlackboardValueCardinality::Scalar,
                                                       .maximumCollectionElements = 1,
                                                       .presence = BlackboardKeyPresence::Required,
                                                       .access = BlackboardKeyAccess::ReadWrite,
                                                       .defaultValue = BlackboardScalarValue{false}}};
        auto schema = BlackboardSchema::Capture({.identity = BlackboardSchemaId::Create(1).Value(), .keys = keys});
        if (schema.HasError())
            return {};
        auto blackboard = std::make_shared<const BlackboardSchema>(std::move(schema).Value());
        const SourceLocation location{"assets/ai/npc.horo", 1, 1};
        const DecisionNodeDescriptor node{.type = DecisionNodeTypeId::Create(1).Value(),
                                          .origin = {DecisionDescriptorSourceKind::Native, DecisionProviderId::Create(1).Value(), 1},
                                          .source = location};
        const DecisionAssetDescriptor asset{.asset = DecisionGraphAssetId::Create(1).Value(),
                                            .kind = DecisionPlanKind::BehaviorTree,
                                            .schemaVersion = CurrentDecisionAssetSchemaVersion,
                                            .blackboardSchema = blackboard->Identity(),
                                            .requiredBlackboardSchemaVersion = {1, 1},
                                            .nodes = {{.id = DecisionNodeId::Create(1).Value(), .type = node.type,
                                                       .descriptorVersion = {1, 1}, .source = location}},
                                            .source = location};
        auto compiled = DecisionAssetCompiler::Compile(asset, {}, std::array{node}, std::array{blackboard});
        if (compiled.HasError() || !compiled.Value().IsValid())
            return {};
        const auto controller = ControllerTypeId::Create(1).Value();
        const AiControllerDescriptor descriptor{.controller = controller, .decisionAsset = asset.asset, .decisionKind = asset.kind,
                                                .blackboardSchema = blackboard, .decisionPlan = compiled.Value().plan};
        const AiSceneActivationBinding scene{AiRuntimeIncarnation::Create(8).Value(), Runtime::SceneRuntimeId{8}};
        const AiSceneAgentDescriptor agent{.owner = {scene.scene, Runtime::EntityId{.index = 0, .generation = 1}},
                                           .agent = {.agent = AgentId::Create(1).Value()},
                                           .controller = AiControllerComponent{.controller = controller, .decisionAsset = asset.asset,
                                                                              .blackboardSchema = blackboard->Identity()}};
        auto prepared = runtime.PrepareScene(scene, std::array{agent}, std::array{descriptor});
        if (prepared.HasError() || prepared.Value()->ValidatePublication().HasError())
            return {};
        auto publication = std::move(prepared).Value();
        publication->Publish();
        return publication;
    }

    /** @brief Cancels a queued remote turn before the I/O worker can send any request. */
    int RunQueuedRetirement(App::NpcDialogueScenePresentation &scene, JobSystem &jobs, AiSceneRuntime &runtime, AgentHandle owner,
                            const NpcDialogueGrant &grant, std::span<const NpcDialogueFact> facts, bool disableAgent) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        std::promise<void> release;
        if (HoldIo(jobs, release).HasError())
            return 11;
        if (scene.Start(owner, grant.route, grant, facts, 1, 10, deadline, {}).HasError())
            return 7;
        if (disableAgent) {
            if (runtime.DisableAtSafePoint(owner).HasError() || scene.Pump(grant, 2) ||
                scene.Start(owner, grant.route, grant, facts, 2, 10, deadline, {}).HasValue())
                return 14;
        } else {
            scene.Retire();
        }
        release.set_value();
        jobs.Shutdown(ShutdownPolicy::Drain);
        return CheckTimeline(scene, true, 0) ? 0 : 10;
    }

    /** @brief Drives a live NPC turn to HUD publication, then optionally retires its Scene. */
    int RunTurn(App::NpcDialogueScenePresentation &scene, AiSceneRuntime &runtime, AgentHandle owner, const NpcDialogueGrant &grant,
                std::span<const NpcDialogueFact> facts, std::string_view mode) {
        const auto deadline = std::chrono::steady_clock::now() + (mode == "timeout" ? 500ms : 5s);
        if (scene.Start(owner, grant.route, grant, facts, 1, 10, deadline, {}).HasError())
            return 7;
        if (!PublishCaption(scene, grant, deadline + 1s))
            return 8;
        if (mode == "retired")
            runtime.BeginShutdown();
        return CheckTimeline(scene, mode == "retired", mode == "approved" ? 1 : 0) ? 0 : 9;
    }

    /** @brief Composes one Scene channel and exercises a complete presentation turn. */
    int RunScene(const App::NpcDialogueProjectActivation &activation, const std::string_view mode) {
        auto created = AiSceneRuntime::Create();
        if (created.HasError())
            return 15;
        auto runtime = std::move(created).Value();
        const auto publication = PublishAgent(runtime);
        const auto snapshot = runtime.Snapshot();
        if (!publication || snapshot.HasError() || snapshot.Value().Agents().empty())
            return 16;
        const auto owner = snapshot.Value().Agents().front().handle;
        const auto grant = activation.Grant();
        const std::array facts{NpcDialogueFact{NpcDialogueFactId::Create(1).Value(), NpcDialogueValueId::Create(1234).Value()},
                               NpcDialogueFact{NpcDialogueFactId::Create(2).Value(), NpcDialogueValueId::Create(20).Value()},
                               NpcDialogueFact{NpcDialogueFactId::Create(3).Value(), NpcDialogueValueId::Create(5678).Value()}};
        const std::array cues{App::NpcDialogueCaption{NpcDialogueCueId::Create(4).Value(), 0, 0, 20},
                              App::NpcDialogueCaption{NpcDialogueCueId::Create(5).Value(), 1, 0, 20}};
        auto queue = MakeQueue();
        if (queue.HasError())
            return 5;
        JobSystem jobs({.ioWorkerCount = 1});
        auto channel = App::NpcDialogueScenePresentation::Create(jobs, runtime, std::move(queue).Value(), cues,
                                                                  cues.front().cue, activation.Binding());
        if (channel.HasError())
            return 6;
        auto scene = std::move(channel).Value();
        if (mode == "queued-retire" || mode == "disabled-agent")
            return RunQueuedRetirement(scene, jobs, runtime, owner, grant, facts, mode == "disabled-agent");
        return RunTurn(scene, runtime, owner, grant, facts, mode);
    }

    /** @brief Captures the same canonical project bytes used to identify approval. */
    auto CaptureProject() {
        using namespace Application;
        const auto version = ParseHoroVersion("0.2.0").Value();
        const auto &decision = *BuiltInReleaseCompatibilityRegistry().Find({version});
        const auto contract = FormatPersistentContractHash(decision.persistentContract);
        const auto contents = R"({"horoVersion":"0.2.0","persistentContract":")" + contract + R"(","projectId":"npc-fixture",
            "name":"NPC fixture","projectVersion":"1.0.0","createdAt":"2026-10-04T00:00:00Z",
            "settings":{"renderBackend":"null","ai":{"schemaVersion":1,
            "npcDialogue":{"provider":1,"destination":2,"version":1,"fields":[3,2]}}}})";
        return DecodeProjectSourceDocument(contents);
    }

    /** @brief Admits the project/host intersection, or runs the same Scene offline on mismatch. */
    int RunProject(App::NpcDialogueHttpsConfig config, std::string_view mode) {
        auto project = CaptureProject();
        if (project.HasError() || !project.Value().sourceFingerprint)
            return 12;
        const std::array allowed{NpcDialogueFactId::Create(1).Value(), NpcDialogueFactId::Create(2).Value()};
        App::NpcDialogueEndpointApproval approval{*project.Value().sourceFingerprint, std::move(config), allowed};
        if (mode == "stale-grant")
            approval.projectSource.bytes[0] ^= 1;
        if (mode == "other-route")
            approval.endpoint.route.destination = NpcDialogueDestinationId::Create(3).Value();
        const bool offline = mode == "offline" || mode == "retired";
        const auto activation = App::NpcDialogueProjectActivation::Create(project.Value(), 1, offline ? nullptr : &approval);
        if (activation.HasError() || !activation.Value())
            return 13;
        return RunScene(*activation.Value(), mode);
    }
}  // namespace

int main(int argc, char **argv) {
    if (argc != 4)
        return 2;
    const NpcDialogueRoute route{DecisionProviderId::Create(1).Value(), NpcDialogueDestinationId::Create(2).Value(), 1};
    const std::string_view mode{argv[3]};
    if (mode == "invalid")
        return App::MakeNpcDialogueHttpsProvider({route, 1, argv[1], argv[2]}).HasError() ? 0 : 3;
    return RunProject({route, 1, argv[1], argv[2]}, mode);
}
