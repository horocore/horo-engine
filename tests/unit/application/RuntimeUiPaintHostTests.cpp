#include "RuntimeUiPaintHostTestFixture.h"

#include <algorithm>

using namespace Horo;
using namespace Horo::Tests::UiPaintHostTesting;

namespace {
    /** @brief Proves real reload owner agreement while retaining explicit stale document-seeded input rejection. */
    void VerifyReloadActionSource(Runtime::Ui::UiReloadCanvas &canvas) {
        REQUIRE(canvas.actions.has_value());
        REQUIRE(canvas.actions->Owner() == canvas.controls.front().control.Owner());
        REQUIRE(canvas.actions->Owner().interaction == canvas.layout->Descriptor().interaction);
        // Deliberately document-seeded interaction remains invalid; it cannot authorize this newer runtime generation.
        auto staleOwner = canvas.actions->Owner();
        staleOwner.interaction = Source::Revision<Runtime::Ui::UiInteractionRevision>(2);
        REQUIRE(staleOwner.interaction != canvas.actions->Owner().interaction);
        const auto *actions = canvas.actions->AsyncActions();
        REQUIRE(actions != nullptr);
        const auto stale = actions->Project({staleOwner, canvas.controls.front().control.Element()});
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().domain.Value() == Runtime::Ui::UiErrors::ActionSourceStale.domain.Value());
        CHECK(stale.ErrorValue().code.Value() == Runtime::Ui::UiErrors::ActionSourceStale.code.Value());
        REQUIRE(actions->Project({canvas.actions->Owner(), canvas.controls.front().control.Element()}).HasValue());
    }
}  // namespace

TEST_CASE("Actual application UI host extracts non-solid source and composes the same runtime frame before its receipt",
          "[application][runtime_ui][paint]") {
    Fixture fixture;
    fixture.Frame();
    CHECK(Tests::RenderFrontendTesting::lifecycleState.uiPaint.updates == 0);
    CHECK_FALSE(fixture.port.InputEligible(fixture.view));
    fixture.Frame();
    const auto &audit = Tests::RenderFrontendTesting::lifecycleState.uiPaint;
    REQUIRE(audit.updates == 1);
    CHECK(fixture.port.InputEligible(fixture.view));
    CHECK(std::ranges::any_of(audit.passes, [](const auto &pass) {
        return std::ranges::any_of(pass.draws, [](const auto &draw) {
            return draw.sampling == Render::UiPaintSampling::Color;
        });
    }));
    const auto residents = Tests::RenderFrontendTesting::lifecycleState.createBufferCount;
    fixture.Frame();
    CHECK(audit.updates == 2);
    CHECK(Tests::RenderFrontendTesting::lifecycleState.createBufferCount == residents);
    fixture.Stop();
    CHECK(fixture.port.Phase() == RuntimeUiPaintHostPhase::NoSource);
    CHECK(fixture.port.Issue(fixture.source.Grant()).HasError());
}

TEST_CASE("Actual application UI host retains detached and revoked owners until an observed frame drains",
          "[application][runtime_ui][paint][lifetime]") {
    Fixture fixture;
    fixture.Frame();
    fixture.Frame();
    auto observed = Take(fixture.port.AcquireFrame());
    REQUIRE(fixture.port.DetachSource().HasValue());
    CHECK_FALSE(Take(fixture.port.CollectRetired()));
    CHECK_FALSE(fixture.port.InputEligible(fixture.view));
    observed = {};
    REQUIRE(Take(fixture.port.CollectRetired()));
    auto client = Take(fixture.port.Issue(fixture.source.Grant()));
    CHECK(client->Validate().HasValue());  // Detaching derived animation does not destroy the service's sole publisher.
    client.reset();
    fixture.Stop();
    fixture.host.reset();
    CHECK(fixture.port.AcquireFrame().HasError());
    CHECK(fixture.port.Issue(fixture.source.Grant()).HasError());
    CHECK(Take(fixture.port.CollectRetired()));
}

TEST_CASE("Actual application coordinated source reload preserves failed admission and eventually presents the new generation",
          "[application][runtime_ui][paint][reload]") {
    Fixture fixture;
    fixture.Frame();
    fixture.Frame();
    auto old = Take(fixture.port.AcquireFrame());
    auto client = Take(fixture.port.Issue(fixture.source.Grant()));
    const auto prepare = [&]() {
        const auto high = std::max(old.Layout().Descriptor().sources.tree.Value(), old.Layout().Descriptor().interaction.Value());
        auto generation = fixture.source.Make(2, high + 1);
        auto &canvas = *generation.Canvas(Source::Stable<Runtime::Ui::UiCanvasId>(2));
        VerifyReloadActionSource(canvas);
        auto resources = Runtime::Ui::AnimationTests::Resources(canvas);
        return Runtime::Ui::UiAnimationReloadPreparation{std::move(generation),
                                                         fixture.source.slots,
                                                         std::move(resources.registry),
                                                         std::move(resources.styles),
                                                         std::move(resources.definition),
                                                         {Runtime::Ui::UiAnimationReloadPolicy::Cancel, Tests::UiAccess::Lifecycle}};
    };
    CancellationSource cancellation;
    cancellation.RequestCancellation();
    auto failed = fixture.port.Reload(prepare(), fixture.Declaration(2), fixture.Assets(), cancellation.Token());
    REQUIRE(failed.HasError());
    CHECK(failed.ErrorValue().domain.Value() == Runtime::Ui::UiErrors::AssetLoadCancelled.domain.Value());
    CHECK(failed.ErrorValue().code.Value() == Runtime::Ui::UiErrors::AssetLoadCancelled.code.Value());
    CHECK(client->Validate().HasValue());
    CHECK(Take(fixture.port.AcquireFrame()).Layout().Descriptor().sources.document.Value() == 1);
    REQUIRE(fixture.port.Reload(prepare(), fixture.Declaration(2), fixture.Assets()).HasValue());
    CHECK(client->Validate().HasError());
    CHECK_FALSE(fixture.port.InputEligible(fixture.view));
    REQUIRE(Take(fixture.port.CollectRetired()));
    fixture.Frame();
    CHECK_FALSE(fixture.port.InputEligible(fixture.view));
    fixture.Frame();
    CHECK(fixture.port.InputEligible(fixture.view));
    CHECK(Take(fixture.port.AcquireFrame()).Layout().Descriptor().sources.document.Value() == 2);
    old = {};
    client.reset();
    fixture.Stop();
}
