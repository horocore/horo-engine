#include "HeadlessReplayArtifact.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Foundation/Utf8.h"
#include "Horo/Physics/PhysicsWorld.h"

#include <algorithm>
#include <charconv>
#include <new>
#include <nlohmann/json.hpp>
#include <ostream>
#include <string>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        using Json = nlohmann::json;
        using Comparison = Result<std::optional<AI::AiReplayDivergence>>;
        using enum AI::AiReplayBoundary;

        struct InvalidEvidence final {};

        struct PortableBoundary final {
            AI::AiReplayBoundaryProfile profile;
            AI::AiReplayObservation observation;
        };

        struct PortableReplay final {
            std::uint64_t tick{};
            std::uint64_t agent{};
            std::array<PortableBoundary, AI::AiReplayBoundaryCount> boundaries;

            PortableBoundary &At(AI::AiReplayBoundary boundary) noexcept {
                return boundaries[static_cast<std::size_t>(boundary)];
            }
        };

        /** @brief Reads a canonical unsigned decimal string without JSON numeric rounding. */
        std::uint64_t ReadWord(const Json &field) {
            const auto &text = field.get_ref<const std::string &>();
            std::uint64_t value{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || (text.size() > 1 && text.front() == '0'))
                throw InvalidEvidence{};
            return value;
        }

        /** @brief Reads a required nonzero owner identity or publication revision. */
        std::uint64_t ReadIdentity(const Json &field) {
            if (const auto value = ReadWord(field); value != 0)
                return value;
            throw InvalidEvidence{};
        }

        /** @brief Rejects missing or empty content identities. */
        Sha256Digest ReadDigest(const Json &field) {
            const auto digest = ParseSha256(field.get_ref<const std::string &>());
            if (digest.HasError() || digest.Value() == Sha256Digest{})
                throw InvalidEvidence{};
            return digest.Value();
        }

        /** @brief Recomputes exact hashes for portable records retained in the file. */
        Sha256Digest HashJson(const Json &value) {
            const auto text = value.dump();
            return ComputeSha256(std::as_bytes(std::span{text}));
        }

        /** @brief Requires the format's recorded coverage to match exactly the sections supplied. */
        void ValidateCoverage(const Json &file, const PortableReplay &replay) {
            const auto &manifest = file.at("manifest");
            const auto &coverage = manifest.at("boundaries");
            if (manifest.at("version") != AI::AiReplayManifest::CurrentVersion || !coverage.is_object() ||
                coverage.size() != HeadlessReplayBoundaryNames.size())
                throw InvalidEvidence{};
            auto recorded = Json::array();
            for (std::size_t index = 0; index < HeadlessReplayBoundaryNames.size(); ++index) {
                const auto name = HeadlessReplayBoundaryNames[index];
                const auto mode = replay.boundaries[index].profile.coverage;
                const auto expected = mode == AI::AiReplayCoverage::Nondeterministic ? "nondeterministic"
                                      : mode == AI::AiReplayCoverage::Deterministic  ? "deterministic"
                                                                                     : "omitted";
                if (coverage.at(name) != expected)
                    throw InvalidEvidence{};
                if (mode != AI::AiReplayCoverage::Omitted)
                    recorded.push_back(name);
            }
            if (file.at("recordedBoundaries") != recorded)
                throw InvalidEvidence{};
        }

        /** @brief Bounds the v4 record projection and requires the producer's stable observation fields. */
        void ValidateDecision(const Json &decision) {
            const auto &records = decision.at("records");
            const auto &blackboard = decision.at("blackboard");
            const auto &schemaVersion = blackboard.at("schemaVersion");
            const auto &instance = blackboard.at("instanceGeneration");
            const auto &outcome = decision.at("outcome");
            if (decision.at("traceVersion") != AI::DecisionTraceHeader::CurrentVersion || !records.is_array() ||
                records.size() > AI::DecisionStepTrace::MaximumRecords || !outcome.is_number_unsigned() ||
                outcome > static_cast<unsigned>(AI::AiTaskState::Cancelled) || !schemaVersion.is_number_unsigned() || schemaVersion == 0 ||
                !instance.is_number_unsigned() || instance == 0)
                throw InvalidEvidence{};
            (void)ReadIdentity(decision.at("rootGraphId"));
            (void)ReadIdentity(blackboard.at("schemaId"));
            for (const auto &record : records) {
                constexpr std::array stableIds{"graphId", "callerId", "nodeId", "taskId", "transitionId", "keyId"};
                constexpr std::array ordinals{"kind", "occurrence", "parentOccurrence", "state", "transitionOrdinalOrCount"};
                const auto &score = record.at("utilityScore");
                if ((!score.is_null() && !score.is_number()) || !record.at("hasActiveTaskHandle").is_boolean())
                    throw InvalidEvidence{};
                for (const auto *key : stableIds)
                    if (!record.at(key).is_string())
                        throw InvalidEvidence{};
                for (const auto *key : ordinals)
                    if (!record.at(key).is_number_unsigned())
                        throw InvalidEvidence{};
            }
        }

        /** @brief Reads scene, decision and canonical AI evidence with their original identifiers and revisions. */
        PortableReplay ReadRequired(const Json &file) {
            constexpr auto deterministic = AI::AiReplayCoverage::Deterministic;
            const auto sceneId = ReadIdentity(file.at("sceneDefinitionId"));
            const auto &scene = file.at("sceneInput");
            const auto &canonical = file.at("canonicalAi");
            const auto &decision = file.at("decision");
            if (scene.at("schemaVersion") != HeadlessSceneInputSchemaVersion ||
                canonical.at("recordVersion") != AI::AiCanonicalReferenceRecordVersion)
                throw InvalidEvidence{};
            ValidateDecision(decision);
            const auto &blackboard = decision.at("blackboard");
            const auto sceneDigest = ReadDigest(scene.at("digest"));
            PortableReplay result{ReadIdentity(file.at("tick")), ReadIdentity(file.at("agentId")), {}};
            result.At(SceneInput) = {{deterministic, HeadlessSceneInputSchemaVersion, sceneDigest},
                                     {{sceneId, ReadIdentity(scene.at("revision"))}, sceneDigest}};
            result.At(
                CanonicalAi) = {{deterministic, AI::AiCanonicalReferenceRecordVersion, sceneDigest},
                                {{sceneId, ReadIdentity(canonical.at("sceneStructureRevision"))}, ReadDigest(canonical.at("digest"))}};
            result.At(Decision) = {{deterministic, AI::DecisionTraceHeader::CurrentVersion, ReadDigest(decision.at("planDigest"))},
                                   {{ReadIdentity(blackboard.at("schemaGeneration")), ReadIdentity(blackboard.at("revision"))},
                                    HashJson(decision)}};
            return result;
        }

        /** @brief Bounds and verifies a native body or Character projection before it can participate in comparison. */
        PortableBoundary ReadMotion(const Json &section, const std::string_view recordsKey, AI::AiReplayStamp stamp,
                                    const std::size_t maximum, const std::uint32_t schema = 1) {
            const auto &records = section.at(recordsKey);
            if (section.at("schemaVersion") != schema || !records.is_array() || records.size() > maximum)
                throw InvalidEvidence{};
            const auto hash = HashJson(records);
            if (hash != ReadDigest(section.at("digest")))
                throw InvalidEvidence{};
            return {{AI::AiReplayCoverage::Deterministic, schema, ReadDigest(section.at("settingsDigest"))}, {stamp, hash}};
        }

        /** @brief Reconstructs and checks the solver-owned compatibility digest carried by a portable artifact. */
        Sha256Digest ReadPhysicsConfiguration(const Json &section) {
            const auto &fp = section.at("floatingPoint");
            const auto &architecture = fp.at("architecture");
            if (!architecture.is_number_unsigned() || architecture == 0 ||
                architecture > static_cast<unsigned>(Physics::PhysicsStateFpArchitecture::ArmFpcr))
                throw InvalidEvidence{};
            const Physics::PhysicsReplayConfiguration configuration{ReadDigest(section.at("buildDigest")),
                                                                    ReadDigest(section.at("solverDigest")),
                                                                    ReadDigest(section.at("platformDigest")),
                                                                    ReadDigest(section.at("filtersDigest")),
                                                                    ReadDigest(section.at("surfacesDigest")),
                                                                    {ReadDigest(section.at("settingsDigest"))},
                                                                    {architecture.get<Physics::PhysicsStateFpArchitecture>(),
                                                                     ReadWord(fp.at("value"))}};
            const auto profile = configuration.Fingerprint();
            if (profile != ReadDigest(section.at("profile")))
                throw InvalidEvidence{};
            return profile;
        }

        /** @brief Reads the redacted query publication or its confirmed Blackboard write records. */
        PortableBoundary ReadQueryBoundary(const Json &section, std::uint64_t agent) {
            const auto &records = section.at("records");
            if (section.at("schemaVersion") != 1 || !records.is_array() || records.size() > 32)
                throw InvalidEvidence{};
            const auto hash = HashJson(records);
            if (hash != ReadDigest(section.at("digest")))
                throw InvalidEvidence{};
            return {{AI::AiReplayCoverage::Deterministic, 1, ReadDigest(section.at("profile"))},
                    {{agent, ReadIdentity(section.at("revision"))}, hash}};
        }

        /** @brief Reads only a bounded regular file and rejects deep JSON before recursive processing. */
        Result<PortableReplay> ReadReplay(const std::filesystem::path &path) {
            using Outcome = Result<PortableReplay>;
            const auto bytes = NativeDurableFileSystem{}.ReadFileBounded(path, MaximumHeadlessReplayEvidenceBytes);
            if (bytes.HasError())
                return Outcome::Failure(bytes.ErrorValue());
            const auto file = Json::parse(bytes.Value(), [](int depth, Json::parse_event_t, Json &) {
                if (depth > 16)
                    throw InvalidEvidence{};
                return true;
            });
            if (file.at("format") != "horo-ai-replay-evidence" || file.at("version") != 1)
                return Outcome::Failure(MakeError(AI::AIErrors::CanonicalSchemaUnsupported));
            auto result = ReadRequired(file);
            if (file.contains("navigation")) {
                const auto &navigation = file.at("navigation");
                const auto &records = navigation.at("results");
                if (navigation.at("schemaVersion") != 1 || navigation.at("scope") != "best-effort-path-publication" ||
                    !records.is_array() || records.size() > 32 || ReadIdentity(navigation.at("tick")) != result.tick ||
                    HashJson(records) != ReadDigest(navigation.at("digest")))
                    throw InvalidEvidence{};
                (void)ReadIdentity(navigation.at("topology"));
                result.At(Navigation).profile = {AI::AiReplayCoverage::Nondeterministic, 1, ReadDigest(navigation.at("profile"))};
            }
            if (file.contains("gameplay")) {
                const auto &gameplay = file.at("gameplay");
                const auto &records = gameplay.at("records");
                const auto &transforms = records.at("transforms");
                const auto &source = gameplay.at("source");
                if (gameplay.at("schemaVersion") != 2 || gameplay.at("scope") != "fixed-transform-submissions" ||
                    (source != "native" && source != "lua") || !transforms.is_array() || transforms.size() > 64 ||
                    !records.at("callbacks").is_number_unsigned() || !records.at("sceneCommandsQueued").is_boolean())
                    throw InvalidEvidence{};
                const auto digest = HashJson(records);
                if (digest != ReadDigest(gameplay.at("digest")))
                    throw InvalidEvidence{};
                const auto image = ReadDigest(gameplay.at("sourceDigest"));
                const auto profile = ReadDigest(gameplay.at("profile"));
                const std::array<std::span<const std::byte>, 2> parts{std::as_bytes(std::span{image.bytes}),
                                                                      std::as_bytes(std::span{profile.bytes})};
                result.At(Gameplay) = {{AI::AiReplayCoverage::Deterministic, 2, ComputeSha256Fragments(parts)},
                                       {{ReadIdentity(gameplay.at("revision")), result.tick}, digest}};
            }
            if (file.contains("queryPublication") || file.contains("blackboardSync")) {
                result.At(QueryPublication) = ReadQueryBoundary(file.at("queryPublication"), result.agent);
                result.At(BlackboardSync) = ReadQueryBoundary(file.at("blackboardSync"), result.agent);
                if (result.At(QueryPublication).profile != result.At(BlackboardSync).profile)
                    throw InvalidEvidence{};
            }
            if (file.contains("perceptionInput")) {
                const auto &input = file.at("perceptionInput");
                if (input.at("schemaVersion") != 1 || input.at("scope") != "spatial-publication")
                    throw InvalidEvidence{};
                const auto &scene = result.At(SceneInput);
                result.At(PerceptionInput) = {{AI::AiReplayCoverage::Deterministic, 1, scene.profile.content},
                                              {{scene.observation.stamp.generation, ReadIdentity(input.at("revision"))},
                                               ReadDigest(input.at("digest"))}};
            }
            if (file.contains("physics")) {
                const auto &physics = file.at("physics");
                if (physics.at("scope") != "authored-rigid-body-motion")
                    throw InvalidEvidence{};
                const AI::AiReplayStamp stamp{ReadIdentity(physics.at("physicsTick")), ReadIdentity(physics.at("publicationRevision"))};
                auto bodyMotion = ReadMotion(physics, "bodies", stamp, 128, 2);
                bodyMotion.profile.content = ReadPhysicsConfiguration(physics);
                result.At(Physics) = bodyMotion;
                if (file.contains("characterMotion"))
                    result.At(AchievedCharacterMotion) = ReadMotion(file.at("characterMotion"), "records", stamp, 256);
            } else if (file.contains("characterMotion")) {
                throw InvalidEvidence{};
            }
            ValidateCoverage(file, result);
            return Outcome::Success(std::move(result));
        }

        /** @brief Compares only matching profiles, following the shared boundary order of original fixed capture. */
        Comparison ComparePortable(const PortableReplay &left, const PortableReplay &right) {
            if (left.agent != right.agent)
                return Comparison::Failure(MakeError(AI::AIErrors::CanonicalSchemaUnsupported));
            for (std::size_t index = 0; index < left.boundaries.size(); ++index)
                if (left.boundaries[index].profile != right.boundaries[index].profile)
                    return Comparison::Failure(MakeError(AI::AIErrors::CanonicalSchemaUnsupported));
            if (left.tick != right.tick)
                return Comparison::Success(AI::AiReplayDivergence{std::min(left.tick, right.tick)});
            for (std::size_t index = 0; index < left.boundaries.size(); ++index) {
                const auto &a = left.boundaries[index].observation;
                const auto &b = right.boundaries[index].observation;
                if (a != b)
                    return Comparison::Success(
                        AI::AiReplayDivergence{left.tick, static_cast<AI::AiReplayBoundary>(index), a.stamp, b.stamp});
            }
            return Comparison::Success(std::nullopt);
        }

        /** @brief Decodes one explicitly provided UTF-8 filename without probing other paths. */
        std::filesystem::path InputPath(const std::string_view text) {
            if (text.empty() || !IsValidUtf8ScalarSequence(text))
                throw InvalidEvidence{};
            const std::filesystem::path path{std::u8string{text.begin(), text.end()}};
            return std::filesystem::absolute(path).lexically_normal();
        }
    }  // namespace

    /** @copydoc CompareHeadlessReplayArtifacts */
    Comparison CompareHeadlessReplayArtifacts(const std::filesystem::path &recorded, const std::filesystem::path &replayed) {
        try {
            const auto baseline = ReadReplay(recorded);
            if (baseline.HasError())
                return Comparison::Failure(baseline.ErrorValue());
            const auto candidate = ReadReplay(replayed);
            if (candidate.HasError())
                return Comparison::Failure(candidate.ErrorValue());
            return ComparePortable(baseline.Value(), candidate.Value());
        } catch (const InvalidEvidence &) {
            return Comparison::Failure(MakeError(AI::AIErrors::CanonicalStateInvalid));
        } catch (const Json::exception &) {
            return Comparison::Failure(MakeError(AI::AIErrors::CanonicalStateInvalid));
        } catch (const std::bad_alloc &) {
            return Comparison::Failure(MakeError(AI::AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    /** @copydoc RunHeadlessReplayComparison */
    int RunHeadlessReplayComparison(const std::span<char *> arguments, std::ostream &out, std::ostream &err) {
        if (arguments.size() != 2) {
            err << "Usage: horo-engine --compare-ai-replay <recorded evidence.json> <replayed evidence.json>\n";
            return 2;
        }
        try {
            const auto result = CompareHeadlessReplayArtifacts(InputPath(arguments[0]), InputPath(arguments[1]));
            if (result.HasError()) {
                err << result.ErrorValue().code.Value() << ": " << result.ErrorValue().message << '\n';
                return 2;
            }
            if (const auto &difference = result.Value()) {
                out << "Divergence at tick " << difference->tick;
                if (difference->boundary) {
                    out << " in " << HeadlessReplayBoundaryNames[static_cast<std::size_t>(*difference->boundary)] << " (recorded "
                        << difference->recorded->generation << '/' << difference->recorded->revision << ", replayed "
                        << difference->replayed->generation << '/' << difference->replayed->revision << ')';
                } else {
                    out << " (one recording has no matching tick)";
                }
                out << '\n';
                return 1;
            }
            out << "Replay evidence matches within the recorded coverage.\n";
            return 0;
        } catch (const InvalidEvidence &) {
            err << "Replay evidence paths must be valid UTF-8 filenames.\n";
            return 2;
        } catch (const std::filesystem::filesystem_error &) {
            err << "Replay evidence path could not be opened.\n";
            return 2;
        }
    }
}  // namespace Horo::Application::Internal
