#include "GnsAuthoritativeAiSupport.h"

#include "../../../support/DecisionGraphCompilerTestSupport.h"
#include "../../../support/assets/AssetCookOutputFixture.h"
#include "CommittedSceneAssets.h"
#include "HeadlessCookedDecisionAdmission.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "NetworkProductHost.h"

#include <limits>

namespace Horo::Network::NativeQualification {
    namespace {
        namespace App = Application::Internal;
        namespace Decisions = AI::DecisionGraphTestSupport;
        namespace Output = Assets::OutputTestSupport;

        constexpr auto DecisionPath = "assets/authority.horo_bt";
        constexpr auto GraphId = 81U;
        constexpr auto ControllerId = 82U;
        constexpr auto AgentId = 83U;
    }  // namespace

    struct AuthoritativeAi::State final {
        Decisions::Fixture fixture;
        const Assets::AssetId asset = Assets::AssetId::FromBytes({84});
        const std::array<AI::AiControllerDescriptor, 1> catalog{
            AI::AiControllerDescriptor{.controller = Decisions::MakeIdentity<AI::ControllerTypeId>(ControllerId),
                                       .decisionAsset = Decisions::Graph(GraphId),
                                       .decisionKind = AI::DecisionPlanKind::BehaviorTree,
                                       .blackboardSchema = fixture.schema}};
        Output::TempDir output;
        std::unique_ptr<App::CommittedSceneAssets> committed;
        AI::AiTaskContinuation pendingAtShutdown;
        std::size_t cleanupsBeforeShutdown{};
        std::size_t abortsBeforeShutdown{};
    };

    /** @copydoc AuthoritativeAi::AuthoritativeAi */
    AuthoritativeAi::AuthoritativeAi(const AssetCookTargetId &target) : state_(std::make_unique<State>()) {
        auto &state = *state_;
        using enum AI::DecisionControlKind;
        const auto pending = Decisions::TreeNode(4, Task);
        auto source =
            Decisions::Tree(GraphId,
                            {Decisions::TreeNode(1, Root), Decisions::TreeNode(2, Parallel), Decisions::TreeNode(3, Task), pending},
                            {Decisions::Edge(1, 1, 2), Decisions::Edge(2, 2, 3), Decisions::Edge(3, 2, 4, 1)});
        auto &probe = state.fixture.probe;
        probe.onStart = [&probe, pendingTask = pending.task] {
            probe.succeedTick =
                probe.continuation.Operation().taskDefinition == pendingTask ? std::numeric_limits<std::uint64_t>::max() : 0;
        };
        source.executionTask = Decisions::MakeIdentity<AI::TaskId>(9000);
        const auto plan = state.fixture.Compile(std::span{&source, 1}, GraphId);
        auto bytes = AI::DecisionPlanCodec::EncodeCooked(*plan);
        REQUIRE(bytes.HasValue());
        const auto type = Output::Type(AI::DecisionAssetType(AI::DecisionPlanKind::BehaviorTree));
        const auto content = ComputeSha256(std::as_bytes(std::span{bytes.Value()}));
        auto envelope = Assets::EncodeCookedArtifact({state.asset, type, target, {}, {}, content, std::move(bytes).Value()});
        REQUIRE(envelope.HasValue());
        const auto digest = ComputeSha256(std::as_bytes(std::span{envelope.Value()}));
        const std::array entries{Assets::AssetCookManifestEntry{state.asset, type, state.asset.ToString() + ".cooked", digest}};
        const std::array payloads{std::move(envelope).Value()};
        REQUIRE(Output::PublishFixture(state.output.path, target, entries, payloads).HasValue());

        Assets::AssetRegistry registry;
        REQUIRE(registry
                    .Publish({{state.asset, type, ProjectPath::Parse(DecisionPath).Value(),
                               ProjectPath::Parse(std::string{DecisionPath} + ".horo").Value()}})
                    .status == Assets::AssetRegistryBuildStatus::Complete);
        auto committed = App::CommittedSceneAssets::Capture(NativeDurableFileSystem{}, {state.output.path, target}, registry.Snapshot());
        REQUIRE(committed.HasValue());
        state.committed = std::move(committed).Value();
    }

    AuthoritativeAi::~AuthoritativeAi() = default;

    /** @copydoc AuthoritativeAi::Attach */
    void AuthoritativeAi::Attach(Runtime::RuntimeEntityDefinition &entity) const {
        const auto &controller = state_->catalog.front();
        entity.components.aiAgent = AI::AiAgentComponent{.agent = Decisions::MakeIdentity<AI::AgentId>(AgentId)};
        entity.components.aiController = AI::AiControllerComponent{.controller = controller.controller,
                                                                   .decisionAsset = controller.decisionAsset,
                                                                   .blackboardSchema = controller.blackboardSchema->Identity()};
    }

    /** @copydoc AuthoritativeAi::Configure */
    void AuthoritativeAi::Configure(App::HeadlessSceneActivationInput &input, JobSystem &jobs) const {
        input.aiControllers = state_->catalog;
        input.cookedDecisions = std::make_shared<const App::HeadlessCookedDecisionInput>(
            App::HeadlessCookedDecisionInput{.committed = *state_->committed,
                                             .environment = state_->fixture.Environment(),
                                             .bindings = {{state_->catalog.front().controller, state_->asset}},
                                             .execution = AI::AiSceneCookedDecisionExecution{jobs, {}, {}}});
    }

    /** @copydoc AuthoritativeAi::Verify */
    void AuthoritativeAi::Verify(const App::NetworkProductHost &host, const INetworkModeService &scene, NetworkModeWorld world,
                                 bool authority) const {
        const auto health = host.AiHealth(world);
        const auto agent = Decisions::MakeIdentity<AI::AgentId>(AgentId);
        const auto key = state_->fixture.schema->Keys().front().key;
        const auto board = App::InspectHeadlessDecisionValue(scene, agent, key);
        const auto &probe = state_->fixture.probe;
        if (authority) {
            REQUIRE(health);
            CHECK(health->decisionTick > 0);
            CHECK_FALSE(health->degraded);
            CHECK(probe.starts == 2);
            CHECK(probe.cleanups == 1);
            CHECK(probe.lastTerminal == AI::AiTaskState::Succeeded);
            REQUIRE(board.HasValue());
            CHECK(board.Value() == state_->fixture.schema->Keys().front().defaultValue);
        } else {
            CHECK_FALSE(health);
            CHECK(probe.factories == 0);
            CHECK(probe.starts == 0);
            CHECK(probe.cleanups == 0);
            REQUIRE(board.HasError());
            CHECK(board.ErrorValue().code.Value() == AI::AIErrors::RuntimeUnavailable.code.Value());
        }
    }

    /** @copydoc AuthoritativeAi::CapturePendingAtShutdown */
    void AuthoritativeAi::CapturePendingAtShutdown() {
        const auto &probe = state_->fixture.probe;
        REQUIRE(probe.continuation.IsCurrent());
        state_->pendingAtShutdown = probe.continuation;
        state_->cleanupsBeforeShutdown = probe.cleanups;
        state_->abortsBeforeShutdown = probe.aborts;
    }

    /** @copydoc AuthoritativeAi::VerifyRetirement */
    void AuthoritativeAi::VerifyRetirement(bool authority) const {
        const auto &probe = state_->fixture.probe;
        if (!authority) {
            CHECK(probe.starts == 0);
            CHECK(probe.aborts == 0);
            CHECK(probe.cleanups == 0);
            return;
        }
        CHECK(probe.aborts == state_->abortsBeforeShutdown + 1);
        CHECK(probe.cleanups == state_->cleanupsBeforeShutdown + 1);
        CHECK(probe.lastTerminal == AI::AiTaskState::Cancelled);
        CHECK_FALSE(state_->pendingAtShutdown.IsCurrent());
        AI::AiTaskTerminalResult late{.state = AI::AiTaskState::Succeeded};
        CHECK(state_->pendingAtShutdown.PublishTerminal(late).Value() == AI::AiTaskPublicationDisposition::Stale);
    }
}  // namespace Horo::Network::NativeQualification
