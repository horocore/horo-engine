#include "Horo/Foundation/Telemetry/Telemetry.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace {
    class NullSink final : public Horo::Telemetry::ISink {
    public:
        void Export(const Horo::Telemetry::Record &, const Horo::Telemetry::InstrumentDescriptor *) override {}

        void Flush() override {}
    };

    [[nodiscard]] Horo::Telemetry::InstrumentDescriptor MakeDescriptor(std::string name) {
        return {.name = std::move(name), .subsystem = "tests.metrics", .unit = Horo::Telemetry::MetricUnit::Count};
    }

    [[nodiscard]] bool Register(Horo::Telemetry::InstrumentDescriptor descriptor) {
        return static_cast<bool>(Horo::Telemetry::Runtime::RegisterCounter(std::move(descriptor)));
    }
}  // namespace

TEST_CASE("Metric descriptors enforce typed units and hard admission bounds", "[foundation][observability][telemetry][contract]") {
    using namespace Horo::Telemetry;
    Runtime::Shutdown();
    REQUIRE(Runtime::Initialize({.queueCapacity = 64, .enabled = true}, std::make_shared<NullSink>()));
    const std::uint64_t invalidBefore = Runtime::GetStatistics().invalidInstrumentRegistrations;

    auto maximum = MakeDescriptor(std::string(MaximumMetricNameBytes, 'a'));
    maximum.subsystem = std::string(MaximumMetricSubsystemBytes, 's');
    maximum.description = std::string(MaximumMetricDescriptionBytes, 'd');
    maximum.maxSeries = MaximumMetricSeries;
    for (std::size_t dimensionIndex = 0; dimensionIndex < MaximumMetricDimensions; ++dimensionIndex) {
        DimensionDescriptor dimension{.key =
                                          std::string(MaximumMetricDimensionKeyBytes - 1, 'd') + static_cast<char>('0' + dimensionIndex)};
        for (std::size_t valueIndex = 0; valueIndex < MaximumMetricDimensionValues; ++valueIndex)
            dimension.allowedValues.push_back(std::string(MaximumMetricDimensionValueBytes - 1, 'v') + static_cast<char>('a' + valueIndex));
        maximum.dimensions.push_back(std::move(dimension));
    }
    static_cast<void>(Runtime::RegisterCounter(std::move(maximum)));
    REQUIRE(Runtime::GetDiagnosticSnapshot().availabilityCount == 1);
    for (const auto &[name, unit] : std::array{
             std::pair{"valid.bytes", MetricUnit::Bytes},
             std::pair{"valid.seconds", MetricUnit::Seconds},
             std::pair{"valid.ratio", MetricUnit::Ratio},
         }) {
        auto descriptor = MakeDescriptor(name);
        descriptor.unit = unit;
        REQUIRE(Register(std::move(descriptor)));
    }

    const auto rejects = [](InstrumentDescriptor descriptor) {
        return !Register(std::move(descriptor));
    };
    auto longName = MakeDescriptor(std::string(MaximumMetricNameBytes + 1, 'n'));
    auto longSubsystem = MakeDescriptor("reject.subsystem");
    longSubsystem.subsystem = std::string(MaximumMetricSubsystemBytes + 1, 's');
    auto longDescription = MakeDescriptor("reject.description");
    longDescription.description = std::string(MaximumMetricDescriptionBytes + 1, 'd');
    auto invalidUnit = MakeDescriptor("reject.unit");
    invalidUnit.unit = static_cast<MetricUnit>(255);
    auto tooManySeries = MakeDescriptor("reject.series");
    tooManySeries.maxSeries = MaximumMetricSeries + 1;
    auto zeroSeries = MakeDescriptor("reject.zero_series");
    zeroSeries.maxSeries = 0;
    auto tooManyDimensions = MakeDescriptor("reject.dimensions");
    tooManyDimensions.dimensions.resize(MaximumMetricDimensions + 1, DimensionDescriptor{.key = "kind", .allowedValues = {"a"}});
    auto longDimensionKey = MakeDescriptor("reject.dimension_key");
    longDimensionKey.dimensions.push_back({.key = std::string(MaximumMetricDimensionKeyBytes + 1, 'k'), .allowedValues = {"value"}});
    auto tooManyValues = MakeDescriptor("reject.dimension_values");
    DimensionDescriptor tooManyAllowedValues{.key = "kind"};
    for (std::size_t index = 0; index <= MaximumMetricDimensionValues; ++index)
        tooManyAllowedValues.allowedValues.push_back("value" + std::to_string(index));
    tooManyValues.dimensions.push_back(std::move(tooManyAllowedValues));
    auto longDimensionValue = MakeDescriptor("reject.dimension_value");
    longDimensionValue.dimensions.push_back({.key = "kind", .allowedValues = {std::string(MaximumMetricDimensionValueBytes + 1, 'v')}});
    auto invalidTimingUnit = MakeDescriptor("reject.timing_unit");
    invalidTimingUnit.unit = static_cast<MetricUnit>(255);

    CHECK(rejects(std::move(longName)));
    CHECK(rejects(std::move(longSubsystem)));
    CHECK(rejects(std::move(longDescription)));
    CHECK(rejects(std::move(invalidUnit)));
    CHECK(rejects(std::move(tooManySeries)));
    CHECK(rejects(std::move(zeroSeries)));
    CHECK(rejects(std::move(tooManyDimensions)));
    CHECK(rejects(std::move(longDimensionKey)));
    CHECK(rejects(std::move(tooManyValues)));
    CHECK(rejects(std::move(longDimensionValue)));
    CHECK_FALSE(static_cast<bool>(Runtime::RegisterTiming(std::move(invalidTimingUnit))));

    const DiagnosticSnapshot snapshot = Runtime::GetDiagnosticSnapshot();
    CHECK(snapshot.availabilityCount == 4);
    CHECK(snapshot.statistics.invalidInstrumentRegistrations == invalidBefore + 11);
    CHECK(Runtime::Shutdown());
}

TEST_CASE("Telemetry diagnostic snapshots are bounded, private, and generation safe",
          "[foundation][observability][telemetry][snapshot][lifecycle]") {
    using namespace Horo::Telemetry;
    static_assert(std::is_trivially_copyable_v<MetricAvailabilityRecord>);
    static_assert(std::is_trivially_copyable_v<DiagnosticSnapshot>);
    static_assert(sizeof(DiagnosticSnapshot) <= 4096);

    Runtime::Shutdown();
    REQUIRE(Runtime::Initialize({.queueCapacity = 16, .enabled = true}, std::make_shared<NullSink>()));
    auto descriptor = MakeDescriptor("service.request.result");
    descriptor.description = "secret payload and personal identity are never copied into this snapshot";
    descriptor.dimensions = {{.key = "outcome", .allowedValues = {"success", "failed"}}};
    descriptor.maxSeries = 2;
    const Counter root = Runtime::RegisterCounter(std::move(descriptor));
    const std::array selection{DimensionValue{.key = "outcome", .value = "success"}};
    const Counter first = root.WithDimensions(selection);
    REQUIRE(first);

    const DiagnosticSnapshot available = Runtime::GetDiagnosticSnapshot();
    REQUIRE(available.runtimeEnabled);
    REQUIRE(available.availabilityCount == 1);
    CHECK(available.availability[0].instrumentId == 1);
    CHECK(available.availability[0].state == MetricAvailabilityState::Available);
    CHECK(available.availabilityRevision == 1);

    REQUIRE(Runtime::SetAvailability(first, MetricAvailabilityState::PermissionDenied));
    const DiagnosticSnapshot unavailable = Runtime::GetDiagnosticSnapshot();
    CHECK(unavailable.availability[0].state == MetricAvailabilityState::PermissionDenied);
    CHECK(unavailable.availabilityRevision == available.availabilityRevision + 1);
    CHECK_FALSE(Runtime::SetAvailability(first, static_cast<MetricAvailabilityState>(255)));
    CHECK(Runtime::GetDiagnosticSnapshot().availabilityRevision == unavailable.availabilityRevision);

    REQUIRE(Runtime::Shutdown());
    const DiagnosticSnapshot stopped = Runtime::GetDiagnosticSnapshot();
    CHECK_FALSE(stopped.runtimeEnabled);
    CHECK(stopped.availabilityCount == 0);

    REQUIRE(Runtime::Initialize({.queueCapacity = 16, .enabled = true}, std::make_shared<NullSink>()));
    const Counter current = Runtime::RegisterCounter(MakeDescriptor("service.request.result"));
    REQUIRE(current);
    CHECK_FALSE(Runtime::SetAvailability(first, MetricAvailabilityState::SamplerFailed));
    const DiagnosticSnapshot restarted = Runtime::GetDiagnosticSnapshot();
    CHECK(restarted.runtimeGeneration != available.runtimeGeneration);
    CHECK(restarted.availabilityCount == 1);
    CHECK(restarted.availability[0].state == MetricAvailabilityState::Available);
    CHECK(Runtime::Shutdown());
}

TEST_CASE("Metric registry and availability snapshot stop at the host capacity",
          "[foundation][observability][telemetry][snapshot][bounds]") {
    using namespace Horo::Telemetry;
    Runtime::Shutdown();
    REQUIRE(Runtime::Initialize({.queueCapacity = 16, .enabled = true}, std::make_shared<NullSink>()));
    const std::uint64_t invalidBefore = Runtime::GetStatistics().invalidInstrumentRegistrations;

    for (std::size_t index = 0; index < MaximumMetricInstruments; ++index)
        REQUIRE(Register(MakeDescriptor("metric." + std::to_string(index))));
    CHECK_FALSE(Register(MakeDescriptor("metric.overflow")));

    const DiagnosticSnapshot snapshot = Runtime::GetDiagnosticSnapshot();
    CHECK(snapshot.availabilityCount == MaximumMetricInstruments);
    CHECK(snapshot.availability.back().instrumentId == MaximumMetricInstruments);
    CHECK(snapshot.statistics.invalidInstrumentRegistrations == invalidBefore + 1);
    CHECK(Runtime::Shutdown());
}
