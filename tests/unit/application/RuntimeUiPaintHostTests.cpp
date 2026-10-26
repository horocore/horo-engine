#include "Horo/Runtime/Ui/UiStyleAsset.h"
#include "RuntimeUiPaintHostTestFixture.h"

#include <algorithm>
#include <catch2/generators/catch_generators.hpp>

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

    /** @brief Advances the initial host publication before realizing the exact image-backed coverage texture. */
    Render::UiRenderImageTexture PrepareEffectMask(Fixture &fixture) {
        fixture.Frame();
        fixture.Frame();
        std::array<std::byte, 64> coverage;
        coverage.fill(std::byte{128});
        const auto mask = Take(fixture.frontend->CreateUiMaskTexture(*fixture.images, fixture.image, 0, coverage));
        REQUIRE(fixture.frontend->ProcessResourceRequests().HasValue());
        return mask;
    }

    /** @brief Prepares actual canonical scrolling and computed opacity before the same host reload transaction. */
    Runtime::Ui::UiAnimationReloadPreparation EffectReload(Fixture &fixture) {
        using namespace Runtime::Ui;
        const auto frame = Take(fixture.port.AcquireFrame());
        const auto high = std::max(frame.Layout().Descriptor().sources.tree.Value(), frame.Layout().Descriptor().interaction.Value());
        auto generation = fixture.source.Make(2, high + 1);
        auto &canvas = *generation.Canvas(Source::Stable<UiCanvasId>(2));
        VerifyReloadActionSource(canvas);
        const auto root = Take(canvas.tree.Root()).handle;
        for (auto &policy : canvas.clipPolicies) {
            policy.overflow = policy.element == root ? UiLayoutOverflowPolicy::Scroll : UiLayoutOverflowPolicy::Clip;
            if (policy.element == root)
                policy.scrollOffset = NestedEffectScroll;
        }
        auto resources = Runtime::Ui::AnimationTests::Resources(canvas);
        const auto core = Take(CreateCoreUiStyleSchema({}));
        const auto opacity = CoreUiStylePropertyId(UiCoreStyleProperty::Opacity);
        const auto property = std::ranges::find(core.properties, opacity, &UiStylePropertyDescriptor::id);
        REQUIRE(property != core.properties.end());
        UiStyleRegistryDefinition registry;
        registry.properties.assign(resources.registry.Properties().begin(), resources.registry.Properties().end());
        registry.properties.push_back(*property);
        registry.assets.push_back({.id = Source::Stable<RuntimeStyleAssetId>(1)});
        resources.registry = Take(RuntimeStyleRegistry::Create(std::move(registry), Source::Revision<RuntimeStyleGeneration>(1)));
        resources.styles =
            Take(UiStyleResolver::Create({canvas.tree.Instance(), canvas.tree.Canvas(), canvas.tree.SourceDocument(), 8, 2, 8, 4,
                                          Source::Revision<RuntimeStyleGeneration>(1), Source::Revision<UiStylePublicationRevision>(1)}));
        for (auto &element : resources.definition.elements) {
            if (element.element == Source::Stable<UiElementId>(11)) {
                element.layout.positioning = UiLayoutPositioning::Absolute;
                element.layout.offsets.top = NestedEffectChildOffset;
            }
            element.policyProperties.push_back(
                {opacity, UiStyleValueSource::Literal(UiStyleScalar{element.element == Source::Stable<UiElementId>(10) ? 0.5F : 0.25F}),
                 false});
        }
        return {std::move(generation),           fixture.source.slots,
                std::move(resources.registry),   std::move(resources.styles),
                std::move(resources.definition), {UiAnimationReloadPolicy::Cancel, Tests::UiAccess::Lifecycle}};
    }

    /** @brief Supplies real image-backed and nested mask/isolation components at the exact source reload boundary. */
    RuntimeUiPaintDeclaration EffectDeclaration(const Fixture &fixture, const Runtime::Ui::UiMaskShape shape,
                                                const Render::UiRenderImageTexture &mask) {
        auto declaration = fixture.Declaration(2);
        const bool textured = shape == Runtime::Ui::UiMaskShape::AlphaTexture;
        if (textured)
            declaration.resources.push_back(mask.Source());
        declaration.masks.push_back({Source::Stable<Runtime::Ui::UiElementId>(11), shape, textured ? 1U : Runtime::Ui::NoUiRenderIndex,
                                     shape == Runtime::Ui::UiMaskShape::RoundedRectangle ? std::array<std::int32_t, 4>{10, 10, 10, 10}
                                                                                         : std::array<std::int32_t, 4>{}});
        declaration.isolated.push_back(Source::Stable<Runtime::Ui::UiElementId>(11));
        return declaration;
    }

    /** @brief Checks the exact new host publication and copied current backend workload, never a previous frame's pass offsets. */
    void VerifyEffectFrame(const Fixture &fixture, const Runtime::Ui::UiAnimationFrameLease &current, const Runtime::Ui::UiMaskShape shape,
                           const std::size_t previousGraphCalls) {
        using namespace Runtime::Ui;
        using namespace Tests::RenderFrontendTesting;
        REQUIRE(lifecycleState.uiPaint.graphCalls > previousGraphCalls);
        REQUIRE(current.Clipping() != nullptr);
        const auto child = std::ranges::find(current.Controls(), Source::Stable<Runtime::Ui::UiElementId>(11),
                                             &Runtime::Ui::UiAnimationControlRecord::element);
        REQUIRE(child != current.Controls().end());
        const auto layout = Take(current.Layout().Get(child->source.element));
        REQUIRE(layout.arrangement.borderBox.extent.width > 0);
        REQUIRE(layout.arrangement.borderBox.extent.height > 0);
        const auto clipped = Take(current.Clipping()->Get(child->source.element));
        CHECK(clipped.scrollTranslation.y < 0);
        CHECK(clipped.scrollTranslation.y == -NestedEffectScroll.y);
        const auto draws = [&](const auto predicate) {
            return std::any_of(lifecycleState.uiPaint.passes.begin(), lifecycleState.uiPaint.passes.end(), [&](const auto &pass) {
                return std::ranges::any_of(pass.draws, predicate);
            });
        };
        CHECK(draws([](const auto &draw) {
            return draw.operation == Render::UiPaintOperation::PushStencil;
        }));
        CHECK(draws([](const auto &draw) {
            return draw.operation == Render::UiPaintOperation::PopStencil;
        }));
        CHECK(draws([&](const auto &draw) {
            return draw.sampling ==
                   (shape == UiMaskShape::RoundedRectangle ? Render::UiPaintSampling::RoundedRectangle : Render::UiPaintSampling::Alpha);
        }));
        CHECK(draws([](const auto &draw) {
            return draw.sampling == Render::UiPaintSampling::Premultiplied && draw.opacity == 0.5F;
        }));
        CHECK(draws([](const auto &draw) {
            return draw.sampling == Render::UiPaintSampling::Premultiplied && draw.opacity == 0.25F;
        }));
        CHECK(draws([&](const auto &draw) {
            return draw.scissor.width < fixture.output.width || draw.scissor.height < fixture.output.height;
        }));
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

TEST_CASE("Actual application component effects retain nested masks opacity and scroll through reload and completion",
          "[application][runtime_ui][paint][effects][lifetime]") {
    using namespace Runtime::Ui;
    using namespace Tests::RenderFrontendTesting;
    const auto shape = GENERATE(UiMaskShape::RoundedRectangle, UiMaskShape::AlphaTexture);
    Fixture fixture{true, NestedEffectScroll};
    const auto mask = PrepareEffectMask(fixture);
    auto assets = fixture.Assets();
    if (shape == UiMaskShape::AlphaTexture)
        assets.images.push_back({fixture.images, mask});
    auto old = Take(fixture.port.AcquireFrame());
    REQUIRE(old.Clipping() != nullptr);
    REQUIRE(std::ranges::any_of(old.Clipping()->Scrolls(), [](const auto &scroll) {
        return scroll.offset == NestedEffectScroll;
    }));
    auto client = Take(fixture.port.Issue(fixture.source.Grant()));
    CancellationSource cancellation;
    cancellation.RequestCancellation();
    const auto cancelled =
        fixture.port.Reload(EffectReload(fixture), EffectDeclaration(fixture, shape, mask), assets, cancellation.Token());
    REQUIRE(cancelled.HasError());
    CHECK(cancelled.ErrorValue().code.Value() == UiErrors::AssetLoadCancelled.code.Value());
    CHECK(client->Validate().HasValue());
    CHECK(fixture.port.InputEligible(fixture.view));
    REQUIRE(fixture.port.Reload(EffectReload(fixture), EffectDeclaration(fixture, shape, mask), assets).HasValue());
    CHECK(client->Validate().HasError());
    CHECK_FALSE(fixture.port.InputEligible(fixture.view));
    const auto previousGraphCalls = lifecycleState.uiPaint.graphCalls;
    fixture.Frame();
    CHECK_FALSE(fixture.port.InputEligible(fixture.view));
    fixture.Frame();
    REQUIRE(fixture.port.InputEligible(fixture.view));
    auto current = Take(fixture.port.AcquireFrame());
    VerifyEffectFrame(fixture, current, shape, previousGraphCalls);
    lifecycleState.completeGraphOnPresent = false;
    fixture.Frame();
    REQUIRE(lifecycleState.graphLease != nullptr);
    REQUIRE(fixture.port.DetachSource().HasValue());
    CHECK_FALSE(fixture.port.InputEligible(fixture.view));
    old = {};
    current = {};
    CHECK_FALSE(Take(fixture.port.CollectRetired()));
    lifecycleState.graphLease->Release();
    lifecycleState.graphLease = nullptr;
    lifecycleState.completeGraphOnPresent = true;
    REQUIRE(Take(fixture.port.CollectRetired()));
    client.reset();
    fixture.Stop();
}
