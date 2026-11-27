#include "NpcDialogueHttpsProvider.h"
#include "NpcDialogueProjectActivation.h"
#include "NpcDialogueScenePresentation.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string_view>
#include <thread>

using namespace Horo;
using namespace Horo::AI;
using namespace std::chrono_literals;

namespace {
    using Runtime::Ui::CaptionTimeline;
    using Application::NpcDialogueTriggerId;
    constexpr std::uint64_t AuthoredTrigger = 3;
    constexpr std::uint64_t OtherNpcTrigger = 4;
    constexpr std::uint64_t MissingTrigger = 5;
    constexpr std::uint64_t DisclosedField = 2;

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
    bool CheckTimeline(App::NpcDialogueScenePresentation &scene, const NpcDialogueGrant &grant, bool retired, std::uint32_t text) {
        CaptionTimeline timeline;
        if (timeline.Reset(9).HasError())
            return false;
        timeline.BeginFrame(2);
        static_cast<void>(scene.Drain(timeline, grant));
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

    /** @brief Releases the I/O worker after retirement and before the next presentation poll. */
    int RunQueuedRetirement(App::NpcDialogueScenePresentation &scene, JobSystem &jobs, AiSceneRuntime &runtime, AgentHandle owner,
                            const NpcDialogueGrant &grant, NpcDialogueTriggerId trigger, std::string_view mode,
                            const CancellationSource &approval) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        std::promise<void> release;
        if (HoldIo(jobs, release).HasError())
            return 11;
        if (scene.Start(owner, trigger, grant, 1, 10, deadline, {}).HasError())
            return 7;
        if (mode == "disabled-agent") {
            if (runtime.DisableAtSafePoint(owner).HasError())
                return 14;
        } else if (mode == "queued-scene-shutdown") {
            runtime.BeginShutdown();
        } else if (mode == "approval-while-queued") {
            approval.RequestCancellation();
        } else {
            scene.Retire();
        }
        release.set_value();
        jobs.Shutdown(ShutdownPolicy::Drain);
        if (mode == "approval-while-queued")
            return scene.Pump(grant, 2) == Accessibility::CaptionAdmission::Accepted && CheckTimeline(scene, grant, false, 0) ? 0 : 21;
        return !scene.Pump(grant, 2) && scene.Start(owner, trigger, grant, 2, 10, deadline, {}).HasError() &&
                       CheckTimeline(scene, grant, true, 0)
                   ? 0
                   : 10;
    }

    /** @brief Applies the selected owner change before or after the HUD takes the cue. */
    bool WithdrawTurn(AiSceneRuntime &runtime, AgentHandle owner, CancellationSource &turn, std::string_view mode) {
        if (mode == "disable-before-drain" || mode == "disable-after-drain")
            return runtime.DisableAtSafePoint(owner).HasValue();
        if (mode == "cancel-before-drain" || mode == "cancel-after-drain")
            turn.RequestCancellation();
        if (mode == "retired")
            runtime.BeginShutdown();
        return true;
    }

    /** @brief Withdraws visible dialogue without affecting another caption on the same HUD. */
    bool CheckVisibleWithdrawal(App::NpcDialogueScenePresentation &scene, AiSceneRuntime &runtime, AgentHandle owner,
                                CancellationSource &turn, NpcDialogueGrant grant, std::string_view mode,
                                const CancellationSource &approval) {
        CaptionTimeline timeline;
        if (timeline.Reset(9).HasError())
            return false;
        timeline.BeginFrame(2);
        static_cast<void>(scene.Drain(timeline, grant));
        if (timeline.Cues().size() != 1 || timeline.Cues().front().cue.kind != Accessibility::CaptionCueKind::Dialogue)
            return false;
        auto unrelated = std::move(MakeQueue()).Value();
        const Accessibility::CaptionCueId sound{9, 2};
        if (unrelated->TryEnqueue({sound, Accessibility::CaptionCueKind::Sound, 0, 0, 2, 20}) != Accessibility::CaptionAdmission::Accepted)
            return false;
        static_cast<void>(timeline.Drain(*unrelated));
        if (!WithdrawTurn(runtime, owner, turn, mode))
            return false;
        const auto disclosedField = NpcDialogueFactId::Create(DisclosedField).Value();
        const std::array duplicateFields{disclosedField, disclosedField};
        if (mode == "grant-after-drain")
            grant.fields = {};
        if (mode == "malformed-grant-after-drain")
            grant.fields = duplicateFields;
        if (mode == "approval-after-drain")
            approval.RequestCancellation();
        static_cast<void>(scene.Drain(timeline, grant));
        return timeline.Cues().size() == 1 && timeline.Cues().front().cue.id == sound;
    }

    /** @brief Drives a live NPC turn to HUD publication, then optionally withdraws the cue. */
    int RunTurn(App::NpcDialogueScenePresentation &scene, AiSceneRuntime &runtime, AgentHandle owner, const NpcDialogueGrant &grant,
                NpcDialogueTriggerId trigger, std::string_view mode, const CancellationSource &approval) {
        const auto deadline = std::chrono::steady_clock::now() + (mode == "timeout" ? 500ms : 5s);
        CancellationSource turn;
        if (scene.Start(owner, trigger, grant, 1, 10, deadline, turn.Token()).HasError())
            return 7;
        if (!PublishCaption(scene, grant, deadline + 1s))
            return 8;
        if (mode == "disable-after-drain" || mode == "cancel-after-drain" || mode == "grant-after-drain" ||
            mode == "malformed-grant-after-drain" || mode == "approval-after-drain")
            return CheckVisibleWithdrawal(scene, runtime, owner, turn, grant, mode, approval) ? 0 : 19;
        if (!WithdrawTurn(runtime, owner, turn, mode))
            return 14;
        auto liveGrant = grant;
        const std::array invalidFields{NpcDialogueFactId{}, NpcDialogueFactId::Create(DisclosedField).Value()};
        if (mode == "grant-before-drain")
            liveGrant.fields = {};
        if (mode == "malformed-grant-before-drain")
            liveGrant.fields = invalidFields;
        if (mode == "unused-grant-before-drain")
            liveGrant.fields = liveGrant.fields.first(1);
        if (mode == "offline-replaced-grant")
            ++liveGrant.providerGeneration;
        const bool withdrawn = mode == "retired" || mode == "disable-before-drain" || mode == "cancel-before-drain" ||
                               mode == "grant-before-drain" || mode == "malformed-grant-before-drain";
        const bool remoteCue = mode == "approved" || mode == "unused-grant-before-drain";
        return CheckTimeline(scene, liveGrant, withdrawn, remoteCue ? 1 : 0) ? 0 : 9;
    }

    /** @brief Verifies that an unknown or mismatched trigger cannot schedule or publish a turn. */
    int RunRejectedTrigger(App::NpcDialogueScenePresentation &scene, AgentHandle owner, const NpcDialogueGrant &grant, std::string_view mode) {
        const auto trigger = NpcDialogueTriggerId::Create(mode == "missing-trigger" ? MissingTrigger : OtherNpcTrigger).Value();
        const auto rejected = scene.Start(owner, trigger, grant, 1, 10, std::chrono::steady_clock::now() + 5s, {});
        return rejected.HasError() && !scene.Pump(grant, 2) && CheckTimeline(scene, grant, true, 0) ? 0 : 17;
    }

    /** @brief Runs the selected scenario using the same admitted project channel. */
    int RunScenario(App::NpcDialogueScenePresentation &scene, JobSystem &jobs, AiSceneRuntime &runtime, AgentHandle owner,
                    const NpcDialogueGrant &grant, std::string_view mode, const CancellationSource &approval) {
        if (mode == "different-agent" || mode == "missing-trigger")
            return RunRejectedTrigger(scene, owner, grant, mode);
        const auto trigger = NpcDialogueTriggerId::Create(AuthoredTrigger).Value();
        if (mode == "queued-retire" || mode == "disabled-agent" || mode == "queued-scene-shutdown" || mode == "approval-while-queued")
            return RunQueuedRetirement(scene, jobs, runtime, owner, grant, trigger, mode, approval);
        return RunTurn(scene, runtime, owner, grant, trigger, mode, approval);
    }

    /** @brief Composes one Scene channel and exercises a complete presentation turn. */
    int RunScene(const App::NpcDialogueProjectActivation &activation, const std::string_view mode, const CancellationSource &approval) {
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
        const std::array cues{App::NpcDialogueCaption{NpcDialogueCueId::Create(4).Value(), 0, 0, 20},
                              App::NpcDialogueCaption{NpcDialogueCueId::Create(5).Value(), 1, 0, 20}};
        auto queue = MakeQueue();
        if (queue.HasError())
            return 5;
        JobSystem jobs({.ioWorkerCount = 1});
        auto channel = App::NpcDialogueScenePresentation::Create(jobs, runtime, std::move(queue).Value(), cues,
                                                                  cues.front().cue, activation.Triggers(), activation.Binding());
        if (channel.HasError())
            return 6;
        auto scene = std::move(channel).Value();
        return RunScenario(scene, jobs, runtime, owner, grant, mode, approval);
    }

    /** @brief Builds identical authored project bytes for the approved root and its copy. */
    std::string ProjectContents() {
        using namespace Application;
        const auto version = ParseHoroVersion("0.2.0").Value();
        const auto &decision = *BuiltInReleaseCompatibilityRegistry().Find({version});
        const auto contract = FormatPersistentContractHash(decision.persistentContract);
        return R"({"horoVersion":"0.2.0","persistentContract":")" + contract + R"(","projectId":"npc-fixture",
            "name":"NPC fixture","projectVersion":"1.0.0","createdAt":"2026-10-04T00:00:00Z",
            "settings":{"renderBackend":"null","ai":{"schemaVersion":1,
            "npcDialogue":{"provider":1,"destination":2,"version":1,"fields":[3,2,9],"triggers":[
              {"id":4,"agent":2,"facts":[{"field":2,"value":20}]},
              {"id":3,"agent":1,"facts":[{"field":3,"value":5678},{"field":1,"value":1234},{"field":2,"value":20}]}
            ]}}}})";
    }

    /** @brief Creates the bounded project document in a directory owned by this TLS fixture. */
    bool WriteProject(const std::filesystem::path &root, const std::string &contents) {
        if (!std::filesystem::create_directory(root))
            return false;
        const auto marker = root / ".horo/project.json";
        std::filesystem::create_directories(marker.parent_path());
        std::ofstream output{marker, std::ios::binary};
        output << contents;
        output.close();
        return !output.fail();
    }

    /** @brief Writes two distinct roots with the same manifest and returns the approved source fingerprint. */
    std::optional<Sha256Digest> PrepareProjects(const std::filesystem::path &approved, const std::filesystem::path &copy) {
        const auto contents = ProjectContents();
        if (!WriteProject(approved, contents) || !WriteProject(copy, contents))
            return std::nullopt;
        const auto source = Application::LoadProjectSourceDocument(approved);
        return source.HasValue() ? source.Value().sourceFingerprint : std::nullopt;
    }

    /** @brief Admits the project/host intersection, or runs the same Scene offline on mismatch. */
    int RunProject(App::NpcDialogueHttpsConfig config, std::string_view mode) {
        const auto fixtureRoot = std::filesystem::current_path();
        const auto approvedRoot = fixtureRoot / "approved-project";
        const auto copiedRoot = fixtureRoot / "copied-project";
        const auto fingerprint = PrepareProjects(approvedRoot, copiedRoot);
        if (!fingerprint)
            return 12;
        CancellationSource lifetime;
        App::NpcDialogueEndpointApproval approval{approvedRoot,
                                                  *fingerprint,
                                                  std::move(config),
                                                  {NpcDialogueFactId::Create(1).Value(), NpcDialogueFactId::Create(DisclosedField).Value(),
                                                   NpcDialogueFactId::Create(9).Value()},
                                                  lifetime.Token()};
        if (mode == "stale-grant")
            approval.projectSource.bytes[0] ^= 1;
        if (mode == "other-route")
            approval.endpoint.route.destination = NpcDialogueDestinationId::Create(3).Value();
        if (mode == "invalid-endpoint")
            approval.endpoint.endpoint += "?unapproved=1";
        if (mode == "invalid-approval-fields")
            approval.fields.push_back(approval.fields.back());
        if (mode == "approval-before-activation")
            lifetime.RequestCancellation();
        const bool offline = mode == "offline" || mode == "retired" || mode == "offline-replaced-grant";
        const auto &projectRoot = mode == "copied-project" ? copiedRoot : approvedRoot;
        const auto activation = App::NpcDialogueProjectActivation::Create(projectRoot, 1, offline ? nullptr : &approval);
        if (activation.HasError() || !activation.Value())
            return 13;
        if ((mode == "invalid-endpoint" || mode == "invalid-approval-fields" || mode == "approval-before-activation") &&
            activation.Value()->Binding())
            return 20;
        return RunScene(*activation.Value(), mode, lifetime);
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
