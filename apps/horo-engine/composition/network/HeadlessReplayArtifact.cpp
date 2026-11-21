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
        constexpr std::size_t MaximumEvidenceBytes = 128U * 1024U;

        /** @brief Describes supported and omitted boundaries even when trace records were captured for diagnostics. */
        Json EncodeCoverage() {
            constexpr auto names =
                std::to_array<std::string_view>({"scene-input", "perception-input", "query-publication", "blackboard-sync", "decision",
                                                 "navigation", "gameplay", "physics", "achieved-character-motion", "canonical-ai"});
            static_assert(names.size() == AI::AiReplayBoundaryCount);
            auto boundaries = Json::object();
            for (const auto name : names)
                boundaries[std::string{name}] = "omitted";
            boundaries["scene-input"] = "deterministic";
            boundaries["decision"] = "deterministic";
            boundaries["canonical-ai"] = "deterministic";
            return {{"version", AI::AiReplayManifest::CurrentVersion}, {"boundaries", std::move(boundaries)}};
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
                data.canonicalRecord.size() > HeadlessDecisionReplay::CanonicalBudget)
                return Result<std::string>::Failure(MakeError(AI::AIErrors::CanonicalStateInvalid));
            const auto &header = capture.trace->Header();
            if (!header.MatchesObservation(header.planDigest, capture.tick, capture.blackboard, capture.blackboardRevision))
                return Result<std::string>::Failure(MakeError(AI::AIErrors::CanonicalStateInvalid));
            try {
                auto records = Json::array();
                for (const auto &record : capture.trace->Records())
                    records.push_back(EncodeRecord(record));
                const Json payload{{"format", "horo-ai-replay-evidence"},
                                   {"version", 1},
                                   {"sceneDefinitionId", std::to_string(data.definition.value)},
                                   {"agentId", std::to_string(capture.subject.Value())},
                                   {"tick", std::to_string(capture.tick)},
                                   {"recordedBoundaries", {"scene-input", "decision", "canonical-ai"}},
                                   {"manifest", EncodeCoverage()},
                                   {"sceneInput",
                                    {{"revision", std::to_string(data.definitionRevision.value)},
                                     {"digest", FormatSha256(data.sceneInput)}}},
                                   {"canonicalAi",
                                    {{"recordVersion", AI::AiCanonicalReferenceRecordVersion}, {"digest", FormatSha256(canonicalDigest)}}},
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
                auto text = payload.dump();
                if (text.size() >= MaximumEvidenceBytes)
                    return Result<std::string>::Failure(MakeError(AI::AIErrors::DecisionAssetStorageUnavailable));
                text.push_back('\n');
                return Result<std::string>::Success(std::move(text));
            } catch (const std::bad_alloc &) {
                return Result<std::string>::Failure(MakeError(AI::AIErrors::DecisionAssetStorageUnavailable));
            }
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
}  // namespace Horo::Application::Internal
