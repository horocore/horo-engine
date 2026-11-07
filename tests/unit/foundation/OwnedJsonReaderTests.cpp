#include "AllocationProbe.h"
#include "OwnedJsonReader.h"
#include "SceneAllocationQualification.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

namespace {
    using Owned = Horo::JsonEncoding::Detail::OwnedJsonValue;
    using Horo::JsonEncoding::Detail::ReadOwnedJson;

    /** @brief Multiple siblings/nested containers exercise staging growth before any final structured-child publication. */
    std::string Source() {
        std::string bytes = R"({"z":[{"unsigned":18446744073709551615,"signed":-9223372036854775808,"float":1.0,"zero":-0.0},)";
        bytes += R"("quoted\" UTF-8: ç",false,null],"a":{"long-owned-key-which-allocates":[)";
        for (std::size_t index = 0; index < 48; ++index) {
            if (index)
                bytes += ',';
            bytes += R"({"nested-long-owned-property":{"items":[1,2.0,"long owned scalar value with storage"]}})";
        }
        bytes += "]}}";
        return bytes;
    }

    /** @brief Every actual allocation point, including normal cleanup, must unwind without touching the accepted destination. */
    void RejectEveryAllocation(const std::string &bytes, bool valid) {
        {
            const auto warm = ReadOwnedJson(bytes, 1U << 20U, 32);
            REQUIRE(warm.has_value() == valid);
        }
        Horo::Tests::AllocationProbe::Measurement measured;
        bool accepted{};
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
            {
                auto candidate = ReadOwnedJson(bytes, 1U << 20U, 32);
                accepted = candidate.has_value();
            }
            measured = measurement.Snapshot();
        }
        REQUIRE(accepted == valid);
        REQUIRE(measured.requests > 0);
        const auto input = bytes;
        for (std::size_t index = 0; index < measured.requests; ++index) {
            INFO(index);
            auto destination = ReadOwnedJson(R"({"accepted":[{"state":"preserved"}]})", 1024, 32);
            REQUIRE(destination.has_value());
            const auto previous = destination->Get().dump();
            bool failed{};
            {
                Horo::Tests::AllocationProbe::ScopedFailure failure{index};
                try {
                    auto candidate = ReadOwnedJson(bytes, 1U << 20U, 32);
                    if (candidate)
                        destination->Get().swap(candidate->Get());
                } catch (const std::bad_alloc &) {
                    failed = true;
                }
            }
            REQUIRE(failed);
            CHECK(destination->Get().dump() == previous);
            CHECK(bytes == input);
        }
    }
}  // namespace

TEST_CASE("Owned JSON reader preserves pinned DOM types scalar values and array event order", "[foundation][json][owned]") {
    const auto source = Source();
    Owned expected{nlohmann::json::parse(source)};
    auto actual = ReadOwnedJson(source, 1U << 20U, 32);
    REQUIRE(actual.has_value());
    CHECK(actual->Get().dump() == expected.Get().dump());
    const auto &scalars = actual->Get()["z"][0];
    CHECK(scalars["unsigned"].is_number_unsigned());
    CHECK(scalars["unsigned"].get<std::uint64_t>() == std::numeric_limits<std::uint64_t>::max());
    CHECK(scalars["signed"].is_number_integer());
    CHECK(scalars["signed"].get<std::int64_t>() == std::numeric_limits<std::int64_t>::min());
    CHECK(scalars["float"].is_number_float());
    CHECK(std::signbit(scalars["zero"].get<double>()));
    CHECK(actual->Get()["z"][1] == "quoted\" UTF-8: ç");
    CHECK(actual->Get()["a"]["long-owned-key-which-allocates"].size() == 48);
}

TEST_CASE("Owned JSON reader unwinds every partial valid construction and malformed validation allocation",
          "[foundation][json][allocation]") {
    Horo::Tests::RequireSupportedSceneAllocationInjection();
    const auto valid = Source();
    RejectEveryAllocation(valid, true);
    auto malformed = valid;
    malformed.pop_back();
    RejectEveryAllocation(malformed, false);
}

TEST_CASE("Owned JSON reader retains strict syntax duplicate depth and byte admission", "[foundation][json][owned]") {
    CHECK_FALSE(ReadOwnedJson(R"({"a":1,"a":2})", 1024, 32).has_value());
    CHECK_FALSE(ReadOwnedJson("[1,]", 1024, 32).has_value());
    constexpr std::string_view unpairedSurrogate = "\"\\ud800\"";
    CHECK_FALSE(ReadOwnedJson(unpairedSurrogate, 1024, 32).has_value());
    CHECK_FALSE(ReadOwnedJson("{} trailing", 1024, 32).has_value());
    CHECK_FALSE(ReadOwnedJson("{}", 1, 32).has_value());
    CHECK_FALSE(ReadOwnedJson("{}", 1024, 0).has_value());
    std::string deep(64, '[');
    deep += "0";
    deep.append(64, ']');
    CHECK_FALSE(ReadOwnedJson(deep, 1024, 32).has_value());
}

TEST_CASE("JSON guard no-DOM validation preserves original global Parse exception behavior", "[foundation][json][guard]") {
    const std::string malformed = R"({"nested":[1,2,]})";
    Horo::Foundation::JsonParseGuard guard{32};
    CHECK_FALSE(guard.Validate(malformed.begin(), malformed.end()));
    Horo::Foundation::JsonParseGuard original{32};
    CHECK_THROWS_AS(original.Parse(malformed.begin(), malformed.end()), nlohmann::json::parse_error);
    const std::string duplicate = R"({"a":1,"a":2})";
    Horo::Foundation::JsonParseGuard duplicateGuard{32};
    CHECK_FALSE(duplicateGuard.Validate(duplicate.begin(), duplicate.end()));
    CHECK(duplicateGuard.HasDuplicate());
}

TEST_CASE("Owned parsed JSON retirement allocates nothing with the next allocation armed", "[foundation][json][allocation]") {
    Horo::Tests::RequireSupportedSceneAllocationInjection();
    auto owned = ReadOwnedJson(Source(), 1U << 20U, 32);
    REQUIRE(owned.has_value());
    Horo::Tests::AllocationProbe::Measurement measured;
    {
        Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
        Horo::Tests::AllocationProbe::ScopedFailure failure{0};
        owned.reset();
        measured = measurement.Snapshot();
    }
    CHECK(measured.requests == 0);
}
