#pragma once

#include "Horo/Cinematic/SequenceAsset.h"

#include <catch2/catch_test_macros.hpp>
#include <string>

namespace SequenceDocumentTests {
    inline std::string Source() {
        return R"({"schemaVersion":{"major":1,"minor":0},
            "assetId":"00000000-0000-0000-0000-000000000001",
            "sequenceId":{"stableValue":1,"generation":1},"name":"Başlangıç sequence",
            "durationFrames":240,"frameRate":{"numerator":24,"denominator":1},
            "playback":{"loopMode":"once","clockSource":"committedSimulation","pausePolicy":"followGameplay","dilationPolicy":"sourceNative"},
            "tracks":[{"id":{"stableValue":10,"generation":1},"type":"transform","keyframeCount":2,"references":[]},
                      {"id":{"stableValue":11,"generation":1},"type":"property","keyframeCount":1,"references":[]},
                      {"id":{"stableValue":12,"generation":1},"type":"cameraCut","keyframeCount":1,"references":[]},
                      {"id":{"stableValue":13,"generation":1},"type":"event","keyframeCount":1,"references":[]}]})";
    }

    inline Horo::Cinematic::SequenceAsset Asset() {
        auto parsed = Horo::Cinematic::ParseSequenceAsset(Source());
        REQUIRE(parsed.HasValue());
        return std::move(parsed).Value();
    }
}  // namespace SequenceDocumentTests
