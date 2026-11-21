#pragma once

/** @file HeadlessReplayTimeline.h @brief Bounded, value-filtered projection of admitted portable replay frames. */

#include "HeadlessReplayArtifact.h"

#include <nlohmann/json.hpp>

namespace Horo::Application::Internal {
    /** @brief The timeline borrows only long enough to copy declared identities and observations. */
    class HeadlessReplayTimeline final {
    public:
        static constexpr std::size_t MaximumEvents = 4096;
        static constexpr std::size_t DefaultEvents = 512;

        explicit HeadlessReplayTimeline(std::size_t limit);

        /** @brief Appends events for one admitted frame in fixed boundary and source order. */
        void Append(const nlohmann::json &frame, std::uint64_t tick, std::uint64_t agent);

        /** @brief Returns the neutral export with an exact count of events beyond capacity. */
        [[nodiscard]] nlohmann::json Finish() &&;

    private:
        void Add(std::string_view boundary, std::size_t ordinal, nlohmann::json fields);

        nlohmann::json events_ = nlohmann::json::array();
        std::size_t limit_{};
        std::size_t dropped_{};
        std::string tick_;
        std::string agent_;
    };

    /** @brief Internal sentinel for malformed portable evidence, with no input payload in its diagnostic. */
    struct InvalidHeadlessReplayEvidence final {};

    /** @brief Parses a canonical unsigned decimal identity without copying caller-provided text into an export. */
    std::uint64_t ReadHeadlessReplayWord(const nlohmann::json &field);

    /** @brief Validates and decodes a nonzero portable digest before it is used as evidence. */
    Sha256Digest ReadHeadlessReplayDigest(const nlohmann::json &field);
}  // namespace Horo::Application::Internal
