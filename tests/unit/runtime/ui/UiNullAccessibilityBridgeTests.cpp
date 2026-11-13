#include "Horo/Platform/AccessibilityBridge.h"
#include "Horo/Platform/AccessibilitySettings.h"
#include "Horo/Runtime/Ui/UiPresentationReceipt.h"
#include "UiAccessibilityTestUtils.h"

namespace Horo::Runtime::Ui {
    TEST_CASE("Unsupported native capability does not erase the desired screen-reader preference",
              "[runtime_ui][accessibility][platform]") {
        ConfigurationSchema schema;
        for (const auto &descriptor : Platform::MakeScreenReaderSettingsContribution().settings)
            REQUIRE(schema.Register(descriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        ConfigurationResolutionRequest inputs;
        const auto initial = Platform::ScreenReaderSettingsSnapshot::Capture(ConfigurationResolver::Resolve(schema, inputs, 1).Value());
        REQUIRE(initial.HasValue());
        REQUIRE_FALSE(initial.Value().Enabled());
        inputs.user.emplace(SettingKey{"accessibility.screen_reader.enabled"}, ConfigurationInputValue{true, {}});
        const auto desired = Platform::ScreenReaderSettingsSnapshot::Capture(ConfigurationResolver::Resolve(schema, inputs, 2).Value());
        REQUIRE(desired.HasValue());
        Platform::NullPlatformAccessibilityBridge bridge;
        REQUIRE(bridge.Descriptor().capability == Platform::AccessibilityBridgeCapability::Unsupported);
        REQUIRE(desired.Value().Enabled());
        REQUIRE(desired.Value().Revision() == 2);
        REQUIRE_FALSE(initial.Value().Enabled());
    }

    TEST_CASE("Null accessibility bridge is unsupported, has no output or allocation, and closes admission idempotently",
              "[runtime_ui][accessibility][platform][null]") {
        auto tree = MakeTree();
        auto extractor = MakeExtractor(tree);
        auto publisher = MakePublisher(tree);
        ProjectionFixture fixture;
        const auto snapshot = extractor.Extract(tree, Descriptor(tree), fixture.View()).Value();
        REQUIRE(publisher.Publish(snapshot).HasValue());
        Platform::NullPlatformAccessibilityBridge bridge;
        const auto capability = bridge.Descriptor();
        REQUIRE(capability.capability == Platform::AccessibilityBridgeCapability::Unsupported);
        REQUIRE(capability.technology == Platform::AccessibilityTechnology::None);
        REQUIRE(capability.sessionGeneration == 0);
        REQUIRE(capability.maximumSemanticSchema == 0);
        REQUIRE(capability.maximumQueuedBytes == 0);
        UiAccessibilityActionRequest action{};
        action.node = snapshot.Nodes()[2].id;
        const auto original = action.node;
        UiAccessibilityAnnouncementCursor cursor{tree.Instance(), tree.Canvas(), UiAccessibilityAnnouncementGeneration::Create(1).Value(),
                                                 17};
        const auto originalCursor = cursor;
        Platform::IPlatformAccessibilityBridge &transport = bridge;
        const auto before = accessibilityAllocations.load();
        const auto result = bridge.Submit(snapshot, publisher, UiPresentationReceipt{});
        const auto produced = bridge.TryDequeueAction(action);
        const auto completed = transport.TryDequeueAnnouncementCompletion(cursor);
        const auto after = accessibilityAllocations.load();
        REQUIRE(result == Platform::AccessibilityEnqueueResult::Unsupported);
        REQUIRE_FALSE(produced);
        REQUIRE_FALSE(completed);
        REQUIRE(action.node == original);
        REQUIRE(cursor == originalCursor);
        REQUIRE(after == before);
        bridge.Shutdown();
        bridge.Shutdown();
        REQUIRE(bridge.Submit(snapshot, publisher, UiPresentationReceipt{}) == Platform::AccessibilityEnqueueResult::ShuttingDown);
        REQUIRE_FALSE(transport.TryDequeueAnnouncementCompletion(cursor));
        REQUIRE(cursor == originalCursor);
        REQUIRE(publisher.Announcements().empty());
    }
}  // namespace Horo::Runtime::Ui
