#include "Horo/Runtime/Render/RenderDisplay.h"
#include "Horo/Runtime/Render/RenderDisplayErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    [[nodiscard]] RenderDisplayMode SdrMode(const float refresh = 60.0F) {
        return {1920,
                1080,
                refresh,
                RenderDisplayColorSpace::Srgb,
                RenderDisplayTransferFunction::Srgb,
                RenderDisplayDynamicRange::Standard};
    }

    [[nodiscard]] RenderDisplayMode HdrMode() {
        return {3840, 2160, 120.0F, RenderDisplayColorSpace::Rec2020, RenderDisplayTransferFunction::Pq, RenderDisplayDynamicRange::High};
    }

    [[nodiscard]] RenderDisplayProperties SdrDisplay(std::string id = "display-a") {
        const RenderDisplayMode mode = SdrMode();
        return {RenderDisplayId{std::move(id)}, RenderDisplayHdrSupport::Unsupported, std::nullopt, {mode}, mode};
    }

    [[nodiscard]] RenderDisplayProperties HdrDisplay(std::string id = "display-b") {
        const RenderDisplayMode sdr = SdrMode();
        const RenderDisplayMode hdr = HdrMode();
        return {RenderDisplayId{std::move(id)},
                RenderDisplayHdrSupport::Supported,
                RenderDisplayLuminance{0.01F, 600.0F, 1'000.0F},
                {sdr, hdr},
                hdr};
    }

    [[nodiscard]] RenderDisplaySnapshot Snapshot(const std::uint64_t revision,
                                                 std::vector<RenderDisplayProperties> displays = {SdrDisplay()}) {
        RenderDisplaySnapshot snapshot;
        snapshot.revision = revision;
        snapshot.displays = std::move(displays);
        return snapshot;
    }

    [[nodiscard]] bool HasCode(const Error &error, const ErrorCodeDescriptor &descriptor) {
        return error.domain.Value() == descriptor.domain.Value() && error.code.Value() == descriptor.code.Value();
    }

    template <typename Mutator>
    void CheckRejected(RenderDisplaySnapshot snapshot, const ErrorCodeDescriptor &descriptor, Mutator &&mutate) {
        std::forward<Mutator>(mutate)(snapshot);
        const auto result = ValidateRenderDisplaySnapshot(snapshot);
        REQUIRE(result.HasError());
        CHECK(HasCode(result.ErrorValue(), descriptor));
    }
}  // namespace

TEST_CASE("Display snapshots accept bounded SDR and HDR facts", "[runtime][renderer][display]") {
    CHECK(ValidateRenderDisplaySnapshot(Snapshot(1)).HasValue());
    auto unknownHdr = Snapshot(2);
    unknownHdr.displays[0].hdr = RenderDisplayHdrSupport::Unknown;
    CHECK(ValidateRenderDisplaySnapshot(unknownHdr).HasValue());
    CHECK(ValidateRenderDisplaySnapshot(Snapshot(2, {SdrDisplay("display-a"), HdrDisplay("display-b")})).HasValue());
}

TEST_CASE("Display snapshots reject invalid numeric facts", "[runtime][renderer][display]") {
    CheckRejected(Snapshot(0), RenderDisplayErrors::InvalidSnapshot, [](auto &) {
    });
    CheckRejected(Snapshot(1), RenderDisplayErrors::InvalidSnapshot, [](auto &snapshot) {
        snapshot.displays[0].modes[0].refreshHertz = std::numeric_limits<float>::quiet_NaN();
    });
    CheckRejected(Snapshot(1), RenderDisplayErrors::InvalidSnapshot, [](auto &snapshot) {
        snapshot.displays[0].modes[0].refreshHertz = -1.0F;
    });
    CheckRejected(Snapshot(1), RenderDisplayErrors::InvalidSnapshot, [](auto &snapshot) {
        snapshot.displays[0].modes[0].refreshHertz = MaximumRenderDisplayRefreshHertz + 1.0F;
    });
    CheckRejected(Snapshot(1, {HdrDisplay()}), RenderDisplayErrors::InvalidSnapshot, [](auto &snapshot) {
        snapshot.displays[0].luminance->maximumPeak = std::numeric_limits<float>::quiet_NaN();
    });
    CheckRejected(Snapshot(1, {HdrDisplay()}), RenderDisplayErrors::InvalidSnapshot, [](auto &snapshot) {
        snapshot.displays[0].luminance->minimumNits = -0.1F;
    });
    CheckRejected(Snapshot(1, {HdrDisplay()}), RenderDisplayErrors::InvalidSnapshot, [](auto &snapshot) {
        snapshot.displays[0].luminance->maximumPeak = MaximumRenderDisplayLuminanceNits + 1.0F;
    });
}

TEST_CASE("Display snapshots require canonical unique identities and modes", "[runtime][renderer][display]") {
    CHECK_FALSE(RenderDisplayId{}.IsValid());
    CHECK(RenderDisplayId{"display:primary-1"}.IsValid());
    CHECK_FALSE(RenderDisplayId{"display primary"}.IsValid());

    auto duplicateDisplays = Snapshot(1, {SdrDisplay("same"), SdrDisplay("same")});
    CHECK(ValidateRenderDisplaySnapshot(duplicateDisplays).HasError());
    auto unorderedDisplays = Snapshot(1, {SdrDisplay("b"), SdrDisplay("a")});
    CHECK(ValidateRenderDisplaySnapshot(unorderedDisplays).HasError());

    auto duplicateModes = Snapshot(1);
    duplicateModes.displays[0].modes.push_back(duplicateModes.displays[0].modes[0]);
    CHECK(ValidateRenderDisplaySnapshot(duplicateModes).HasError());
    auto missingCurrent = Snapshot(1);
    missingCurrent.displays[0].currentMode = HdrMode();
    CHECK(ValidateRenderDisplaySnapshot(missingCurrent).HasError());

    std::vector<RenderDisplayProperties> tooManyDisplays;
    tooManyDisplays.reserve(MaximumRenderDisplays + 1);
    for (std::size_t index = 0; index <= MaximumRenderDisplays; ++index) {
        const std::string suffix = index < 10 ? "0" + std::to_string(index) : std::to_string(index);
        tooManyDisplays.push_back(SdrDisplay("display-" + suffix));
    }
    CHECK(ValidateRenderDisplaySnapshot(Snapshot(1, std::move(tooManyDisplays))).HasError());

    auto tooManyModes = Snapshot(1);
    tooManyModes.displays[0].modes.assign(MaximumRenderDisplayModes + 1, SdrMode());
    CHECK(ValidateRenderDisplaySnapshot(tooManyModes).HasError());
}

TEST_CASE("Display snapshots reject unknown and contradictory color HDR facts", "[runtime][renderer][display]") {
    CheckRejected(Snapshot(1), RenderDisplayErrors::UnsupportedFact, [](auto &snapshot) {
        snapshot.displays[0].modes[0].colorSpace = static_cast<RenderDisplayColorSpace>(255);
    });
    CheckRejected(Snapshot(1), RenderDisplayErrors::UnsupportedFact, [](auto &snapshot) {
        snapshot.displays[0].modes[0].dynamicRange = RenderDisplayDynamicRange::High;
    });
    CheckRejected(Snapshot(1, {HdrDisplay()}), RenderDisplayErrors::UnsupportedFact, [](auto &snapshot) {
        snapshot.displays[0].modes[1].transfer = RenderDisplayTransferFunction::Hlg;
    });
    CheckRejected(Snapshot(1), RenderDisplayErrors::UnsupportedFact, [](auto &snapshot) {
        snapshot.displays[0].modes[0].transfer = static_cast<RenderDisplayTransferFunction>(255);
    });
    CheckRejected(Snapshot(1), RenderDisplayErrors::UnsupportedFact, [](auto &snapshot) {
        snapshot.displays[0].modes[0].dynamicRange = static_cast<RenderDisplayDynamicRange>(255);
    });
    CheckRejected(Snapshot(1), RenderDisplayErrors::UnsupportedFact, [](auto &snapshot) {
        snapshot.displays[0].hdr = static_cast<RenderDisplayHdrSupport>(255);
    });
    CheckRejected(Snapshot(1, {HdrDisplay()}), RenderDisplayErrors::UnsupportedFact, [](auto &snapshot) {
        snapshot.displays[0].hdr = RenderDisplayHdrSupport::Unsupported;
    });
    CheckRejected(Snapshot(1), RenderDisplayErrors::UnsupportedFact, [](auto &snapshot) {
        snapshot.displays[0].hdr = RenderDisplayHdrSupport::Supported;
    });
    CheckRejected(Snapshot(1, {HdrDisplay()}), RenderDisplayErrors::UnsupportedFact, [](auto &snapshot) {
        snapshot.displays[0].hdr = RenderDisplayHdrSupport::Unknown;
    });
}

TEST_CASE("Display snapshot diffs report no change add remove mode and capability changes", "[runtime][renderer][display]") {
    const auto noChange = DiffRenderDisplaySnapshots(Snapshot(1), Snapshot(2));
    REQUIRE(noChange.HasValue());
    CHECK(noChange.Value().changes.empty());

    const RenderDisplaySnapshot previous = Snapshot(3, {SdrDisplay("a"), SdrDisplay("b")});
    const RenderDisplaySnapshot changedMembership = Snapshot(4, {SdrDisplay("b"), SdrDisplay("c")});
    const auto membership = DiffRenderDisplaySnapshots(previous, changedMembership);
    REQUIRE(membership.HasValue());
    REQUIRE(membership.Value().changes.size() == 2);
    CHECK(membership.Value().changes[0] == RenderDisplayChange{RenderDisplayId{"a"}, RenderDisplayChangeReason::Removed});
    CHECK(membership.Value().changes[1] == RenderDisplayChange{RenderDisplayId{"c"}, RenderDisplayChangeReason::Added});

    auto before = Snapshot(5, {HdrDisplay()});
    auto after = before;
    after.revision = 6;
    after.displays[0].currentMode = SdrMode();
    after.displays[0].luminance->maximumPeak = 900.0F;
    const auto capabilities = DiffRenderDisplaySnapshots(before, after);
    REQUIRE(capabilities.HasValue());
    REQUIRE(capabilities.Value().changes.size() == 2);
    CHECK(capabilities.Value().changes[0].reason == RenderDisplayChangeReason::CurrentModeChanged);
    CHECK(capabilities.Value().changes[1].reason == RenderDisplayChangeReason::CapabilitiesChanged);
}

TEST_CASE("Display feed rejects stale and equal revisions without changing publication", "[runtime][renderer][display]") {
    RenderDisplaySnapshotFeed feed;
    const auto initial = feed.Publish(Snapshot(7));
    REQUIRE(initial.HasValue());
    REQUIRE(initial.Value().changes.size() == 1);
    CHECK(initial.Value().previousRevision == 0);
    CHECK(initial.Value().changes[0].reason == RenderDisplayChangeReason::Added);

    auto result = feed.Publish(Snapshot(6));
    REQUIRE(result.HasError());
    CHECK(HasCode(result.ErrorValue(), RenderDisplayErrors::StaleRevision));
    result = feed.Publish(Snapshot(7));
    REQUIRE(result.HasError());
    CHECK(HasCode(result.ErrorValue(), RenderDisplayErrors::RevisionUnchanged));
    const auto current = feed.Snapshot();
    REQUIRE(current.HasValue());
    CHECK(current.Value().revision == 7);
}

TEST_CASE("Display feed enforces owner affinity and idempotent stop", "[runtime][renderer][display][lifecycle]") {
    RenderDisplaySnapshotFeed feed;
    const auto unavailable = feed.Snapshot();
    REQUIRE(unavailable.HasError());
    CHECK(HasCode(unavailable.ErrorValue(), RenderDisplayErrors::SnapshotUnavailable));

    std::optional<Result<RenderDisplaySnapshotDiff>> offThread;
    std::thread worker([&] {
        offThread.emplace(feed.Publish(Snapshot(1)));
    });
    worker.join();
    REQUIRE(offThread.has_value());
    REQUIRE(offThread->HasError());
    CHECK(HasCode(offThread->ErrorValue(), RenderDisplayErrors::ThreadAffinityViolation));

    REQUIRE(feed.Publish(Snapshot(1)).HasValue());
    feed.Stop();
    feed.Stop();
    auto publication = feed.Publish(Snapshot(2));
    REQUIRE(publication.HasError());
    CHECK(HasCode(publication.ErrorValue(), RenderDisplayErrors::FeedStopped));
    const auto snapshot = feed.Snapshot();
    REQUIRE(snapshot.HasError());
    CHECK(HasCode(snapshot.ErrorValue(), RenderDisplayErrors::FeedStopped));
}

TEST_CASE("Display facts distinguish separately named linear EDR from HDR10 PQ", "[runtime][renderer][display][linear-edr]") {
    for (const auto colorSpace : {RenderDisplayColorSpace::Srgb, RenderDisplayColorSpace::DisplayP3}) {
        auto display = HdrDisplay();
        display.modes = {{1920, 1080, 60.0F, colorSpace, RenderDisplayTransferFunction::Linear, RenderDisplayDynamicRange::High}};
        display.currentMode = display.modes.front();
        CHECK(ValidateRenderDisplaySnapshot(Snapshot(1, {display})).HasValue());
        display.hdr = RenderDisplayHdrSupport::Unsupported;
        CHECK(ValidateRenderDisplaySnapshot(Snapshot(1, {display})).HasError());
    }
}
