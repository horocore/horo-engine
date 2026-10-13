#include "Horo/PCG/PCGCookedPlan.h"
#include "Horo/PCG/PCGErrors.h"
#include "Horo/PCG/PCGPointFiltering.h"
#include "PCGPointFilteringInternal.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <vector>

namespace Horo::PCG {
    namespace {
        /** @brief Asserts the exact codec error without accepting coerced or partial payloads. */
        void CheckInvalid(const Result<PCGPointFilterSettings> &result) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == PCGErrors::CpuEvaluationInvalid.code.Value());
        }
    }  // namespace

    TEST_CASE("PCG attribute codecs preserve golden version-one bytes for all seven exact value tags", "[unit][pcg][filters]") {
        const std::array<PCGAttributePredicateValue, 7> values{true,
                                                               std::numeric_limits<std::int64_t>::min(),
                                                               std::numeric_limits<std::uint64_t>::max(),
                                                               1.0,
                                                               Math::Vec2{1, -0.0F},
                                                               Math::Vec3{1, 0, -0.0F},
                                                               Math::Vec4{1, 0, 0, -0.0F}};
        const std::array<std::vector<std::uint8_t>, 7> words{{{1},
                                                              {0x80, 0, 0, 0, 0, 0, 0, 0},
                                                              {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
                                                              {0x3f, 0xf0, 0, 0, 0, 0, 0, 0},
                                                              {0x3f, 0x80, 0, 0, 0, 0, 0, 0},
                                                              {0x3f, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
                                                              {0x3f, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}};
        const auto key = PCGAttributeKey::Create("test.value").Value();
        for (std::size_t index = 0; index < values.size(); ++index) {
            std::vector<std::uint8_t>
                expected{0, 0, 0, 1, 10, 't', 'e', 's', 't', '.', 'v', 'a', 'l', 'u', 'e', 0, static_cast<std::uint8_t>(index)};
            expected.insert(expected.end(), words[index].begin(), words[index].end());
            const auto encoded =
                EncodePCGPointFilterSettings(PCGAttributeFilterSettings{key, PCGAttributeComparison::Equal, values[index]});
            REQUIRE(encoded.HasValue());
            CHECK(encoded.Value() == expected);
            PCGCookedNode node;
            node.type = PCGCpuNodeType(PCGCpuNodeKind::AttributeFilter).Value();
            node.payload = expected;
            const auto decoded = detail::DecodePointFilter(node);
            REQUIRE(decoded.HasValue());
            CHECK(std::get<PCGAttributeFilterSettings>(decoded.Value()).value.index() == index);
            const auto roundTrip = EncodePCGPointFilterSettings(decoded.Value());
            REQUIRE(roundTrip.HasValue());
            CHECK(roundTrip.Value() == expected);
        }
    }

    TEST_CASE("PCG filter codec rejects truncated and trailing envelopes for every settings kind", "[unit][pcg][filters]") {
        using enum PCGCpuNodeKind;
        constexpr std::array kinds{DensityRangeFilter, BoundsFilter, SlopeFilter, DistanceFilter, AttributeFilter};
        const std::array<PCGPointFilterSettings, 5> settings{PCGDensityRangeFilterSettings{0, 1},
                                                             PCGBoundsFilterSettings{{{0, 0, 0}, {1, 1, 1}}},
                                                             PCGSlopeFilterSettings{0, 180}, PCGDistanceFilterSettings{1},
                                                             PCGAttributeFilterSettings{PCGAttributeKey::Create("test.boolean").Value(),
                                                                                        PCGAttributeComparison::Equal, true}};
        for (std::size_t index = 0; index < kinds.size(); ++index) {
            const auto encoded = EncodePCGPointFilterSettings(settings[index]);
            REQUIRE(encoded.HasValue());
            PCGCookedNode node;
            node.type = PCGCpuNodeType(kinds[index]).Value();
            node.payload = encoded.Value();
            REQUIRE(detail::DecodePointFilter(node).HasValue());
            for (std::size_t size = 0; size < encoded.Value().size(); ++size) {
                node.payload.assign(encoded.Value().begin(), encoded.Value().begin() + static_cast<std::ptrdiff_t>(size));
                CheckInvalid(detail::DecodePointFilter(node));
            }
            node.payload = encoded.Value();
            node.payload.push_back(0);
            CheckInvalid(detail::DecodePointFilter(node));
        }
    }

}  // namespace Horo::PCG
