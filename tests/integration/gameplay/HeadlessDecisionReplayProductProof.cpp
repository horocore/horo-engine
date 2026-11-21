#include "HeadlessNetworkServices.h"
#include "HeadlessReplayArtifact.h"
#include "Horo/AI/AICanonicalState.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Scene/CookedSceneDefinition.h"
#include "NetworkProductHost.h"
#include "OwnedTestDirectory.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
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

        /** @brief Checks the actual detached report and protects the previously published bytes on a second invocation. */
        void ExpectReplayArtifact(const Internal::HeadlessReplayOutput &output, const Internal::HeadlessDecisionReplayData &data) {
            const auto digest = ComputeSha256(std::as_bytes(std::span{data.canonicalRecord}));
            const auto published = Internal::PublishHeadlessReplayArtifact(output, data, digest);
            INFO((published.HasError() ? published.ErrorValue().message : "success"));
            REQUIRE(published.HasValue());
            const auto file = output.Directory() / "evidence.json";
            std::ifstream stream{file, std::ios::binary};
            REQUIRE(stream.is_open());
            const auto artifact = nlohmann::json::parse(stream);
            CHECK(artifact.at("format") == "horo-ai-replay-evidence");
            CHECK(artifact.at("version") == 1);
            CHECK(artifact.at("sceneDefinitionId") == std::to_string(data.definition.value));
            CHECK(artifact.at("tick") == "1");
            CHECK(artifact.at("agentId") == std::to_string(data.evidence.subject.Value()));
            CHECK(artifact.at("canonicalAi").at("digest") == FormatSha256(digest));
            CHECK(artifact.at("canonicalAi").at("recordVersion") == AI::AiCanonicalReferenceRecordVersion);
            const auto &coverage = artifact.at("manifest").at("boundaries");
            CHECK(coverage.at("scene-input") == "deterministic");
            CHECK(coverage.at("decision") == "deterministic");
            CHECK(coverage.at("canonical-ai") == "deterministic");
            CHECK(coverage.at("physics") == "omitted");
            CHECK(coverage.at("navigation") == "omitted");
            CHECK(coverage.at("achieved-character-motion") == "omitted");
            CHECK(artifact.at("sceneInput").at("revision") == std::to_string(data.definitionRevision.value));
            CHECK(artifact.at("sceneInput").at("digest") == FormatSha256(data.sceneInput));
            const auto &decision = artifact.at("decision");
            CHECK(decision.at("planDigest") == FormatSha256(data.evidence.trace->Header().planDigest));
            CHECK(decision.at("traceVersion") == AI::DecisionTraceHeader::CurrentVersion);
            REQUIRE(decision.at("records").size() == data.evidence.trace->Records().size());
            for (const auto &record : decision.at("records")) {
                CHECK(record.at("hasActiveTaskHandle").is_boolean());
                CHECK_FALSE(record.contains("taskHandle"));
            }
            CHECK_FALSE(decision.at("blackboard").contains("values"));
            CHECK(Internal::PublishHeadlessReplayArtifact(output, data, digest).HasError());
            std::ifstream retained{file, std::ios::binary};
            CHECK(nlohmann::json::parse(retained) == artifact);
        }
    }  // namespace

    /** @copydoc ExpectDecisionReplay */
    void ExpectDecisionReplay(std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition,
                              Internal::HeadlessSceneActivationInput input, AI::AgentId agent, const Sha256Digest &planDigest, bool abort,
                              std::optional<std::uint32_t> referenceGeneration) {
        using namespace Network;
        const auto definitionId = definition->Id();
        const auto definitionRevision = definition->Revision();
        const auto encodedScene = SceneCook::EncodeCookedSceneDefinition(*definition);
        REQUIRE(encodedScene.HasValue());
        const auto sceneInputDigest = ComputeSha256(std::as_bytes(std::span{encodedScene.Value()}));
        Tests::OwnedTestDirectory artifactOwner{"headless-replay"};
        const auto artifactPath = artifactOwner.Path() / std::filesystem::path{u8"replay evidence ü"};
        const auto utf8 = artifactPath.u8string();
        const auto output =
            Internal::HeadlessReplayOutput::Parse(std::string_view{reinterpret_cast<const char *>(utf8.data()), utf8.size()});
        REQUIRE(output.HasValue());
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
        const auto started = host.Value()->Startup();
        INFO((started.HasError() ? started.ErrorValue().message : "startup succeeded"));
        REQUIRE(started.HasValue());
        REQUIRE(host.Value()->RunFrame().HasValue());
        CHECK_FALSE(replay->TakeCommitted());
        clock.Advance(Duration::FromNanoseconds(16'666'667));
        const auto fixed = host.Value()->RunFrame();
        if (abort) {
            REQUIRE(fixed.HasError());
            CHECK(fixed.ErrorValue().message == "Reject prepared replay test tick.");
            CHECK_FALSE(replay->TakeCommitted());
            CHECK_FALSE(std::filesystem::exists(artifactPath));
            return;
        }
        INFO((fixed.HasError() ? fixed.ErrorValue().message : "success"));
        REQUIRE(fixed.HasValue());
        auto captured = replay->TakeCommitted();
        REQUIRE(captured);
        if (referenceGeneration && *referenceGeneration != 1) {
            REQUIRE(captured->HasError());
            CHECK(captured->ErrorValue().code.Value() == AI::AIErrors::CanonicalRestoreStale.code.Value());
            CHECK_FALSE(replay->TakeCommitted());
            return;
        }
        INFO((captured->HasError() ? captured->ErrorValue().message : "success"));
        REQUIRE(captured->HasValue());
        const auto &data = captured->Value();
        CHECK(data.definitionRevision == definitionRevision);
        CHECK(data.sceneInput == sceneInputDigest);
        const auto &evidence = data.evidence;
        CHECK(evidence.tick == 1);
        CHECK(evidence.subject == agent);
        REQUIRE(evidence.trace);
        CHECK(evidence.trace->Header().planDigest == planDigest);
        std::size_t references{};
        auto decoded = AI::DecodeAiCanonicalStateRecord(data.canonicalRecord, Internal::HeadlessDecisionReplay::CanonicalBudget,
                                                        [&](const AI::AiSavedEntityReference &reference) {
            ++references;
            std::uint64_t encodedScene{}, encodedObject{};
            for (std::size_t byte = 0; byte < sizeof(encodedScene); ++byte) {
                encodedScene = (encodedScene << 8) | reference.identity[byte];
                encodedObject = (encodedObject << 8) | reference.identity[sizeof(encodedScene) + byte];
            }
            CHECK(encodedScene == definitionId.value);
            CHECK(encodedObject == 2);
            CHECK(reference.generation == 1);
            return Result<AI::BlackboardStoredEntityReference>::Success({7, 1, 1});
        });
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value() == evidence.canonical);
        CHECK(references == (referenceGeneration ? 1 : 0));
        ExpectReplayArtifact(output.Value(), data);
        CHECK_FALSE(replay->TakeCommitted());
        host.Value()->Shutdown();
    }
}  // namespace Horo::Application::PrefabAtomicTest
