#include "HeadlessNetworkServices.h"
#include "Horo/AI/AiReplayEvidence.h"
#include "Horo/Network/NetworkErrors.h"
#include "NetworkProductHost.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <utility>

namespace Horo::Application::PrefabAtomicTest {
    namespace {
        /** @brief Rejects a real runtime tick after its application owner has prepared the detached replay. */
        class RejectPreparedReplay final : public Runtime::RuntimeLifecycleParticipant {
        public:
            explicit RejectPreparedReplay(std::unique_ptr<Runtime::RuntimeLifecycleParticipant> next) : next_(std::move(next)) {}

            Result<void> Startup(const CancellationToken &token) override {
                return next_->Startup(token);
            }

            Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &frame) override {
                return next_->OnPhase(phase, frame);
            }

            Result<void> OnFixedUpdate(const Runtime::FixedStepContext &fixed) override {
                return next_->OnFixedUpdate(fixed);
            }

            Result<void> PrepareFixedCommit(const Runtime::FixedStepContext &fixed) override {
                auto prepared = next_->PrepareFixedCommit(fixed);
                return prepared.HasError() ? std::move(prepared)
                                           : Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                                                             "Reject prepared replay test tick."));
            }

            void AbortFixedUpdate(const Runtime::FixedStepContext &fixed) noexcept override {
                next_->AbortFixedUpdate(fixed);
            }

            void Shutdown() noexcept override {
                next_->Shutdown();
            }

        private:
            std::unique_ptr<Runtime::RuntimeLifecycleParticipant> next_;
        };
    }  // namespace

    /** @copydoc ExpectDecisionReplay */
    void ExpectDecisionReplay(std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition,
                              Internal::HeadlessSceneActivationInput input, AI::AgentId agent, const Sha256Digest &planDigest, bool abort) {
        using namespace Network;
        const auto replay = std::make_shared<Internal::HeadlessDecisionReplay>(Internal::HeadlessDecisionReplaySelection{agent, 1});
        input.decisionReplay = replay;
        auto configured = Internal::ComposeHeadlessSceneNetworkServices(std::move(definition), std::move(input));
        REQUIRE(configured.HasValue());
        auto composition = std::move(configured).Value();
        const auto defaults = DefaultNetworkProjectSettings(NetworkProjectSettingsId::Create(10).Value());
        REQUIRE(defaults.HasValue());
        const auto project = NetworkProjectSettings::Create(defaults.Value());
        REQUIRE(project.HasValue());
        constexpr auto roles = NetworkProjectRoleSet::Standalone;
        constexpr auto platform = NetworkTargetPlatform::Linux;
        const auto protocol = project.Value().Protocol();
        const auto build = NetworkProductBuildId::Create(1).Value();
        const auto revision = NetworkProductCapabilityRevision::Create(1).Value();
        const auto hostRevision = NetworkHostCapabilityRevision::Create(1).Value();
        const NetworkProductCapabilityManifest product{.build = build,
                                                       .revision = revision,
                                                       .platform = platform,
                                                       .supportedRoles = roles,
                                                       .includesNetworkRuntime = true,
                                                       .profile = project.Value().Profile().id,
                                                       .profileRevision = project.Value().Profile().revision,
                                                       .protocol = protocol};
        const NetworkTargetPackageInventory inventory{build, platform, roles, true, protocol};
        const NetworkTargetHostFacts hostFacts{hostRevision, platform, roles, true, protocol};
        const NetworkTargetRequirements requirements{roles};
        const NetworkTargetSelection selection{.role = NetworkProjectRole::Standalone,
                                               .expectedBuild = build,
                                               .expectedProductRevision = revision,
                                               .expectedHostRevision = hostRevision,
                                               .expectedProjectRevision = project.Value().Revision()};
        const std::array worlds{NetworkModeWorld{NetworkModeWorldKind::Standalone, {7}}};
        if (abort) {
            composition.fixedParticipant = [next = std::move(composition.fixedParticipant)](
                                               auto source) -> Result<std::unique_ptr<Runtime::RuntimeLifecycleParticipant>> {
                auto fixed = next(std::move(source));
                if (fixed.HasError())
                    return fixed;
                return Result<std::unique_ptr<Runtime::RuntimeLifecycleParticipant>>::Success(
                    std::make_unique<RejectPreparedReplay>(std::move(fixed).Value()));
            };
        }
        DeterministicClock clock;
        auto host = Internal::NetworkProductHost::Create(clock,
                                                         {.project = project.Value(),
                                                          .product = product,
                                                          .inventory = inventory,
                                                          .hostFacts = hostFacts,
                                                          .requirements = requirements,
                                                          .selection = selection,
                                                          .hostGeneration = 6,
                                                          .worlds = worlds,
                                                          .fixedParticipant = std::move(composition.fixedParticipant),
                                                          .pollRetirement = std::move(composition.pollRetirement)},
                                                         std::move(composition.factories));
        REQUIRE(host.HasValue());
        REQUIRE(host.Value()->Startup().HasValue());
        REQUIRE(host.Value()->RunFrame().HasValue());
        CHECK_FALSE(replay->TakeCommitted());
        clock.Advance(Duration::FromNanoseconds(16'666'667));
        const auto fixed = host.Value()->RunFrame();
        if (abort) {
            REQUIRE(fixed.HasError());
            CHECK(fixed.ErrorValue().message == "Reject prepared replay test tick.");
            CHECK_FALSE(replay->TakeCommitted());
            return;
        }
        INFO((fixed.HasError() ? fixed.ErrorValue().message : "success"));
        REQUIRE(fixed.HasValue());
        auto captured = replay->TakeCommitted();
        REQUIRE(captured);
        INFO((captured->HasError() ? captured->ErrorValue().message : "success"));
        REQUIRE(captured->HasValue());
        const auto &evidence = captured->Value();
        CHECK(evidence.tick == 1);
        CHECK(evidence.subject == agent);
        REQUIRE(evidence.trace);
        CHECK(evidence.trace->Header().planDigest == planDigest);
        CHECK(AI::HashAiReplayCanonicalState(evidence.canonical, Internal::HeadlessDecisionReplay::CanonicalBudget).HasValue());
        CHECK_FALSE(replay->TakeCommitted());
        host.Value()->Shutdown();
    }
}  // namespace Horo::Application::PrefabAtomicTest
