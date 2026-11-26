#include "HeadlessNetworkServices.h"
#include "HeadlessReplayArtifact.h"
#include "Horo/AI/AICanonicalState.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/Foundation/Platform.h"
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
            physics.at("buildDigest") = FormatSha256(configuration.build);
            physics.at("profile") = FormatSha256(configuration.Fingerprint());
            auto &scene = artifact.at("sceneInput");
            const auto originalSceneDigest = scene.at("digest");
            scene.at("digest") = FormatSha256(configuration.filters);
            writeCandidate();
            const auto incompatibleScene = Internal::CompareHeadlessReplayArtifacts(file, candidate);
            REQUIRE(incompatibleScene.HasError());
            CHECK(incompatibleScene.ErrorValue().code.Value() == AI::AIErrors::CanonicalSchemaUnsupported.code.Value());
            scene.at("digest") = originalSceneDigest;
            auto &gameplay = artifact.at("gameplay");
            const auto originalImage = gameplay.at("sourceDigest");
            gameplay.at("sourceDigest") = FormatSha256(configuration.build);
            writeCandidate();
            const auto differentImage = Internal::CompareHeadlessReplayArtifacts(file, candidate);
            REQUIRE(differentImage.HasError());
            CHECK(differentImage.ErrorValue().code.Value() == AI::AIErrors::CanonicalSchemaUnsupported.code.Value());
            gameplay.at("sourceDigest") = originalImage;

            auto &omissions = artifact.at("manifest").at("omittedInputsAndState");
            const auto originalPhysicsOmissions = omissions.at("physics");
            omissions.erase("physics");
            writeCandidate();
            CHECK(Internal::CompareHeadlessReplayArtifacts(file, candidate).HasError());
            omissions["physics"] = originalPhysicsOmissions;

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
            const auto &manifest = artifact.at("manifest");
            const auto &coverage = manifest.at("boundaries");
            const auto &omissions = manifest.at("omittedInputsAndState");
            CHECK(manifest.at("version") == 2);
            CHECK(manifest.at("nondeterministicProviders").empty());
            CHECK(omissions.at("scene-input") == nlohmann::json::array({"streamed-cell-content", "runtime-structural-group-content"}));
            CHECK(omissions.at("physics") == nlohmann::json::array({"solver-internal-state", "external-physics-inputs"}));
            CHECK(coverage.at("scene-input") == "deterministic");
            CHECK(coverage.at("decision") == "deterministic");
            CHECK(coverage.at("canonical-ai") == "deterministic");
            CHECK(coverage.at("perception-input") == (data.perception ? "deterministic" : "omitted"));
            if (const auto &perception = data.perception) {
                CHECK(omissions.at("perception-input") == nlohmann::json::array({"provider-output", "provider-internal-state"}));
                const auto &input = artifact.at("perceptionInput");
                CHECK(input.at("scope") == "spatial-publication");
                CHECK(input.at("sourceCount") == perception->sourceCount);
                CHECK(input.at("listenerCount") == perception->listenerCount);
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
                CHECK(omissions.at("gameplay") ==
                      nlohmann::json::array({"private-module-state", "external-gameplay-inputs", "deferred-scene-publication"}));
                const auto &section = artifact.at("gameplay");
                CHECK(section.at("source") == "native");
                CHECK(section.at("schemaVersion") == 2);
                CHECK(section.at("sourceDigest") == FormatSha256(gameplay->profile.source));
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

        /** @brief Invokes the terminal export for a real replay file and returns its neutral JSON. */
        nlohmann::json ExportReplayTimeline(const std::filesystem::path &file, std::string limit) {
            auto input = file.u8string();
            std::array<char *, 2> arguments{reinterpret_cast<char *>(input.data()), limit.data()};
            std::ostringstream out, err;
            CHECK(Internal::RunHeadlessReplayTimeline(arguments, out, err) == 0);
            INFO(err.str());
            REQUIRE(err.str().empty());
            return nlohmann::json::parse(out.str());
        }

        /** @brief Checks that the terminal exposes the actual authored stimulus identity and no provider values. */
        void ExpectStimulusTimeline(const std::filesystem::path &file) {
            const auto timeline = ExportReplayTimeline(file, "512");
            const auto &events = timeline.at("events");
            const auto stimulus = std::ranges::find_if(events, [](const auto &event) {
                return event.at("boundary") == "perception-stimulus" && !event.at("ordinal").is_null();
            });
            REQUIRE(stimulus != events.end());
            CHECK(stimulus->at("tick") == "1");
            const auto &fields = stimulus->at("operation");
            CHECK(fields.at("sourceObjectId") == "2");
            CHECK(fields.at("listenerId") == "91");
            CHECK(fields.at("firstSensedTick") == "1");
            CHECK(fields.at("lastSensedTick") == "1");
            CHECK(fields.at("sensed") == true);
            CHECK(fields.size() == 7);
        }

        /** @brief Checks exact capture ordering, export redaction, and visible overflow. */
        void ExpectTimelineExport(const std::filesystem::path &file) {
            const auto timeline = ExportReplayTimeline(file, "512");
            CHECK(timeline.at("droppedEvents") == 0);
            const auto &events = timeline.at("events");
            REQUIRE(events.size() > 2);
            CHECK(events.front().at("tick") == "3");
            CHECK(events.back().at("tick") == "4");
            CHECK(std::ranges::is_sorted(events, {}, [](const auto &event) {
                return event.at("tick");
            }));
            const auto publication = std::ranges::find_if(events, [](const auto &event) {
                return event.at("boundary") == "query-publication" && !event.at("ordinal").is_null();
            });
            const auto sync = std::ranges::find_if(events, [](const auto &event) {
                return event.at("boundary") == "blackboard-sync" && !event.at("ordinal").is_null();
            });
            REQUIRE(publication != events.end());
            REQUIRE(sync != events.end());
            CHECK(publication->at("tick") == "3");
            CHECK(sync->at("tick") == "4");
            CHECK(publication->at("operation") == sync->at("operation"));
            const auto text = timeline.dump();
            CHECK(text.find("positionMillimeters") == std::string::npos);
            CHECK(text.find("fixture-private-input") == std::string::npos);
            const auto bounded = ExportReplayTimeline(file, "1");
            CHECK(bounded.at("droppedEvents") == events.size() - 1);
            CHECK(bounded.at("events") == nlohmann::json::array({events.front()}));
        }

        /** @brief Reads the terminal comparison's privacy-filtered navigation address after the human-readable line. */
        nlohmann::json ReadDivergenceLocation(const std::filesystem::path &recorded, const std::filesystem::path &replayed) {
            auto left = recorded.u8string();
            auto right = replayed.u8string();
            std::array<char *, 2> arguments{reinterpret_cast<char *>(left.data()), reinterpret_cast<char *>(right.data())};
            std::ostringstream out, err;
            CHECK(Internal::RunHeadlessReplayComparison(arguments, out, err) == 1);
            CHECK(err.str().empty());
            const auto text = out.str();
            const auto newline = text.find('\n');
            REQUIRE(newline != std::string::npos);
            return nlohmann::json::parse(text.substr(newline + 1));
        }

        /** @brief Checks that stimulus differences supplement only a deterministic spatial divergence. */
        void ExpectPerceptionDivergence(const std::filesystem::path &file, const Sha256Digest &differentDigest) {
            std::ifstream original{file, std::ios::binary};
            REQUIRE(original.is_open());
            auto changed = nlohmann::json::parse(original);
            auto &stimuli = changed.at("perceptionStimuli");
            auto &records = stimuli.at("records");
            REQUIRE(records.size() == 1);
            auto &stimulus = records.front();
            stimulus["sensed"] = false;
            stimulus["privatePayload"] = "private-stimulus-data";

            const auto candidate = file.parent_path() / "changed-stimulus.json";
            const auto saveCandidate = [&] {
                std::ofstream stream{candidate, std::ios::binary | std::ios::trunc};
                stream << changed;
                REQUIRE(stream.good());
            };
            saveCandidate();
            const auto diagnosticOnly = Internal::CompareHeadlessReplayArtifacts(file, candidate);
            REQUIRE(diagnosticOnly.HasValue());
            CHECK_FALSE(diagnosticOnly.Value());

            changed.at("perceptionInput")["digest"] = FormatSha256(differentDigest);
            saveCandidate();
            const auto divergence = ReadDivergenceLocation(file, candidate);
            CHECK(divergence.at("boundary") == "perception-input");
            CHECK(divergence.at("ordinal").is_null());
            const auto &related = divergence.at("relatedStimulus");
            const auto exported = ExportReplayTimeline(candidate, "512");
            const auto &events = exported.at("events");
            const auto match = std::ranges::find_if(events, [&](const auto &event) {
                return event.at("boundary") == related.at("boundary") && event.at("ordinal") == related.at("ordinal") &&
                       event.at("tick") == related.at("tick") && event.at("agentId") == related.at("agentId");
            });
            REQUIRE(match != events.end());
            CHECK(related.at("recorded").at("sourceObjectId") == "2");
            CHECK(related.at("recorded").at("sensed") == true);
            CHECK(related.at("replayed").at("sensed") == false);
            CHECK(related.at("replayed") == match->at("operation"));
            CHECK_FALSE(related.at("replayed").contains("privatePayload"));

            stimulus["sensed"] = true;
            saveCandidate();
            CHECK_FALSE(ReadDivergenceLocation(file, candidate).contains("relatedStimulus"));
            stimuli["available"] = false;
            stimuli["records"] = nlohmann::json::array();
            saveCandidate();
            CHECK_FALSE(ReadDivergenceLocation(file, candidate).contains("relatedStimulus"));
        }

        /** @brief Checks the real consecutive captures and locates a difference in the later boundary. */
        void ExpectReplayWindow(const Internal::HeadlessReplayOutput &output,
                                std::span<const Internal::HeadlessDecisionReplayData> frames) {
            REQUIRE(frames.size() == 2);
            REQUIRE(Internal::PublishHeadlessReplayWindow(output, frames).HasValue());
            const auto file = output.Directory() / "evidence.json";
            std::ifstream stream{file, std::ios::binary};
            REQUIRE(stream.is_open());
            auto window = nlohmann::json::parse(stream);
            CHECK(window.at("version") == 2);
            auto &encoded = window.at("frames");
            REQUIRE(encoded.size() == 2);
            CHECK(encoded.at(0).at("tick") == "3");
            CHECK(encoded.at(1).at("tick") == "4");
            CHECK(encoded.at(0).at("queryPublication").at("records").size() == 1);
            CHECK(encoded.at(0).at("blackboardSync").at("records").empty());
            CHECK(encoded.at(1).at("queryPublication").at("records").empty());
            CHECK(encoded.at(1).at("blackboardSync").at("records").size() == 1);
            const auto same = Internal::CompareHeadlessReplayArtifacts(file, file);
            REQUIRE(same.HasValue());
            CHECK_FALSE(same.Value());
            const auto candidate = output.Directory() / "candidate.json";
            const auto writeCandidate = [&] {
                std::ofstream changed{candidate, std::ios::binary | std::ios::trunc};
                changed << window;
                REQUIRE(changed.good());
            };
            std::array distinctScenes{frames.front(), frames.back()};
            ++distinctScenes.back().definition.value;
            const auto mixedPath = (output.Directory() / "mixed-scenes").u8string();
            const auto mixed = Internal::HeadlessReplayOutput::Parse(
                std::string_view{reinterpret_cast<const char *>(mixedPath.data()), mixedPath.size()});
            REQUIRE(mixed.HasValue());
            CHECK(Internal::PublishHeadlessReplayWindow(mixed.Value(), distinctScenes).HasError());
            CHECK_FALSE(std::filesystem::exists(mixed.Value().Directory()));

            auto &sceneIdentity = encoded.at(1).at("sceneDefinitionId");
            const auto originalScene = sceneIdentity;
            sceneIdentity = std::to_string(distinctScenes.back().definition.value);
            writeCandidate();
            CHECK(Internal::CompareHeadlessReplayArtifacts(file, candidate).HasError());
            sceneIdentity = originalScene;

            auto &decisions = encoded.at(0).at("decision").at("records");
            REQUIRE_FALSE(decisions.empty());
            auto &record = decisions.back();
            const auto originalNode = record.at("nodeId");
            record["nodeId"] = "999";
            record["privatePayload"] = "fixture-private-input";
            writeCandidate();
            ExpectTimelineExport(candidate);
            const auto node = ReadDivergenceLocation(file, candidate);
            CHECK(node.at("boundary") == "decision");
            CHECK(node.at("tick") == "3");
            CHECK(node.at("agentId") == encoded.at(0).at("agentId"));
            CHECK(node.at("ordinal") == decisions.size() - 1);
            CHECK(node.at("recorded").at("nodeId") == originalNode);
            CHECK(node.at("replayed").at("nodeId") == "999");
            CHECK_FALSE(node.at("replayed").contains("privatePayload"));
            record["nodeId"] = originalNode;
            record.erase("privatePayload");
            encoded.at(1).at("canonicalAi").at("digest") = FormatSha256(frames.back().sceneInput);
            writeCandidate();
            const auto different = Internal::CompareHeadlessReplayArtifacts(file, candidate);
            REQUIRE(different.HasValue());
            REQUIRE(different.Value());
            CHECK(different.Value()->tick == 4);
            CHECK(different.Value()->boundary == AI::AiReplayBoundary::CanonicalAi);
            const auto state = ReadDivergenceLocation(file, candidate);
            CHECK(state.at("tick") == "4");
            CHECK(state.at("boundary") == "canonical-ai");
            CHECK(state.at("ordinal").is_null());
            CHECK(state.at("replayed").at("digest") == encoded.at(1).at("canonicalAi").at("digest"));
            encoded.at(1).at("tick") = "5";
            writeCandidate();
            CHECK(Internal::CompareHeadlessReplayArtifacts(file, candidate).HasError());
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
                              Internal::HeadlessSceneActivationInput input, const Internal::HeadlessDecisionReplaySelection replaySelection,
                              const Sha256Digest &planDigest, bool abort, std::optional<std::uint32_t> referenceGeneration,
                              std::optional<Internal::GameplayWorldSelection> gameplay,
                              const std::span<const std::uint8_t> materialPayload) {
        using namespace Network;
        const auto [agent, tick, tickCount] = replaySelection;
        const auto definitionId = definition->Id();
        const bool hasPerception = static_cast<bool>(input.perceptionPositions);
        const bool hasNativeSensing = input.perceptionExecution.has_value();
        const bool hasCharacter = std::ranges::any_of(definition->Entities(), [](const auto &entity) {
            return entity.components.characterController.has_value();
        });
        const auto definitionRevision = definition->Revision();
        const auto encodedScene = SceneCook::EncodeCookedSceneDefinition(*definition);
        REQUIRE(encodedScene.HasValue());
        const auto definitionDigest = ComputeSha256(std::as_bytes(std::span{encodedScene.Value()}));
        auto sceneInputDigest = ComputeSha256(std::as_bytes(std::span{definitionDigest.bytes}));
        const auto dependencies = definition->AssetDependencies();
        REQUIRE(dependencies.size() == (materialPayload.empty() ? 0 : 1));
        if (!materialPayload.empty()) {
            const auto materialDigest = ComputeSha256(std::as_bytes(materialPayload));
            const std::array<std::span<const std::byte>, 3> input{std::as_bytes(std::span{definitionDigest.bytes}),
                                                                  std::as_bytes(std::span{dependencies.front().id.Bytes()}),
                                                                  std::as_bytes(std::span{materialDigest.bytes})};
            sceneInputDigest = ComputeSha256Fragments(input);
        }
        Tests::OwnedTestDirectory artifactOwner{"headless-replay"};
        const auto artifactPath = artifactOwner.Path() / std::filesystem::path{u8"replay evidence ü"};
        const auto utf8 = artifactPath.u8string();
        const auto output =
            Internal::HeadlessReplayOutput::Parse(std::string_view{reinterpret_cast<const char *>(utf8.data()), utf8.size()});
        REQUIRE(output.HasValue());
        const auto replay = std::make_shared<Internal::HeadlessDecisionReplay>(replaySelection);
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
        if (tickCount > 1) {
            for (std::size_t frame = 1; frame < tickCount; ++frame) {
                CHECK_FALSE(replay->TakeCommittedWindow());
                clock.Advance(Duration::FromNanoseconds(16'666'667));
                REQUIRE(host.Value()->RunFrame().HasValue());
            }
            const auto window = replay->TakeCommittedWindow();
            REQUIRE(window);
            INFO((window->HasError() ? window->ErrorValue().message : "window committed"));
            REQUIRE(window->HasValue());
            ExpectReplayWindow(output.Value(), window->Value());
            CHECK_FALSE(replay->TakeCommittedWindow());
            host.Value()->Shutdown();
            return;
        }
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
        if (gameplay) {
            REQUIRE(data.gameplay);
            const auto bytes = NativeDurableFileSystem{}.ReadFileBounded(gameplay->nativeArtifact, 512U * 1024U * 1024U);
            REQUIRE(bytes.HasValue());
            CHECK(data.gameplay->profile.source == ComputeSha256(std::as_bytes(std::span{bytes.Value()})));
        }
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
        if (hasNativeSensing) {
            REQUIRE(data.stimuli);
            CHECK(data.stimuli->available);
            CHECK(data.stimuli->count == 1);
            CHECK(data.stimuli->unresolvedSources == 0);
            const auto file = output.Value().Directory() / "evidence.json";
            ExpectStimulusTimeline(file);
            ExpectPerceptionDivergence(file, data.sceneInput);
        } else {
            CHECK_FALSE(data.stimuli);
        }
        CHECK_FALSE(replay->TakeCommitted());
        host.Value()->Shutdown();
    }
}  // namespace Horo::Application::PrefabAtomicTest
