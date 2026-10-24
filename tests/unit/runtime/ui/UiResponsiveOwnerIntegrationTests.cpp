#include "Horo/Runtime/Ui/UiDocumentSerialization.h"
#include "UiAnimationOwnerIntegrationFixture.h"

#include <algorithm>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Ui;
    using namespace Horo::Runtime::Ui::AnimationTests;
    using ReloadTests::Revision;
    using ReloadTests::Stable;

    class AttachmentMetrics final : public UiLayoutIntrinsicProvider {
    public:
        mutable std::array<std::int32_t, 32> widths{};
        mutable std::size_t count{};

        Result<UiLayoutIntrinsicMeasurement> MeasureText(const UiLayoutIntrinsicRequest &request) const override {
            if (count < widths.size())
                widths[count++] = request.available.width;
            if (request.available.width == 16)
                return Result<UiLayoutIntrinsicMeasurement>::Failure(MakeError(UiErrors::LayoutIntrinsicUnavailable));
            return Result<UiLayoutIntrinsicMeasurement>::Success({{20, 20}, NoUiBaseline, false, false});
        }

        Result<UiLayoutIntrinsicMeasurement> MeasureImage(const UiLayoutIntrinsicRequest &request) const override {
            return MeasureText(request);
        }
    };

    UiDocumentPresentationProfiles CompactProfiles() {
        UiLayoutStyle compact;
        compact.width = UiLength::Dip(80);
        compact.height = UiLength::Dip(40);
        compact.container.orientation = UiLayoutOrientation::Horizontal;
        UiPresentationProfile row{Stable<UiPresentationProfileId>(30), 1};
        row.selector.width.maximum = 80;
        row.overrides.push_back({Stable<UiElementId>(11), compact});
        return {Stable<UiCanvasId>(2), Revision<UiPresentationProfileRevision>(1), {row}};
    }

    UiAnimationViewport CompactViewport() {
        auto viewport = Config().viewport;
        viewport.content.extent.width = 64;
        viewport.constraints.maximum.width = 64;
        viewport.presentation =
            UiAnimationPresentationEvidence{Revision<UiPresentationEvidenceRevision>(1), UiPresentationFormFactor::Handheld,
                                            UiPresentationViewRole::Split, UiPresentationCapabilities::Touch};
        return viewport;
    }

    /** @brief Checks actual selected, clipped and computed geometry before exercising input transitions. */
    void CheckCompactFrame(const UiAnimationFrameLease &first) {
        REQUIRE(first.PresentationSelection() != nullptr);
        REQUIRE_FALSE(first.PresentationSelection()->profile.has_value());
        REQUIRE(first.Attachments().size() == 1);
        const auto &derived = first.Attachments().front();
        REQUIRE(derived.selection.profile == Stable<UiPresentationProfileId>(30));
        REQUIRE(derived.layout->Descriptor().interaction == first.Layout().Descriptor().interaction);
        REQUIRE(derived.clipping->Descriptor().interaction == first.Layout().Descriptor().interaction);
        REQUIRE(derived.focusGeometry.size() == derived.layout->Records().size());
        REQUIRE(derived.focusGeometry.back().element == first.Controls().front().source.element);
        REQUIRE(derived.focusGeometry.back().bounds.extent.width == 64);
        const auto child = first.Controls().front().source.element;
        REQUIRE(derived.layout->Get(child).Value().arrangement.contentBox.extent.width == 80);
        // Complete profile height=40 is a base; the explicit computed height binding remains final at300.
        REQUIRE(derived.layout->Get(child).Value().arrangement.contentBox.extent.height == 300);
        REQUIRE(first.Layout().Get(child).Value().arrangement.contentBox.extent.width == 100);
    }

    /** @brief Creates the real replacement closure and resources for one owner reload boundary. */
    Result<UiAnimationRuntimeReload> ReloadFixture(UiAnimationRuntimeParticipant &participant, UiElementSlotAllocator &allocator,
                                                   const std::uint64_t version, const CancellationToken &cancellation = {}) {
        auto replacement = ReloadTests::Generation(allocator, version);
        auto resources = Resources(replacement.Canvases().front());
        return participant.Reload(std::move(replacement), allocator, std::move(resources.registry), std::move(resources.styles),
                                  std::move(resources.definition),
                                  {UiAnimationReloadPolicy::Cancel, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands}, cancellation);
    }
}  // namespace

TEST_CASE("Actual owner selects durable alternatives per attachment before computed bound dimensions",
          "[runtime_ui][responsive][integration]") {
    HostOptions options;
    options.profiles = {CompactProfiles()};
    HostFixture fixture{options};
    auto &participant = *fixture.participant;
    const UiRenderViewId view{ReloadTests::Owner(), 21, 1};
    const auto attached = participant.Attach(view, CompactViewport());
    REQUIRE(attached.HasValue());
    REQUIRE_FALSE(participant.InputEligible(view));
    REQUIRE(participant.ConfigureInspection(attached.Value(), {8, 8, 8, 2}, true).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto first = Frame(participant);
    CheckCompactFrame(first);
    const auto &derived = first.Attachments().front();
    const auto child = first.Controls().front().source.element;
    REQUIRE(participant.Inspect().has_value());
    REQUIRE(participant.Inspect()->Descriptor().selection->profile == Stable<UiPresentationProfileId>(30));
    REQUIRE_FALSE(participant.InputEligible(view));
    auto receipt = Receipt(first, 1);
    receipt.view = view;
    REQUIRE(participant.ApplyPresentation(receipt).HasValue());
    REQUIRE(participant.InputEligible(view));
    auto changed = CompactViewport();
    changed.presentation->revision = Revision<UiPresentationEvidenceRevision>(2);
    changed.content.extent.width = 90;
    changed.constraints.maximum.width = 90;
    REQUIRE(participant.UpdateAttachment(attached.Value(), changed).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto second = Frame(participant);
    REQUIRE(second.Layout().Descriptor().interaction > first.Layout().Descriptor().interaction);
    REQUIRE_FALSE(second.Attachments().front().selection.profile.has_value());
    REQUIRE_FALSE(participant.InputEligible(view));
    REQUIRE(participant.ApplyPresentation(receipt).HasError());
    receipt = Receipt(second, 2);
    receipt.view = view;
    REQUIRE(participant.ApplyPresentation(receipt).HasValue());
    REQUIRE(participant.InputEligible(view));
    REQUIRE(derived.selection.profile == Stable<UiPresentationProfileId>(30));
    REQUIRE(participant.Detach(attached.Value()).HasValue());
    REQUIRE_FALSE(participant.InputEligible(view));
    REQUIRE(participant.UpdateAttachment(attached.Value(), changed).HasError());
    fixture.host->Shutdown();
    REQUIRE(derived.layout->Get(child).HasValue());
}

TEST_CASE("Unchanged responsive frames retain interaction while later attachments join the actual primary fence",
          "[runtime_ui][responsive][integration]") {
    HostFixture fixture;
    auto &participant = *fixture.participant;
    auto firstAttachment = participant.Attach({ReloadTests::Owner(), 21, 1}, CompactViewport());
    REQUIRE(firstAttachment.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto first = Frame(participant);
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto unchanged = Frame(participant);
    REQUIRE(unchanged.Layout().Descriptor().interaction == first.Layout().Descriptor().interaction);
    REQUIRE(unchanged.Attachments().front().layout->Descriptor().interaction == first.Layout().Descriptor().interaction);
    auto joined = participant.Attach({ReloadTests::Owner(), 22, 1}, CompactViewport());
    REQUIRE(joined.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto next = Frame(participant);
    REQUIRE(next.Attachments().size() == 2);
    for (const auto &attachment : next.Attachments())
        REQUIRE(attachment.layout->Descriptor().interaction == next.Layout().Descriptor().interaction);
    REQUIRE(next.Layout().Descriptor().interaction > first.Layout().Descriptor().interaction);
}

TEST_CASE("Required attachment failure rolls back after primary and an earlier attachment prepared",
          "[runtime_ui][responsive][integration]") {
    auto metrics = std::make_shared<AttachmentMetrics>();
    HostOptions options;
    options.intrinsicProvider = metrics;
    auto profiles = CompactProfiles();
    UiPresentationProfile failureRow{Stable<UiPresentationProfileId>(31), 0};
    failureRow.selector.width.maximum = 32;
    UiLayoutStyle failureStyle;
    failureStyle.width = UiLength::Dip(16);
    failureRow.overrides.push_back({Stable<UiElementId>(11), failureStyle});
    profiles.rows.push_back(failureRow);
    options.profiles = {profiles};
    HostFixture fixture{options};
    auto &participant = *fixture.participant;
    REQUIRE(participant.Attach({ReloadTests::Owner(), 21, 1}, CompactViewport()).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto old = Frame(participant);
    const auto oldInteraction = old.Layout().Descriptor().interaction;
    auto impossible = CompactViewport();
    impossible.content.extent.width = 16;
    impossible.constraints.maximum.width = 16;
    REQUIRE(participant.Attach({ReloadTests::Owner(), 22, 1}, impossible).HasValue());
    const auto before = metrics->count;
    REQUIRE(fixture.host->RunFrame().HasError());
    REQUIRE(metrics->count >= before + 3);
    REQUIRE(metrics->widths[before] == 100);
    REQUIRE(metrics->widths[before + 1] == 80);
    REQUIRE(metrics->widths[before + 2] == 16);
    REQUIRE(old.Attachments().size() == 1);
    REQUIRE(old.Layout().Descriptor().interaction == oldInteraction);
    REQUIRE(old.Attachments().front().layout->Descriptor().interaction == oldInteraction);
    REQUIRE_FALSE(participant.InputEligible({ReloadTests::Owner(), 21, 1}));
}

TEST_CASE("Responsive bases preserve computed width animation and accessibility authority", "[runtime_ui][responsive][integration]") {
    for (const bool accessible : {false, true}) {
        HostOptions options;
        options.profiles = {CompactProfiles()};
        options.boundWidth = true;
        options.accessibilityWidth = accessible;
        HostFixture fixture{options};
        auto &participant = *fixture.participant;
        REQUIRE(participant.Attach({ReloadTests::Owner(), 21, 1}, CompactViewport()).HasValue());
        REQUIRE(participant.Start(Stable<UiAnimationId>(1)).HasValue());
        REQUIRE(fixture.host->RunFrame().HasValue());
        const auto first = Frame(participant);
        const auto child = first.Controls().front().source.element;
        const auto derived = *first.Attachments().front().layout;
        REQUIRE(derived.Get(child).Value().arrangement.contentBox.extent.width == (accessible ? 64 : 300));
        REQUIRE(derived.Get(child).Value().arrangement.contentBox.extent.height == 40);
        fixture.clock.Advance(Duration::FromMilliseconds(100));
        REQUIRE(fixture.host->RunFrame().HasValue());
        const auto next = Frame(participant);
        REQUIRE(next.Attachments().front().layout->Get(child).Value().arrangement.contentBox.extent.width == (accessible ? 64 : 150));
        REQUIRE(next.Attachments().front().selection.profile == Stable<UiPresentationProfileId>(30));
        REQUIRE(derived.Get(child).Value().arrangement.contentBox.extent.width == (accessible ? 64 : 300));
    }
}

TEST_CASE("Optional owner inspection exhaustion and disabled capture do not stop committed frames",
          "[runtime_ui][responsive][integration]") {
    HostFixture fixture;
    auto &participant = *fixture.participant;
    REQUIRE(participant.ConfigureInspection(participant.PrimaryAttachment(), {8, 8, 8, 2}, false).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    REQUIRE_FALSE(participant.Inspect().has_value());
    REQUIRE(participant.ConfigureInspection(participant.PrimaryAttachment(), {8, 8, 8, 2}, true).HasValue());
    REQUIRE(participant.Start(Stable<UiAnimationId>(1)).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto first = participant.Inspect();
    REQUIRE(first.has_value());
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto second = participant.Inspect();
    REQUIRE(second.has_value());
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto current = Frame(participant);
    REQUIRE(current.Layout().Descriptor().interaction > first->Descriptor().layout.interaction);
    REQUIRE(participant.Inspect()->Descriptor().layout.interaction == second->Descriptor().layout.interaction);
    auto receipt = Receipt(current, 1);
    REQUIRE(participant.ApplyPresentation(receipt).HasValue());
    REQUIRE(participant.InputEligible(receipt.view));
    fixture.host->Shutdown();
    REQUIRE_FALSE(first->Records().empty());
}

TEST_CASE("Responsive reload preserves cancelled admission and burns successful replacement attachment identities",
          "[runtime_ui][responsive][integration][reload]") {
    auto issued = UiElementSlotAllocator::Create(ReloadTests::Owner());
    REQUIRE(issued.HasValue());
    auto allocator = std::move(issued).Value();
    HostOptions options;
    options.allocator = &allocator;
    options.profiles = {CompactProfiles()};
    HostFixture fixture{options};
    auto &participant = *fixture.participant;
    const auto primary = participant.PrimaryAttachment();
    const UiRenderViewId view{ReloadTests::Owner(), 21, 1};
    const auto attached = participant.Attach(view, CompactViewport());
    REQUIRE(attached.HasValue());
    REQUIRE(participant.ConfigureInspection(attached.Value(), {8, 8, 8, 2}, true).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto retained = Frame(participant);
    const auto inspected = participant.Inspect();
    REQUIRE(inspected.has_value());
    CancellationSource cancelled;
    cancelled.RequestCancellation();
    REQUIRE(ReloadFixture(participant, allocator, 100, cancelled.Token()).HasError());
    REQUIRE(participant.PrimaryAttachment() == primary);
    REQUIRE(participant.Inspect()->Descriptor().layout.interaction == inspected->Descriptor().layout.interaction);
    REQUIRE(Frame(participant).Attachments().front().attachment == attached.Value());
    REQUIRE(ReloadFixture(participant, allocator, 101).HasValue());
    REQUIRE(participant.PrimaryAttachment() != primary);
    REQUIRE_FALSE(participant.Inspect().has_value());
    REQUIRE(participant.UpdateAttachment(attached.Value(), CompactViewport()).HasError());
    REQUIRE(participant.ConfigureInspection(attached.Value(), {8, 8, 8, 2}, true).HasError());
    REQUIRE(fixture.host->RunFrame().HasValue());
    REQUIRE(Frame(participant).Attachments().empty());
    auto oldReceipt = Receipt(retained, 1);
    oldReceipt.view = view;
    REQUIRE(participant.ApplyPresentation(oldReceipt).HasError());
    const auto fresh = participant.Attach(view, CompactViewport());
    REQUIRE(fresh.HasValue());
    REQUIRE(fresh.Value() != attached.Value());
    REQUIRE(fixture.host->RunFrame().HasValue());
    REQUIRE(Frame(participant).Attachments().front().attachment == fresh.Value());
    REQUIRE(retained.Attachments().front().selection.profile == Stable<UiPresentationProfileId>(30));
    REQUIRE_FALSE(inspected->Records().empty());
}

TEST_CASE("Failed allocating attachment admission cannot adopt a presentation tracker",
          "[runtime_ui][responsive][integration][allocation]") {
    const UiRenderViewId view{ReloadTests::Owner(), 21, 1};
    std::size_t requests{};
    {
        HostFixture measured;
        Horo::Tests::AllocationProbe::ScopedMeasurement allocations;
        const auto attached = measured.participant->Attach(view, CompactViewport());
        requests = allocations.Snapshot().requests;
        REQUIRE(attached.HasValue());
    }
    REQUIRE(requests > 0);
    HostFixture fixture;
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto retained = Frame(participant);
    std::optional<Result<UiPresentationAttachmentId>> failed;
    {
        Horo::Tests::AllocationProbe::ScopedFailure failure{requests - 1};
        failed.emplace(participant.Attach(view, CompactViewport()));
    }
    REQUIRE(failed->HasError());
    REQUIRE(Frame(participant).Attachments().empty());
    auto foreignReceipt = Receipt(retained, 1);
    foreignReceipt.view = view;
    REQUIRE(participant.ApplyPresentation(foreignReceipt).HasError());
    REQUIRE_FALSE(participant.InputEligible(view));
    REQUIRE(fixture.host->RunFrame().HasValue());
    REQUIRE(Frame(participant).Attachments().empty());
    REQUIRE(participant.Attach(view, CompactViewport()).HasValue());
}
