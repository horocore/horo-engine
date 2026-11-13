#include "AllocationProbe.h"
#include "CanonicalJsonWriter.h"
#include "SceneAllocationQualification.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <optional>

namespace {
    using Ordered = Horo::JsonEncoding::Detail::CanonicalJsonValue<false>;
    using Sorted = Horo::JsonEncoding::Detail::CanonicalJsonValue<true>;

    /** @brief Exercises nested ownership, escaped UTF-8 strings and exact scalar alternatives in an intentionally ordered tree. */
    Ordered Source() {
        return Ordered{{"z", Ordered::array({Ordered{{"z", -0.0}, {"a", std::numeric_limits<std::uint64_t>::max()}}, "quoted\" UTF-8: ç",
                                             false, nullptr})},
                       {"a", Ordered{{"last", std::int64_t{-7}}, {"first", 1.0}}}};
    }

    /** @brief Proves every conversion allocation failure preserves the existing destination and complete source. */
    template <class Destination, class SourceType> void RejectEveryAllocation(const SourceType &source) {
        std::optional<Destination> prepared;
        Horo::Tests::AllocationProbe::Measurement measured;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
            prepared.emplace(source);
            measured = measurement.Snapshot();
        }
        REQUIRE(measured.requests > 0);
        const auto before = source.dump(0);
        for (std::size_t index = 0; index < measured.requests; ++index) {
            Destination destination{"unmodified destination with owned string storage"};
            const auto destinationBefore = destination.dump(0);
            bool failed{};
            {
                Horo::Tests::AllocationProbe::ScopedFailure failure{index};
                try {
                    destination = source;
                } catch (const std::bad_alloc &) {
                    failed = true;
                }
            }
            REQUIRE(failed);
            CHECK(source.dump(0) == before);
            CHECK(destination.dump(0) == destinationBefore);
        }
    }

    using Owned = Horo::JsonEncoding::Detail::OwnedJsonValue;

    /** @brief Exercises every real DOM bridge allocation while preserving an already owned destination and source. */
    template <class SourceType> void RejectEveryDomAllocation(const SourceType &source, const std::string &expected) {
        std::optional<Owned> prepared;
        Horo::Tests::AllocationProbe::Measurement measured;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
            prepared.emplace(source.ToOwnedJson());
            measured = measurement.Snapshot();
        }
        REQUIRE(measured.requests > 0);
        REQUIRE(prepared->Get().dump(0) == expected);
        const auto before = source.dump(0);
        for (std::size_t index = 0; index < measured.requests; ++index) {
            Owned destination{nlohmann::json(nlohmann::json::value_t::string)};
            destination.Get().get_ref<std::string &>() = "unmodified destination with owned string storage";
            const auto original = destination.Get().dump(0);
            bool failed = false;
            {
                Horo::Tests::AllocationProbe::ScopedFailure failure{index};
                try {
                    Owned candidate{source.ToOwnedJson()};
                    destination.Get().swap(candidate.Get());
                } catch (const std::bad_alloc &) {
                    failed = true;
                }
            }
            REQUIRE(failed);
            CHECK(destination.Get().dump(0) == original);
            CHECK(source.dump(0) == before);
        }
    }
}  // namespace

TEST_CASE("Canonical writer cross-policy copy sorts only the target and preserves scalar spelling", "[foundation][json][canonical]") {
    const auto ordered = Source();
    const auto original = ordered.dump(0);
    const Sorted sorted{ordered};
    const Sorted expected{{"a", Sorted{{"first", 1.0}, {"last", std::int64_t{-7}}}},
                          {"z", Sorted::array({Sorted{{"a", std::numeric_limits<std::uint64_t>::max()}, {"z", -0.0}}, "quoted\" UTF-8: ç",
                                               false, nullptr})}};
    CHECK(sorted.dump(0) == expected.dump(0));
    CHECK(ordered.dump(0) == original);
    const Ordered sortedOrder{sorted};
    CHECK(sortedOrder.dump(0) == sorted.dump(0));
    Sorted assigned{"prior"};
    assigned = ordered;
    CHECK(assigned.dump(0) == expected.dump(0));
    Ordered orderedAssigned{"prior"};
    orderedAssigned = sorted;
    CHECK(orderedAssigned.dump(0) == expected.dump(0));
}

TEST_CASE("Canonical writer cross-policy conversion unwinds every nested allocation safely", "[foundation][json][allocation]") {
    Horo::Tests::RequireSupportedSceneAllocationInjection();
    const auto ordered = Source();
    const Sorted sorted{ordered};
    RejectEveryAllocation<Sorted>(ordered);
    RejectEveryAllocation<Ordered>(sorted);
}

TEST_CASE("Canonical DOM bridge preserves exact values and unwinds every conversion allocation safely", "[foundation][json][allocation]") {
    Horo::Tests::RequireSupportedSceneAllocationInjection();
    const auto ordered = Source();
    const Sorted sorted{ordered};
    const auto expected = sorted.dump(0);
    const Owned legacy{ordered.ToJson()};
    CHECK(legacy.Get().dump(0) == expected);
    RejectEveryDomAllocation(ordered, expected);
    RejectEveryDomAllocation(sorted, expected);
}

TEST_CASE("Owned JSON move replacement and nested retirement allocate nothing even with the next allocation armed",
          "[foundation][json][allocation]") {
    Horo::Tests::RequireSupportedSceneAllocationInjection();
    const auto source = Source();
    Owned first{source.ToOwnedJson()};
    Owned second{source.ToOwnedJson()};
    Horo::Tests::AllocationProbe::Measurement measured;
    {
        Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
        {
            Horo::Tests::AllocationProbe::ScopedFailure failure{0};
            Owned moved{std::move(first)};
            second = std::move(moved);
            Owned retired{std::move(second)};
        }
        measured = measurement.Snapshot();
    }
    CHECK(measured.requests == 0);
    CHECK(first.Get().is_null());
    CHECK(second.Get().is_null());
}
