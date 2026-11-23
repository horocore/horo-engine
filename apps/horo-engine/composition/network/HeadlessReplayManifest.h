#pragma once

/** @file HeadlessReplayManifest.h @brief Portable replay coverage and input disclosure shared by writer and reader. */

#include "HeadlessReplayArtifact.h"

#include <nlohmann/json.hpp>

namespace Horo::Application::Internal {
    using HeadlessReplayCoverage = std::array<AI::AiReplayCoverage, AI::AiReplayBoundaryCount>;

    struct HeadlessReplayManifestJson final {
        nlohmann::json manifest;
        nlohmann::json recordedBoundaries;
    };

    /** @brief Describes the observed scope and exact limitations of the portable standalone replay profile. */
    inline HeadlessReplayManifestJson EncodeHeadlessReplayManifest(const HeadlessReplayCoverage &coverage) {
        using Json = nlohmann::json;
        using enum AI::AiReplayCoverage;
        auto boundaries = Json::object();
        auto recorded = Json::array();
        for (std::size_t index = 0; index < coverage.size(); ++index) {
            const auto name = HeadlessReplayBoundaryNames[index];
            const auto mode = coverage[index];
            boundaries[std::string{name}] = mode == Nondeterministic ? "nondeterministic"
                                            : mode == Deterministic  ? "deterministic"
                                                                     : "omitted";
            if (mode != Omitted)
                recorded.push_back(name);
        }

        const auto includes = [&](const AI::AiReplayBoundary boundary) {
            return coverage[static_cast<std::size_t>(boundary)] != Omitted;
        };
        using enum AI::AiReplayBoundary;
        auto providers = Json::object();
        if (coverage[static_cast<std::size_t>(Navigation)] == Nondeterministic)
            providers["navigation"] = {"native-navigation-path-worker"};
        Json omissions = {{"scene-input", {"streamed-cell-content", "runtime-structural-group-content"}}};
        if (includes(PerceptionInput))
            omissions["perception-input"] = {"provider-output", "provider-internal-state"};
        if (includes(Gameplay))
            omissions["gameplay"] = {"private-module-state", "external-gameplay-inputs", "deferred-scene-publication"};
        if (includes(Physics))
            omissions["physics"] = {"solver-internal-state", "external-physics-inputs"};
        return {{{"version", 2},
                 {"boundaries", std::move(boundaries)},
                 {"nondeterministicProviders", std::move(providers)},
                 {"omittedInputsAndState", std::move(omissions)}},
                std::move(recorded)};
    }
}  // namespace Horo::Application::Internal
