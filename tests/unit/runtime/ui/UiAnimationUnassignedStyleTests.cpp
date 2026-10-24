#include "UiAnimationOwnerIntegrationFixture.h"

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Ui;
    using namespace Horo::Runtime::Ui::ReloadTests;
    using namespace Horo::Runtime::Ui::AnimationTests;

    AnimationResources UnassignedResources(const UiReloadCanvas &canvas) {
        auto resources = Resources(canvas);
        const auto properties = resources.registry.Properties();
        auto registry = RuntimeStyleRegistry::Create({{properties.begin(), properties.end()}, {}}, Revision<RuntimeStyleGeneration>(1));
        REQUIRE(registry.HasValue());
        resources.registry = std::move(registry).Value();
        for (auto &element : resources.definition.elements)
            element.asset = {};
        return resources;
    }
}  // namespace

TEST_CASE("Real animation host publishes property-only styles without inventing an asset namespace",
          "[runtime_ui][animation][integration][unassigned]") {
    DeterministicClock clock;
    auto issued = UiElementSlotAllocator::Create(ReloadTests::Owner());
    REQUIRE(issued.HasValue());
    auto allocator = std::move(issued).Value();
    auto created = RuntimeHost::Create(clock);
    REQUIRE(created.HasValue());
    auto host = std::move(created).Value();
    auto initial = Generation(allocator, 1);
    auto resources = UnassignedResources(initial.Canvases().front());
    auto owner = UiAnimationOwner::Create(std::move(initial), allocator, std::move(resources.registry), std::move(resources.styles),
                                          std::move(resources.definition));
    REQUIRE(owner.HasValue());
    auto composed = UiAnimationRuntimeParticipant::Compose(std::move(owner).Value(), host->DispatchSource(), Config());
    REQUIRE(composed.HasValue());
    auto composition = std::move(composed).Value();
    auto *participant = composition.participant.get();
    REQUIRE(host->AddParticipant(std::move(composition.participant)).HasValue());
    REQUIRE(host->Startup().HasValue());
    REQUIRE(host->RunFrame().HasValue());
    auto frame = Frame(*participant);
    CheckGeometry(frame, 200);
    REQUIRE_FALSE(frame.Styles().Records().empty());
    for (const auto &record : frame.Styles().Records()) {
        const auto properties = frame.Styles().Properties(record);
        REQUIRE(properties.size() == 1);
        CHECK(properties.front().provenance.origin == UiStyleOrigin::RegisteredDefault);
        CHECK_FALSE(properties.front().provenance.asset.IsValid());
        CHECK(std::get<UiStyleDimension>(properties.front().value).value == 300);
    }
    const auto receipt = Receipt(frame, 1);
    REQUIRE(participant->ApplyPresentation(receipt).HasValue());
    CHECK(participant->InputEligible(receipt.view));
    host->Shutdown();
    CHECK(participant->Acquire().HasError());
    CHECK(frame.Styles().Records().size() == 2);
}

TEST_CASE("Actual animation owner rejects explicitly unknown styles even with an optional base asset",
          "[runtime_ui][animation][integration][unassigned]") {
    auto allocatorResult = UiElementSlotAllocator::Create(ReloadTests::Owner());
    REQUIRE(allocatorResult.HasValue());
    auto allocator = std::move(allocatorResult).Value();
    auto initial = Generation(allocator, 1);
    auto resources = UnassignedResources(initial.Canvases().front());
    SECTION("unknown base namespace") {
        resources.definition.elements.front().asset = Stable<RuntimeStyleAssetId>(99);
    }
    SECTION("unknown qualified class") {
        resources.definition.elements.front().typeClass = {Stable<RuntimeStyleAssetId>(99), Stable<UiStyleClassId>(7)};
    }
    const auto owner = UiAnimationOwner::Create(std::move(initial), allocator, std::move(resources.registry), std::move(resources.styles),
                                                std::move(resources.definition));
    REQUIRE(owner.HasError());
    CHECK(owner.ErrorValue().domain.Value() == UiErrors::AnimationPolicyInvalid.domain.Value());
    CHECK(owner.ErrorValue().code.Value() == UiErrors::AnimationPolicyInvalid.code.Value());
}
