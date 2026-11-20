#pragma once

/** @file HeadlessReplayArtifact.h @brief Explicit terminal output for detached gameplay AI replay evidence. */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"

#include <filesystem>
#include <string_view>

namespace Horo::Application::Internal {
    struct HeadlessDecisionReplayData;

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
}  // namespace Horo::Application::Internal
