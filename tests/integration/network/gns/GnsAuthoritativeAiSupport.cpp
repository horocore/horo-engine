#include "GnsAuthoritativeAiSupport.h"

#include "../../../support/DecisionGraphCompilerTestSupport.h"
#include "../../../support/assets/AssetCookOutputFixture.h"
#include "CommittedSceneAssets.h"
#include "HeadlessCookedDecisionAdmission.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "NetworkProductHost.h"

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
    };

    /** @copydoc AuthoritativeAi::AuthoritativeAi */
    AuthoritativeAi::AuthoritativeAi(const AssetCookTargetId &target) : state_(std::make_unique<State>()) {
        auto &state = *state_;
        auto source = Decisions::TaskTree(GraphId);
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
    void AuthoritativeAi::Verify(const App::NetworkProductHost &host, NetworkModeWorld world, bool authority) const {
        const auto health = host.AiHealth(world);
        if (authority) {
            REQUIRE(health);
            CHECK(health->decisionTick > 0);
            CHECK_FALSE(health->degraded);
            CHECK(state_->fixture.probe.starts > 0);
        } else {
            CHECK_FALSE(health);
            CHECK(state_->fixture.probe.factories == 0);
            CHECK(state_->fixture.probe.starts == 0);
        }
    }
}  // namespace Horo::Network::NativeQualification
