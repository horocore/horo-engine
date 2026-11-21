#include "HeadlessReplayArtifact.h"

#include "HeadlessNetworkServices.h"
#include "Horo/AI/AIReplayEvidence.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Foundation/Utf8.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Platform/SecureRandom.h"

#include <array>
#include <new>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        using Json = nlohmann::json;
        constexpr std::uint32_t SceneInputSchemaVersion = 1;
        constexpr std::uint32_t RigidBodyMotionSchemaVersion = 1;
        constexpr std::uint32_t CharacterMotionSchemaVersion = 1;

        /** @brief Hashes the same canonical array for comparison and detached artifact publication. */
        Sha256Digest HashRecords(const Json &records) {
            const auto canonical = records.dump();
            return ComputeSha256(std::as_bytes(std::span{canonical}));
        }

        /** @brief Describes supported and omitted boundaries even when trace records were captured for diagnostics. */
        Json EncodeCoverage(const HeadlessDecisionReplayData &data, Json &recorded) {
            constexpr auto &names = HeadlessReplayBoundaryNames;
            const std::array
                deterministic{true, false, false, false, true, false, false, data.physics.has_value(), data.characterMotion.has_value(),
                              true};
            static_assert(deterministic.size() == names.size());
            auto boundaries = Json::object();
            for (std::size_t index = 0; index < names.size(); ++index) {
                boundaries[std::string{names[index]}] = deterministic[index] ? "deterministic" : "omitted";
                if (deterministic[index])
                    recorded.push_back(names[index]);
            }
            return {{"version", AI::AiReplayManifest::CurrentVersion}, {"boundaries", std::move(boundaries)}};
        }

        /** @brief Encodes stable authored keys and copied solver outputs in canonical evidence order. */
        Json EncodeCharacterMotion(std::span<const HeadlessReplayCharacterMotion> motion) {
            auto records = Json::array();
            for (const auto &record : motion) {
                const auto &position = record.position;
                const auto &heading = record.heading;
                const auto &velocity = record.achievedVelocity;
                records.push_back({{"objectId", std::to_string(record.object.value)},
                                   {"componentId", std::to_string(record.component.value)},
                                   {"characterTick", std::to_string(record.characterTick)},
                                   {"positionMeters", {position.x, position.y, position.z}},
                                   {"heading", {heading.x, heading.y, heading.z, heading.w}},
                                   {"velocityMetersPerSecond", {velocity.x, velocity.y, velocity.z}},
                                   {"grounded", record.grounded},
                                   {"termination", static_cast<std::uint8_t>(record.termination)}});
            }
            return records;
        }

        /** @brief Encodes portable observations copied from the exact native body publication. */
        Json EncodeRigidBodies(std::span<const HeadlessReplayRigidBody> records) {
            auto bodies = Json::array();
            for (const auto &body : records) {
                const auto &position = body.pose.translation;
                const auto &rotation = body.pose.rotation;
                const auto &linear = body.linearVelocity;
                const auto &angular = body.angularVelocity;
                bodies.push_back({{"objectId", std::to_string(body.object.value)},
                                  {"bodySlot", std::to_string(body.slot.value)},
                                  {"positionMeters", {position.x, position.y, position.z}},
                                  {"rotation", {rotation.x, rotation.y, rotation.z, rotation.w}},
                                  {"linearVelocityMetersPerSecond", {linear.x, linear.y, linear.z}},
                                  {"angularVelocityRadiansPerSecond", {angular.x, angular.y, angular.z}},
                                  {"activity", static_cast<std::uint8_t>(body.activity)}});
            }
            return bodies;
        }

        /** @brief Adds provenance and explicit omissions to the portable body observations. */
        Json EncodePhysics(const HeadlessReplayPhysics &physics) {
            auto bodies = EncodeRigidBodies(physics.bodies);
            return {{"schemaVersion", RigidBodyMotionSchemaVersion},
                    {"scope", "authored-rigid-body-motion"},
                    {"omissions", {"solver-internal-state", "solver-build-identity", "external-physics-inputs"}},
                    {"physicsTick", std::to_string(physics.publication.completedTick)},
                    {"publicationRevision", std::to_string(physics.publication.publicationRevision)},
                    {"settingsDigest", FormatSha256(physics.settings.digest)},
                    {"digest", FormatSha256(HashRecords(bodies))},
                    {"bodies", std::move(bodies)}};
        }

        /** @brief Projects the stable portion of one original v4 trace record. */
        Json EncodeRecord(const AI::DecisionStepRecord &record) {
            return Json{{"kind", static_cast<std::uint8_t>(record.kind)},
                        {"graphId", std::to_string(record.occurrence.graph.Value())},
                        {"callerId", std::to_string(record.occurrence.caller.Value())},
                        {"occurrence", record.occurrence.occurrence},
                        {"parentOccurrence", record.occurrence.parent},
                        {"nodeId", std::to_string(record.node.Value())},
                        {"taskId", std::to_string(record.task.Value())},
                        {"hasActiveTaskHandle", record.handle.IsValid()},
                        {"state", static_cast<std::uint8_t>(record.state)},
                        {"transitionId", std::to_string(record.lastTransition.Value())},
                        {"transitionOrdinalOrCount", record.transitionCount},
                        {"keyId", std::to_string(record.key.Value())},
                        {"utilityScore", record.utilityScore ? Json(*record.utilityScore) : Json(nullptr)}};
        }

        /** @brief Produces a versioned neutral report using only committed, bounded owner evidence. */
        Result<std::string> EncodeEvidence(const HeadlessDecisionReplayData &data, const Sha256Digest &canonicalDigest) {
            const auto &capture = data.evidence;
            if (!capture.trace || capture.trace->DroppedRecords() != 0 || !data.definition.IsValid() || !capture.subject.IsValid() ||
                data.definitionRevision.value == 0 || data.sceneInput == Sha256Digest{} || data.canonicalRecord.empty() ||
                data.canonicalRecord.size() > HeadlessDecisionReplay::CanonicalBudget || (data.characterMotion && !data.physics))
                return Result<std::string>::Failure(MakeError(AI::AIErrors::CanonicalStateInvalid));
            const auto &header = capture.trace->Header();
            if (!header.MatchesObservation(header.planDigest, capture.tick, capture.blackboard, capture.blackboardRevision))
                return Result<std::string>::Failure(MakeError(AI::AIErrors::CanonicalStateInvalid));
            try {
                auto records = Json::array();
                for (const auto &record : capture.trace->Records())
                    records.push_back(EncodeRecord(record));
                auto recorded = Json::array();
                auto coverage = EncodeCoverage(data, recorded);
                Json payload{{"format", "horo-ai-replay-evidence"},
                             {"version", 1},
                             {"sceneDefinitionId", std::to_string(data.definition.value)},
                             {"agentId", std::to_string(capture.subject.Value())},
                             {"tick", std::to_string(capture.tick)},
                             {"recordedBoundaries", std::move(recorded)},
                             {"manifest", std::move(coverage)},
                             {"sceneInput",
                              {{"revision", std::to_string(data.definitionRevision.value)}, {"digest", FormatSha256(data.sceneInput)}}},
                             {"canonicalAi",
                              {{"recordVersion", AI::AiCanonicalReferenceRecordVersion},
                               {"sceneStructureRevision", std::to_string(capture.sceneStructureRevision)},
                               {"digest", FormatSha256(canonicalDigest)}}},
                             {"decision",
                              {{"traceVersion", header.version},
                               {"rootGraphId", std::to_string(header.root.Value())},
                               {"planDigest", FormatSha256(header.planDigest)},
                               {"outcome", static_cast<std::uint8_t>(header.outcome)},
                               {"blackboard",
                                {{"schemaId", std::to_string(capture.blackboard.schema.Value())},
                                 {"schemaVersion", capture.blackboard.schemaVersion},
                                 {"schemaGeneration", std::to_string(capture.blackboard.schemaGeneration)},
                                 {"instanceGeneration", capture.blackboard.instanceGeneration},
                                 {"revision", std::to_string(capture.blackboardRevision)}}},
                               {"records", std::move(records)}}}};
                if (data.physics)
                    payload["physics"] = EncodePhysics(*data.physics);
                if (data.characterMotion) {
                    auto motion = EncodeCharacterMotion(*data.characterMotion);
                    payload["characterMotion"] = {{"schemaVersion", CharacterMotionSchemaVersion},
                                                  {"settingsDigest", FormatSha256(data.physics->characterSettings.digest)},
                                                  {"digest", FormatSha256(HashRecords(motion))},
                                                  {"records", std::move(motion)}};
                }
                auto text = payload.dump();
                if (text.size() >= MaximumHeadlessReplayEvidenceBytes)
                    return Result<std::string>::Failure(MakeError(AI::AIErrors::DecisionAssetStorageUnavailable));
                text.push_back('\n');
                return Result<std::string>::Success(std::move(text));
            } catch (const std::bad_alloc &) {
                return Result<std::string>::Failure(MakeError(AI::AIErrors::DecisionAssetStorageUnavailable));
            }
        }

        /** @brief Product projection onto the shared typed replay comparer. The original v4 trace is borrowed. */
        struct ComparableCapture final {
            AI::AiReplayManifest manifest;
            AI::AiReplayTick tick;

            void Observe(AI::AiReplayBoundary boundary, std::uint32_t schema, const Sha256Digest &profile, AI::AiReplayStamp stamp,
                         const Sha256Digest &state) noexcept {
                const auto index = static_cast<std::size_t>(boundary);
                manifest.boundaries[index] = {AI::AiReplayCoverage::Deterministic, schema, profile};
                tick.observations[index] = AI::AiReplayObservation{stamp, state};
            }
        };

        /** @brief Adapts the captured product evidence without synthesizing missing observations. */
        Result<ComparableCapture> ProjectComparison(const HeadlessDecisionReplayData &data) {
            if (!data.evidence.trace || data.canonicalRecord.empty() ||
                data.canonicalRecord.size() > HeadlessDecisionReplay::CanonicalBudget || (data.characterMotion && !data.physics))
                return Result<ComparableCapture>::Failure(MakeError(AI::AIErrors::CanonicalStateInvalid));

            using enum AI::AiReplayBoundary;
            ComparableCapture result;
            result.tick.tick = data.evidence.tick;
            result.tick.decision = data.evidence.Decision();
            result.manifest.decisionSubject = data.evidence.subject;
            const auto &decision = data.evidence.trace->Header();
            result.manifest.boundaries[static_cast<std::size_t>(Decision)] = {AI::AiReplayCoverage::Deterministic, decision.version,
                                                                              decision.planDigest};
            result.Observe(SceneInput, SceneInputSchemaVersion, data.sceneInput, {data.definition.value, data.definitionRevision.value},
                           data.sceneInput);
            result.Observe(CanonicalAi, AI::AiCanonicalReferenceRecordVersion, data.sceneInput,
                           {data.definition.value, data.evidence.sceneStructureRevision},
                           ComputeSha256(std::as_bytes(std::span{data.canonicalRecord})));
            if (const auto &physics = data.physics) {
                const AI::AiReplayStamp stamp{physics->publication.completedTick, physics->publication.publicationRevision};
                result.Observe(Physics, RigidBodyMotionSchemaVersion, physics->settings.digest, stamp,
                               HashRecords(EncodeRigidBodies(physics->bodies)));
                if (data.characterMotion)
                    result.Observe(AchievedCharacterMotion, CharacterMotionSchemaVersion, physics->characterSettings.digest, stamp,
                                   HashRecords(EncodeCharacterMotion(*data.characterMotion)));
            }
            return Result<ComparableCapture>::Success(std::move(result));
        }

        /** @brief Removes only its exclusively created unpublished files and directory. */
        class EvidenceStaging final {
        public:
            explicit EvidenceStaging(std::filesystem::path directory)
                : directory(std::move(directory)), file(this->directory / "evidence.json") {}

            EvidenceStaging(const EvidenceStaging &) = delete;
            EvidenceStaging &operator=(const EvidenceStaging &) = delete;

            ~EvidenceStaging() {
                if (!owned || receipt.WasCommitted())
                    return;
                std::error_code ignored;
                if (fileCreated)
                    std::filesystem::remove(file, ignored);
                std::filesystem::remove(directory, ignored);
            }

            const std::filesystem::path directory;
            const std::filesystem::path file;
            bool owned{};
            bool fileCreated{};
            AtomicFileReplacementReceipt receipt;
        };

        /** @brief Stages and commits under the existing owner-controlled destination parent. */
        Result<void> PublishDirectory(const std::filesystem::path &destination, const std::string &text) {
            const auto parent = destination.parent_path();
            std::error_code failure;
            if (std::filesystem::canonical(parent, failure) != parent || failure)
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                                       "AI replay output requires an existing canonical directory parent."));
            NativeDurableFileSystem files;
            auto occupied = files.InspectEntry(destination);
            if (occupied.HasError())
                return Result<void>::Failure(std::move(occupied).ErrorValue());
            if (occupied.Value())
                return Result<void>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeInvalid, "AI replay output directory already exists."));

            std::array<std::byte, 16> nonce{};
            if (auto filled = Platform::CreateNativeSecureRandomSource()->Fill(nonce); filled.HasError())
                return filled;
            EvidenceStaging staged{parent / (".horo-replay-" + FormatSha256(ComputeSha256(nonce)).substr(7, 24))};
            auto created = files.CreateDirectoryPath(staged.directory, DirectoryCreationMode::Single);
            if (created.HasError())
                return Result<void>::Failure(std::move(created).ErrorValue());
            if (!created.Value())
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeUnavailable, "AI replay staging is occupied."));
            staged.owned = true;
            if (auto written = files.WritePrivateDurable(staged.file, std::as_bytes(std::span{text}), staged.fileCreated);
                written.HasError())
                return written;

            auto published = files.PublishDirectoryNoReplaceTracked(staged.directory, destination, staged.receipt);
            if (!staged.receipt.WasCommitted()) {
                if (published.HasError())
                    return published;
                return Result<void>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeUnavailable, "AI replay output directory was not committed."));
            }
            if (published.HasError())
                return Result<void>::Failure(WrapError(Network::NetworkErrors::NetworkModeUnavailable, published.ErrorValue(),
                                                       "AI replay output directory was published but durability could not be confirmed."));
            return Result<void>::Success();
        }
    }  // namespace

    HeadlessReplayOutput::HeadlessReplayOutput(std::filesystem::path directory) noexcept : directory_(std::move(directory)) {}

    /** @copydoc HeadlessReplayOutput::Parse */
    Result<HeadlessReplayOutput> HeadlessReplayOutput::Parse(const std::string_view text) {
        using Outcome = Result<HeadlessReplayOutput>;
        if (!text.empty() && text.find('\0') == std::string_view::npos && IsValidUtf8ScalarSequence(text)) {
            const std::filesystem::path directory{std::u8string{text.begin(), text.end()}};
            if (directory.is_absolute() && !directory.filename().empty() && directory.lexically_normal() == directory)
                return Outcome::Success(HeadlessReplayOutput{directory});
        }
        return Outcome::Failure(
            MakeError(Network::NetworkErrors::NetworkModeInvalid, "Use --ai-replay-output <absolute normalized new directory> once."));
    }

    /** @copydoc PublishHeadlessReplayArtifact */
    Result<void> PublishHeadlessReplayArtifact(const HeadlessReplayOutput &output, const HeadlessDecisionReplayData &data,
                                               const Sha256Digest &canonicalDigest) {
        auto evidence = EncodeEvidence(data, canonicalDigest);
        if (evidence.HasError())
            return Result<void>::Failure(std::move(evidence).ErrorValue());
        return PublishDirectory(output.Directory(), evidence.Value());
    }

    /** @copydoc CompareHeadlessReplayCaptures */
    Result<std::optional<AI::AiReplayDivergence>> CompareHeadlessReplayCaptures(const HeadlessDecisionReplayData &recorded,
                                                                                const HeadlessDecisionReplayData &replayed) {
        using Comparison = Result<std::optional<AI::AiReplayDivergence>>;
        try {
            const auto baseline = ProjectComparison(recorded);
            if (baseline.HasError())
                return Comparison::Failure(baseline.ErrorValue());
            const auto candidate = ProjectComparison(replayed);
            if (candidate.HasError())
                return Comparison::Failure(candidate.ErrorValue());
            return AI::CompareAiReplayEvidence(baseline.Value().manifest, std::span{&baseline.Value().tick, 1U}, candidate.Value().manifest,
                                               std::span{&candidate.Value().tick, 1U});
        } catch (const std::bad_alloc &) {
            return Comparison::Failure(MakeError(AI::AIErrors::DecisionAssetStorageUnavailable));
        }
    }
}  // namespace Horo::Application::Internal
