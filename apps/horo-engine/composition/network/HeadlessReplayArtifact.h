#pragma once

/** @file HeadlessReplayArtifact.h @brief Explicit terminal output for detached gameplay AI replay evidence. */

#include "Horo/AI/AIReplayEvidence.h"

#include <filesystem>
#include <iosfwd>
#include <string_view>

namespace Horo::Application::Internal {
    struct HeadlessDecisionReplayData;

    inline constexpr auto HeadlessReplayBoundaryNames =
        std::to_array<std::string_view>({"scene-input", "perception-input", "query-publication", "blackboard-sync", "decision",
                                         "navigation", "gameplay", "physics", "achieved-character-motion", "canonical-ai"});
    static_assert(HeadlessReplayBoundaryNames.size() == AI::AiReplayBoundaryCount);
    inline constexpr std::size_t MaximumHeadlessReplayEvidenceBytes = 128U * 1024U;

    /** @brief User-selected absolute directory name for one immutable evidence publication. */
    class HeadlessReplayOutput final {
    public:
        /** @brief Parses a nonempty absolute normalized UTF-8 path without touching the filesystem.
         * @param text Explicit argument bytes. @return Typed destination or a launch option error. */
        [[nodiscard]] static Result<HeadlessReplayOutput> Parse(std::string_view text);

        /** @brief Returns the exact admitted spelling for publication. */
        [[nodiscard]] const std::filesystem::path &Directory() const noexcept {
            return directory_;
        }

    private:
        explicit HeadlessReplayOutput(std::filesystem::path directory) noexcept;
        std::filesystem::path directory_;
    };

    /** @brief Publishes bounded neutral evidence after the frame loop using an atomic no-replace directory commit.
     * @param output Exact new directory under an existing caller-controlled parent.
     * @param data Detached committed decision and canonical AI state.
     * @param canonicalDigest SHA-256 of the bounded canonical AI byte record.
     * @return Durable success, a typed prepublication failure, or a typed postcommit durability error.
     * @details The output contains only evidence metadata and neutral trace records. The parent namespace must stay caller-controlled
     * throughout publication. On a postcommit error the published directory remains intact. */
    [[nodiscard]] Result<void> PublishHeadlessReplayArtifact(const HeadlessReplayOutput &output, const HeadlessDecisionReplayData &data,
                                                             const Sha256Digest &canonicalDigest);

    /** @brief Compares committed captures using their original decision evidence and owned publication identities.
     * @param recorded Baseline product capture. @param replayed Independently committed product capture.
     * @return First differing observed boundary, equality within admitted coverage, or typed profile/evidence failure.
     */
    [[nodiscard]] Result<std::optional<AI::AiReplayDivergence>> CompareHeadlessReplayCaptures(const HeadlessDecisionReplayData &recorded,
                                                                                              const HeadlessDecisionReplayData &replayed);

    /** @brief Compares two bounded portable evidence files without loading a project or running simulation. */
    [[nodiscard]] Result<std::optional<AI::AiReplayDivergence>> CompareHeadlessReplayArtifacts(const std::filesystem::path &recorded,
                                                                                               const std::filesystem::path &replayed);

    /** @brief Terminal view of the first differing tick and boundary, or typed evidence failure. */
    int RunHeadlessReplayComparison(std::span<char *> arguments, std::ostream &out, std::ostream &err);
}  // namespace Horo::Application::Internal
