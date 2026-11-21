#include "HeadlessNetworkServices.h"
#include "HeadlessReplayArtifact.h"
#include "Horo/AI/AICanonicalState.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Scene/CookedSceneDefinition.h"
#include "NetworkProductHost.h"
#include "OwnedTestDirectory.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
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

        /** @brief Compares the producer's real artifact and verifies ordered divergence and corrupted evidence handling. */
        void ExpectPortableComparison(const std::filesystem::path &file, nlohmann::json artifact,
                                      const Physics::PhysicsReplayConfiguration &configuration) {
            const auto same = Internal::CompareHeadlessReplayArtifacts(file, file);
            INFO((same.HasError() ? same.ErrorValue().message : "evidence accepted"));
            REQUIRE(same.HasValue());
            CHECK_FALSE(same.Value());
            if (!artifact.contains("characterMotion"))
                return;

            const auto candidate = file.parent_path() / "changed evidence.json";
            const auto writeCandidate = [&] {
                std::ofstream stream{candidate, std::ios::binary | std::ios::trunc};
                REQUIRE(stream.is_open());
                stream << artifact.dump();
                stream.close();
                REQUIRE(stream.good());
            };
            auto &physics = artifact.at("physics");
            const auto baselineRevision = std::stoull(physics.at("publicationRevision").get<std::string>());
            physics.at("publicationRevision") = std::to_string(baselineRevision + 1);
            writeCandidate();
            const auto different = Internal::CompareHeadlessReplayArtifacts(file, candidate);
            REQUIRE(different.HasValue());
            REQUIRE(different.Value());
            CHECK(different.Value()->boundary == AI::AiReplayBoundary::Physics);
            CHECK(different.Value()->tick == 1);
            auto left = file.u8string();
            auto right = candidate.u8string();
            std::array<char *, 2> arguments{reinterpret_cast<char *>(left.data()), reinterpret_cast<char *>(right.data())};
            std::ostringstream out, err;
            CHECK(Internal::RunHeadlessReplayComparison(arguments, out, err) == 1);
            CHECK(out.str().find("Divergence at tick 1 in physics") != std::string::npos);
            CHECK(err.str().empty());

            auto incompatibleBuild = configuration;
            incompatibleBuild.build = configuration.solver;
            physics.at("buildDigest") = FormatSha256(incompatibleBuild.build);
            physics.at("profile") = FormatSha256(incompatibleBuild.Fingerprint());
            writeCandidate();
            const auto incompatible = Internal::CompareHeadlessReplayArtifacts(file, candidate);
            REQUIRE(incompatible.HasError());
            CHECK(incompatible.ErrorValue().code.Value() == AI::AIErrors::CanonicalSchemaUnsupported.code.Value());

            physics.at("bodies").at(0).at("activity") = 99;
            writeCandidate();
            const auto malformed = Internal::CompareHeadlessReplayArtifacts(file, candidate);
            REQUIRE(malformed.HasError());
            CHECK(malformed.ErrorValue().code.Value() == AI::AIErrors::CanonicalStateInvalid.code.Value());
        }

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
            CHECK(artifact.at("tick") == std::to_string(data.evidence.tick));
            CHECK(artifact.at("agentId") == std::to_string(data.evidence.subject.Value()));
            CHECK(artifact.at("canonicalAi").at("digest") == FormatSha256(digest));
            CHECK(artifact.at("canonicalAi").at("recordVersion") == AI::AiCanonicalReferenceRecordVersion);
            const auto &coverage = artifact.at("manifest").at("boundaries");
            CHECK(coverage.at("scene-input") == "deterministic");
            CHECK(coverage.at("decision") == "deterministic");
            CHECK(coverage.at("canonical-ai") == "deterministic");
            CHECK(coverage.at("perception-input") == (data.perception ? "deterministic" : "omitted"));
            if (const auto &perception = data.perception) {
                const auto &input = artifact.at("perceptionInput");
                CHECK(input.at("scope") == "spatial-publication");
                CHECK(input.at("sourceCount") == 1);
                CHECK(input.at("listenerCount") == 1);
                CHECK(input.at("digest") == FormatSha256(perception->digest));
            }
            REQUIRE(data.physics);
            CHECK(coverage.at("physics") == "deterministic");
            const auto &physics = artifact.at("physics");
            CHECK(physics.at("physicsTick") == std::to_string(data.evidence.tick));
            CHECK(physics.at("scope") == "authored-rigid-body-motion");
            const auto &configuration = data.physics->configuration;
            CHECK(physics.at("schemaVersion") == 2);
            CHECK(physics.at("settingsDigest") == FormatSha256(configuration.settings.digest));
            CHECK(physics.at("buildDigest") == FormatSha256(configuration.build));
            CHECK(physics.at("solverDigest") == FormatSha256(configuration.solver));
            CHECK(physics.at("profile") == FormatSha256(configuration.Fingerprint()));
            CHECK(configuration.filters != Sha256Digest{});
            CHECK(configuration.surfaces != Sha256Digest{});
            const auto &bodies = physics.at("bodies");
            const auto canonicalBodies = bodies.dump();
            CHECK(physics.at("digest") == FormatSha256(ComputeSha256(std::as_bytes(std::span{canonicalBodies}))));
            REQUIRE(bodies.size() == (data.characterMotion ? 2 : 0));
            if (!bodies.empty()) {
                CHECK(bodies.at(0).at("objectId") == "4");
                CHECK(bodies.at(1).at("objectId") == "5");
                CHECK(bodies.at(1).at("bodySlot") == "300");
                CHECK(bodies.at(1).at("positionMeters").at(1).get<float>() < 10);
                CHECK(bodies.at(1).at("linearVelocityMetersPerSecond").at(1).get<float>() < 0);
            }
            CHECK(coverage.at("navigation") == "omitted");
            CHECK(coverage.at("gameplay") == (data.gameplay ? "deterministic" : "omitted"));
            if (const auto &gameplay = data.gameplay) {
                const auto &section = artifact.at("gameplay");
                CHECK(section.at("source") == "native");
                CHECK(section.at("profile") == FormatSha256(gameplay->profile.digest));
                const auto &records = section.at("records");
                CHECK(records.at("callbacks") == (data.characterMotion ? 1 : 0));
                if (data.characterMotion) {
                    CHECK(records.at("sceneCommandsQueued") == true);
                    const auto &transforms = records.at("transforms");
                    REQUIRE(transforms.size() == 1);
                    CHECK(transforms.at(0).at("objectId") == "3");
                    CHECK(transforms.at(0).at("translation").at(0) == 21);
                }
            }
            CHECK(coverage.at("query-publication") == (data.queries ? "deterministic" : "omitted"));
            if (data.queries) {
                const bool terminal = data.evidence.tick == 3;
                REQUIRE(artifact.at("queryPublication").at("records").size() == (terminal ? 1 : 0));
                REQUIRE(artifact.at("blackboardSync").at("records").size() == (terminal ? 0 : 1));
                const auto &record = artifact.at(terminal ? "queryPublication" : "blackboardSync").at("records").at(0);
                CHECK(record.at("queryId") == "1");
                CHECK(record.at("outputKeyId") == "4");
                CHECK(record.at("publicationTick") == "3");
                CHECK(record.at("winner").at("positionMillimeters").is_array());
            }
            CHECK(coverage.at("achieved-character-motion") == (data.characterMotion ? "deterministic" : "omitted"));
            if (data.characterMotion) {
                const auto &capture = artifact.at("characterMotion");
                const auto &motion = capture.at("records");
                REQUIRE(motion.size() == 1);
                CHECK(capture.at("schemaVersion") == 1);
                const auto canonicalMotion = motion.dump();
                CHECK(capture.at("digest") == FormatSha256(ComputeSha256(std::as_bytes(std::span{canonicalMotion}))));
                CHECK(motion.at(0).at("objectId") == "3");
                CHECK(motion.at(0).at("componentId") == "7");
                CHECK(motion.at(0).at("velocityMetersPerSecond").at(0).get<float>() > 0);
            }
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
            stream.close();
            retained.close();
            ExpectPortableComparison(file, artifact, configuration);
        }

        /** @brief Verifies shared first-difference ordering using evidence from an actual committed fixed tick. */
        void ExpectReplayComparison(const Internal::HeadlessDecisionReplayData &data) {
            const auto equal = Internal::CompareHeadlessReplayCaptures(data, data);
            REQUIRE(equal.HasValue());
            CHECK_FALSE(equal.Value());
            if (data.perception) {
                auto changed = data;
                changed.perception->digest = data.sceneInput;
                const auto perception = Internal::CompareHeadlessReplayCaptures(data, changed);
                REQUIRE(perception.HasValue());
                REQUIRE(perception.Value());
                CHECK(perception.Value()->boundary == AI::AiReplayBoundary::PerceptionInput);
            }
            if (!data.characterMotion)
                return;

            REQUIRE_FALSE(data.characterMotion->empty());
            auto changed = data;
            changed.characterMotion->front().position.x += 0.125F;
            const auto character = Internal::CompareHeadlessReplayCaptures(data, changed);
            REQUIRE(character.HasValue());
            REQUIRE(character.Value());
            CHECK(character.Value()->tick == data.evidence.tick);
            CHECK(character.Value()->boundary == AI::AiReplayBoundary::AchievedCharacterMotion);
            REQUIRE(changed.gameplay);
            ++changed.gameplay->callbacks;
            const auto gameplay = Internal::CompareHeadlessReplayCaptures(data, changed);
            REQUIRE(gameplay.HasValue());
            REQUIRE(gameplay.Value());
            CHECK(gameplay.Value()->boundary == AI::AiReplayBoundary::Gameplay);
            changed.gameplay = data.gameplay;
            REQUIRE(changed.physics);
            REQUIRE_FALSE(changed.physics->bodies.empty());
            changed.physics->bodies.back().pose.translation.y -= 1.0F;
            ++changed.physics->publication.publicationRevision;
            const auto physics = Internal::CompareHeadlessReplayCaptures(data, changed);
            REQUIRE(physics.HasValue());
            REQUIRE(physics.Value());
            CHECK(physics.Value()->boundary == AI::AiReplayBoundary::Physics);
            REQUIRE(physics.Value()->recorded);
            REQUIRE(physics.Value()->replayed);
            CHECK(physics.Value()->recorded->revision + 1 == physics.Value()->replayed->revision);

            changed.physics->configuration.settings.digest = changed.sceneInput;
            const auto incompatible = Internal::CompareHeadlessReplayCaptures(data, changed);
            REQUIRE(incompatible.HasError());
            CHECK(incompatible.ErrorValue().code.Value() == AI::AIErrors::CanonicalSchemaUnsupported.code.Value());
            changed.physics->configuration = data.physics->configuration;
            changed.physics->configuration.build = changed.sceneInput;
            const auto changedBuild = Internal::CompareHeadlessReplayCaptures(data, changed);
            REQUIRE(changedBuild.HasError());
            CHECK(changedBuild.ErrorValue().code.Value() == AI::AIErrors::CanonicalSchemaUnsupported.code.Value());
        }
    }  // namespace

    /** @copydoc ExpectDecisionReplay */
    void ExpectDecisionReplay(std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition,
                              Internal::HeadlessSceneActivationInput input, AI::AgentId agent, const Sha256Digest &planDigest, bool abort,
                              std::optional<std::uint32_t> referenceGeneration, std::optional<Internal::GameplayWorldSelection> gameplay,
                              const std::uint64_t tick) {
        using namespace Network;
        const auto definitionId = definition->Id();
        const bool hasPerception = static_cast<bool>(input.perceptionPositions);
        const bool hasCharacter = std::ranges::any_of(definition->Entities(), [](const auto &entity) {
            return entity.components.characterController.has_value();
        });
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
        const auto replay = std::make_shared<Internal::HeadlessDecisionReplay>(Internal::HeadlessDecisionReplaySelection{agent, tick});
        input.decisionReplay = replay;
        if (hasCharacter) {
            std::ifstream revision{HORO_TEST_GAME_MODULE_REVISION_PATH};
            std::uint64_t descriptorRevision{};
            REQUIRE((revision >> descriptorRevision));
            gameplay = Internal::GameplayWorldSelection{.moduleId = "game.tests",
                                                        .nativeArtifact = HORO_TEST_GAME_MODULE_PATH,
                                                        .descriptorRevision = descriptorRevision,
                                                        .physicsPermission = Internal::GameplayPhysicsPermission::Granted,
                                                        .moduleRetirements = std::make_shared<Gameplay::GameModuleRetirementQueue>(),
                                                        .characterPermission = Internal::GameplayCharacterPermission::Granted};
        }
        auto configured = Internal::ComposeHeadlessSceneNetworkServices(std::move(definition), std::move(input), {}, gameplay);
        INFO((configured.HasError() ? configured.ErrorValue().message : "composition succeeded"));
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
        for (std::uint64_t step = 1; step < tick; ++step) {
            clock.Advance(Duration::FromNanoseconds(16'666'667));
            REQUIRE(host.Value()->RunFrame().HasValue());
            CHECK_FALSE(replay->TakeCommitted());
        }
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
        CHECK(data.gameplay.has_value() == gameplay.has_value());
        CHECK(data.definitionRevision == definitionRevision);
        CHECK(data.sceneInput == sceneInputDigest);
        CHECK(data.perception.has_value() == hasPerception);
        REQUIRE(data.characterMotion.has_value() == hasCharacter);
        if (data.characterMotion)
            CHECK(data.characterMotion->size() == 1);
        const auto &evidence = data.evidence;
        CHECK(evidence.tick == tick);
        CHECK(data.queries.has_value() == (tick > 1));
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
        ExpectReplayComparison(data);
        ExpectReplayArtifact(output.Value(), data);
        CHECK_FALSE(replay->TakeCommitted());
        host.Value()->Shutdown();
    }
}  // namespace Horo::Application::PrefabAtomicTest
