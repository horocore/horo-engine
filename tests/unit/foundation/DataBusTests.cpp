#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorEngineEventBridge.h"
#include "Horo/Editor/EditorSurfaceEventContext.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/ProcessEvents.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
    struct EnginePingEvent {
        static constexpr std::string_view HoroEventTypeName = "horo.tests.EnginePingEvent";
        int value = 0;
    };

    struct EditorSelectionInvalidatedEvent {
        static constexpr std::string_view HoroEventTypeName = "horo.tests.EditorSelectionInvalidatedEvent";
        int revision = 0;
    };

    struct QueueEvent {
        static constexpr std::string_view HoroEventTypeName = "horo.tests.QueueEvent";
        int value = 0;
    };

    struct UnlistedProcessEvent {
        static constexpr std::string_view HoroEventTypeName = "horo.tests.UnlistedProcessEvent";
    };

    template <typename EventT, typename ValueFn>
    void AddBridgeSubscription(Horo::Editor::EditorDataBus &editorEvents, std::array<Horo::Subscription, 9> &subscriptions,
                               std::array<int, 9> &delivered, const std::size_t index, ValueFn value) {
        subscriptions[index] = editorEvents.Subscribe<EventT>([&delivered, index, value = std::move(value)](const EventT &event) {
            delivered[index] = static_cast<int>(value(event));
        });
    }

    void SubscribeToAllBridgeEvents(Horo::Editor::EditorDataBus &editorEvents, std::array<Horo::Subscription, 9> &subscriptions,
                                    std::array<int, 9> &delivered) {
        AddBridgeSubscription<Horo::Editor::EditorProjectOpenedEvent>(editorEvents, subscriptions, delivered, 0, [](const auto &event) {
            return event.revision;
        });
        AddBridgeSubscription<Horo::Editor::EditorProjectClosedEvent>(editorEvents, subscriptions, delivered, 1, [](const auto &event) {
            return event.revision;
        });
        AddBridgeSubscription<Horo::Editor::EditorAssetImportedEvent>(editorEvents, subscriptions, delivered, 2, [](const auto &event) {
            return event.revision;
        });
        AddBridgeSubscription<Horo::Editor::EditorAssetReloadedEvent>(editorEvents, subscriptions, delivered, 3, [](const auto &event) {
            return event.revision;
        });
        AddBridgeSubscription<Horo::Editor::EditorOperationStoreRevisionChangedEvent>(editorEvents, subscriptions, delivered, 4,
                                                                                      [](const auto &event) {
            return event.revision;
        });
        AddBridgeSubscription<Horo::Editor::EditorConsoleLogEvent>(editorEvents, subscriptions, delivered, 5, [](const auto &event) {
            return event.revision;
        });
        AddBridgeSubscription<Horo::Editor::EditorMetricsChangedEvent>(editorEvents, subscriptions, delivered, 6, [](const auto &event) {
            return event.revision;
        });
        AddBridgeSubscription<Horo::Editor::EditorProfilerCaptureStateEvent>(editorEvents, subscriptions, delivered, 7,
                                                                             [](const auto &event) {
            return event.captureId;
        });
        AddBridgeSubscription<Horo::Editor::EditorMcpToolInvocationEvent>(editorEvents, subscriptions, delivered, 8, [](const auto &event) {
            return event.historyRevision;
        });
    }

    void PublishAllProcessEvents(Horo::EngineDataBus &engineEvents) {
        engineEvents.Publish(Horo::ProjectOpenedEvent{.revision = 1});
        engineEvents.Publish(Horo::ProjectClosedEvent{.revision = 2});
        engineEvents.Publish(Horo::AssetImportedEvent{.revision = 3, .assetId = "asset.imported"});
        engineEvents.Publish(Horo::AssetReloadedEvent{.revision = 4, .assetId = "asset.reloaded"});
        engineEvents.Publish(Horo::OperationStoreRevisionChangedEvent{
            .revision = 5,
            .changedOperation = Horo::OperationId{7},
            .state = Horo::OperationState::Running,
        });
        engineEvents.Publish(Horo::ConsoleLogEvent{.revision = 6, .appendedCount = 2, .category = "test", .message = "private"});
        engineEvents.Publish(Horo::MetricsChangedEvent{.revision = 7, .changedGroups = 3});
        engineEvents.Publish(Horo::ProfilerCaptureStateEvent{
            .captureId = 8,
            .state = Horo::ProfilerCaptureState::Complete,
            .failureReason = "private",
        });
        engineEvents.Publish(Horo::McpToolInvocationEvent{
            .requestId = 9,
            .toolName = "private-tool",
            .operationId = 10,
            .state = Horo::OperationState::Succeeded,
            .historyRevision = 11,
            .arguments = "private",
        });
    }

    void PublishRejectedProcessEvents(Horo::EngineDataBus &engineEvents) {
        engineEvents.Publish(Horo::AssetReloadedEvent{.assetId = "../private"});
        engineEvents.Publish(Horo::OperationStoreRevisionChangedEvent{.state = static_cast<Horo::OperationState>(255)});
        engineEvents.Publish(Horo::ProfilerCaptureStateEvent{.state = static_cast<Horo::ProfilerCaptureState>(255)});
        engineEvents.Publish(Horo::McpToolInvocationEvent{.state = static_cast<Horo::OperationState>(255)});
    }

    template <typename EventT, typename ValueFn>
    void AddSurfaceSubscription(Horo::Editor::EditorSurfaceEventContext &context, std::vector<Horo::Subscription> &tokens,
                                std::array<int, 9> &delivered, const std::size_t index, ValueFn value) {
        auto result = context.Subscribe<EventT>([&delivered, index, value = std::move(value)](const EventT &event) {
            delivered[index] = static_cast<int>(value(event));
        });
        REQUIRE(result.HasValue());
        tokens.push_back(std::move(result).Value());
    }

    std::vector<Horo::Subscription> SubscribeToAllSurfaceEvents(Horo::Editor::EditorSurfaceEventContext &context,
                                                                std::array<int, 9> &delivered) {
        std::vector<Horo::Subscription> tokens;
        tokens.reserve(9);
        AddSurfaceSubscription<Horo::Editor::EditorProjectOpenedEvent>(context, tokens, delivered, 0, [](const auto &event) {
            return event.revision;
        });
        AddSurfaceSubscription<Horo::Editor::EditorProjectClosedEvent>(context, tokens, delivered, 1, [](const auto &event) {
            return event.revision;
        });
        AddSurfaceSubscription<Horo::Editor::EditorAssetImportedEvent>(context, tokens, delivered, 2, [](const auto &event) {
            return event.revision;
        });
        AddSurfaceSubscription<Horo::Editor::EditorAssetReloadedEvent>(context, tokens, delivered, 3, [](const auto &event) {
            return event.revision;
        });
        AddSurfaceSubscription<Horo::Editor::EditorOperationStoreRevisionChangedEvent>(context, tokens, delivered, 4,
                                                                                       [](const auto &event) {
            return event.revision;
        });
        AddSurfaceSubscription<Horo::Editor::EditorConsoleLogEvent>(context, tokens, delivered, 5, [](const auto &event) {
            return event.revision;
        });
        AddSurfaceSubscription<Horo::Editor::EditorMetricsChangedEvent>(context, tokens, delivered, 6, [](const auto &event) {
            return event.revision;
        });
        AddSurfaceSubscription<Horo::Editor::EditorProfilerCaptureStateEvent>(context, tokens, delivered, 7, [](const auto &event) {
            return event.captureId;
        });
        AddSurfaceSubscription<Horo::Editor::EditorMcpToolInvocationEvent>(context, tokens, delivered, 8, [](const auto &event) {
            return event.historyRevision;
        });
        return tokens;
    }

    void PublishAllSurfaceEvents(Horo::Editor::EditorDataBus &editorEvents) {
        editorEvents.Publish(Horo::Editor::EditorProjectOpenedEvent{.revision = 1});
        editorEvents.Publish(Horo::Editor::EditorProjectClosedEvent{.revision = 2});
        editorEvents.Publish(Horo::Editor::EditorAssetImportedEvent{.revision = 3, .assetId = "asset.imported"});
        editorEvents.Publish(Horo::Editor::EditorAssetReloadedEvent{.revision = 4, .assetId = "asset.reloaded"});
        editorEvents.Publish(Horo::Editor::EditorOperationStoreRevisionChangedEvent{
            .revision = 5,
            .changedOperation = Horo::OperationId{7},
            .state = Horo::OperationState::Running,
        });
        editorEvents.Publish(Horo::Editor::EditorConsoleLogEvent{.revision = 6, .appendedCount = 2});
        editorEvents.Publish(Horo::Editor::EditorMetricsChangedEvent{.revision = 7, .changedGroups = 3});
        editorEvents.Publish(Horo::Editor::EditorProfilerCaptureStateEvent{
            .captureId = 8,
            .state = Horo::ProfilerCaptureState::Complete,
        });
        editorEvents.Publish(Horo::Editor::EditorMcpToolInvocationEvent{
            .state = Horo::OperationState::Succeeded,
            .historyRevision = 9,
        });
    }

    TEST_CASE("Subscription Token Detaches Handler", "[unit][foundation]") {
        Horo::EngineDataBus bus;
        int handled = 0;
        {
            auto subscription = bus.Subscribe<EnginePingEvent>([&handled](const EnginePingEvent &) {
                ++handled;
            });
            bus.Publish(EnginePingEvent{.value = 1});
            REQUIRE((handled == 1));
        }

        bus.Publish(EnginePingEvent{.value = 2});
        REQUIRE((handled == 1));
    }

    TEST_CASE("Publish Targets Matching Event Type Only", "[unit][foundation]") {
        Horo::EngineDataBus bus;
        int engineHandled = 0;
        int editorHandled = 0;
        auto engineSubscription = bus.Subscribe<EnginePingEvent>([&engineHandled](const EnginePingEvent &) {
            ++engineHandled;
        });
        auto editorSubscription = bus.Subscribe<EditorSelectionInvalidatedEvent>([&editorHandled](const EditorSelectionInvalidatedEvent &) {
            ++editorHandled;
        });

        bus.Publish(EnginePingEvent{.value = 9});
        REQUIRE((engineHandled == 1));
        REQUIRE((editorHandled == 0));
    }

    TEST_CASE("Handler Failure Is Isolated", "[unit][foundation]") {
        Horo::EngineDataBus bus;
        int handled = 0;
        auto throwingSubscription = bus.Subscribe<EnginePingEvent>([](const EnginePingEvent &) {
            throw std::runtime_error{"expected test failure"};
        });
        auto healthySubscription = bus.Subscribe<EnginePingEvent>([&handled](const EnginePingEvent &) {
            ++handled;
        });

        bus.Publish(EnginePingEvent{});
        REQUIRE((handled == 1));
    }

    TEST_CASE("Async Events Dispatch At The Explicit Synchronization Point", "[unit][foundation]") {
        Horo::EngineDataBus bus;
        int handled = 0;
        auto subscription = bus.Subscribe<EnginePingEvent>([&handled](const EnginePingEvent &event) {
            handled = event.value;
        });

        bus.PublishAsync(EnginePingEvent{.value = 42});
        REQUIRE((handled == 0));
        bus.DispatchQueued();
        REQUIRE((handled == 42));
    }

    TEST_CASE("Editor Bus Does Not Share Engine Handlers", "[unit][foundation]") {
        Horo::EngineDataBus engineBus;
        Horo::Editor::EditorDataBus editorBus;
        int engineHandled = 0;
        int editorHandled = 0;
        auto engineSubscription =
            engineBus.Subscribe<EditorSelectionInvalidatedEvent>([&engineHandled](const EditorSelectionInvalidatedEvent &) {
            ++engineHandled;
        });
        auto editorSubscription =
            editorBus.Subscribe<EditorSelectionInvalidatedEvent>([&editorHandled](const EditorSelectionInvalidatedEvent &) {
            ++editorHandled;
        });

        editorBus.Publish(EditorSelectionInvalidatedEvent{.revision = 1});
        REQUIRE((engineHandled == 0));
        REQUIRE((editorHandled == 1));
    }

    TEST_CASE("Same Type Recursion Is Deferred Until Current Delivery Completes", "[unit][foundation]") {
        Horo::Editor::EditorDataBus bus;
        std::vector<int> order;
        auto firstSubscription = bus.Subscribe<EditorSelectionInvalidatedEvent>([&](const EditorSelectionInvalidatedEvent &event) {
            order.push_back(event.revision);
            if (event.revision == 1) {
                bus.Publish(EditorSelectionInvalidatedEvent{.revision = 2});
            }
        });
        auto secondSubscription = bus.Subscribe<EditorSelectionInvalidatedEvent>([&](const EditorSelectionInvalidatedEvent &event) {
            order.push_back(event.revision * 10);
        });

        bus.Publish(EditorSelectionInvalidatedEvent{.revision = 1});
        REQUIRE((order == std::vector{1, 10, 2, 20}));
    }

    TEST_CASE("Engine Bus Bounds Active Subscriptions", "[unit][foundation][data_bus]") {
        Horo::EngineDataBusConfig config;
        config.maxSubscriptions = 1;
        Horo::EngineDataBus bus{config};
        int handled = 0;
        auto first = bus.Subscribe<EnginePingEvent>([&handled](const EnginePingEvent &) {
            ++handled;
        });
        auto second = bus.Subscribe<EditorSelectionInvalidatedEvent>([](const EditorSelectionInvalidatedEvent &) {
        });

        REQUIRE(static_cast<bool>(first));
        CHECK_FALSE(static_cast<bool>(second));
        CHECK(bus.QueueStats().activeSubscriptions == 1);
        bus.Publish(EnginePingEvent{});
        CHECK(handled == 1);

        first.Reset();
        CHECK(bus.QueueStats().activeSubscriptions == 0);
    }

    TEST_CASE("Async Queue Reports And Applies Backpressure Policies", "[unit][foundation][data_bus]") {
        const auto run = [](const Horo::BackpressurePolicy policy) {
            Horo::EngineDataBusConfig config;
            config.maxAsyncQueueSize = 2;
            config.defaultBackpressurePolicy = policy;
            Horo::EngineDataBus bus{config};
            std::vector<int> values;
            auto subscription = bus.Subscribe<QueueEvent>([&values](const QueueEvent &event) {
                values.push_back(event.value);
            });
            bus.PublishAsync(QueueEvent{.value = 1});
            bus.PublishAsync(QueueEvent{.value = 2});
            bus.PublishAsync(QueueEvent{.value = 3});
            const Horo::EngineDataBusQueueStats beforeDispatch = bus.QueueStats();
            bus.DispatchQueued();
            return std::pair{std::move(values), std::pair{beforeDispatch, bus.QueueStats()}};
        };

        const auto dropNewest = run(Horo::BackpressurePolicy::DropNewest);
        CHECK(dropNewest.first == std::vector{1, 2});
        CHECK(dropNewest.second.first.droppedNewest == 1);
        CHECK(dropNewest.second.first.droppedOldest == 0);

        const auto dropOldest = run(Horo::BackpressurePolicy::DropOldest);
        CHECK(dropOldest.first == std::vector{2, 3});
        CHECK(dropOldest.second.first.droppedOldest == 1);

        const auto merge = run(Horo::BackpressurePolicy::Merge);
        CHECK(merge.first == std::vector{1, 3});
        CHECK(merge.second.first.merged == 1);
        CHECK(merge.second.first.enqueued == 2);
        CHECK(merge.second.second.dispatched == 2);
    }

    TEST_CASE("Process Bridge Allowlist Sanitizes And Detaches", "[unit][editor][data_bus]") {
        Horo::EngineDataBus engineEvents;
        Horo::Editor::EditorDataBus editorEvents;
        Horo::Editor::EditorEngineEventBridge bridge{engineEvents, editorEvents};
        int imported = 0;
        int opened = 0;
        std::string assetId;
        auto subscription = editorEvents.Subscribe<Horo::Editor::EditorAssetImportedEvent>([&](const auto &event) {
            ++imported;
            assetId = event.assetId;
        });
        auto openedSubscription = editorEvents.Subscribe<Horo::Editor::EditorProjectOpenedEvent>([&](const auto &) {
            ++opened;
        });

        bridge.Attach();
        REQUIRE(bridge.IsAttached());
        engineEvents.PublishAsync(Horo::ProjectOpenedEvent{
            .revision = 6,
            .projectId = "private-project-id",
            .projectRoot = std::filesystem::path{"/private/secret/project"},
        });
        engineEvents.DispatchQueued();
        CHECK(opened == 1);
        engineEvents.Publish(Horo::AssetImportedEvent{
            .revision = 7,
            .assetId = "asset.hero",
            .sourcePath = std::filesystem::path{"/private/secret/source.hero"},
        });
        CHECK(imported == 1);
        CHECK(assetId == "asset.hero");

        engineEvents.Publish(Horo::AssetImportedEvent{
            .revision = 8,
            .assetId = "../private.secret",
            .sourcePath = std::filesystem::path{"/private/secret/source.hero"},
        });
        engineEvents.Publish(UnlistedProcessEvent{});
        CHECK(imported == 1);
        CHECK(bridge.Stats().forwarded == 2);
        CHECK(bridge.Stats().filtered == 1);

        bridge.Detach();
        CHECK_FALSE(bridge.IsAttached());
        engineEvents.Publish(Horo::AssetImportedEvent{.revision = 9, .assetId = "asset.after_detach"});
        CHECK(imported == 1);
    }

    TEST_CASE("Process Bridge Forwards Every Approved Event And Drops Worker Events", "[unit][editor][data_bus]") {
        Horo::EngineDataBus engineEvents;
        Horo::Editor::EditorDataBus editorEvents;
        Horo::Editor::EditorEngineEventBridge bridge{engineEvents, editorEvents};
        std::array<int, 9> delivered{};
        std::array<Horo::Subscription, 9> subscriptions;
        SubscribeToAllBridgeEvents(editorEvents, subscriptions, delivered);

        bridge.Attach();
        REQUIRE(bridge.IsAttached());
        PublishAllProcessEvents(engineEvents);

        CHECK(delivered == std::array{1, 2, 3, 4, 5, 6, 7, 8, 11});
        CHECK(bridge.Stats().forwarded == 9);

        PublishRejectedProcessEvents(engineEvents);
        CHECK(bridge.Stats().filtered == 4);

        const auto threadDropsBefore = bridge.Stats().threadDrops;
        std::thread worker([&engineEvents] {
            engineEvents.Publish(Horo::ProjectClosedEvent{.revision = 12});
        });
        worker.join();
        CHECK(bridge.Stats().threadDrops == threadDropsBefore + 1);
    }

    TEST_CASE("Surface Event Context Bounds And Revokes Provider Tokens", "[unit][editor][data_bus]") {
        Horo::Editor::EditorDataBus editorEvents;
        const std::array allowed{Horo::EditorEventKind::AssetImported};
        Horo::Editor::EditorSurfaceEventContext context{
            editorEvents,
            Horo::Editor::EditorSurfaceProviderOwnership{"com.example.tools", "com.example.editor", 3},
            allowed,
            Horo::Editor::EditorSurfaceEventContextLimits{.maximumSubscriptions = 1},
        };
        int delivered = 0;
        auto tokenResult = context.Subscribe<Horo::Editor::EditorAssetImportedEvent>([&delivered](const auto &) {
            ++delivered;
        });
        REQUIRE(tokenResult.HasValue());
        auto token = std::move(tokenResult).Value();
        CHECK(context.Stats().activeSubscriptions == 1);

        const auto overCapacity = context.Subscribe<Horo::Editor::EditorAssetImportedEvent>([](const auto &) {
        });
        REQUIRE(overCapacity.HasError());
        CHECK(overCapacity.ErrorValue().code.Value() == "surface_event_subscription_limit");
        const auto notAllowed = context.Subscribe<Horo::Editor::EditorProjectOpenedEvent>([](const auto &) {
        });
        REQUIRE(notAllowed.HasError());
        CHECK(notAllowed.ErrorValue().code.Value() == "surface_event_not_allowed");

        editorEvents.Publish(Horo::Editor::EditorAssetImportedEvent{.revision = 1, .assetId = "asset.one"});
        CHECK(delivered == 1);
        context.Close();
        CHECK(context.IsClosed());
        CHECK(context.Stats().activeSubscriptions == 0);
        editorEvents.Publish(Horo::Editor::EditorAssetImportedEvent{.revision = 2, .assetId = "asset.two"});
        CHECK(delivered == 1);
        token.Reset();

        const auto afterClose = context.Subscribe<Horo::Editor::EditorAssetImportedEvent>([](const auto &) {
        });
        REQUIRE(afterClose.HasError());
        CHECK(afterClose.ErrorValue().code.Value() == "surface_event_context_closed");
    }

    TEST_CASE("Surface Event Context Delivers Every Admitted Event Kind", "[unit][editor][data_bus]") {
        Horo::Editor::EditorDataBus editorEvents;
        const std::array allowed{
            Horo::EditorEventKind::ProjectOpened,
            Horo::EditorEventKind::ProjectClosed,
            Horo::EditorEventKind::AssetImported,
            Horo::EditorEventKind::AssetReloaded,
            Horo::EditorEventKind::OperationStoreRevisionChanged,
            Horo::EditorEventKind::ConsoleLog,
            Horo::EditorEventKind::MetricsChanged,
            Horo::EditorEventKind::ProfilerCaptureState,
            Horo::EditorEventKind::McpToolInvocation,
        };
        Horo::Editor::EditorSurfaceEventContext context{
            editorEvents,
            Horo::Editor::EditorSurfaceProviderOwnership{"com.example.tools", "com.example.editor", 5},
            allowed,
        };
        std::array<int, 9> delivered{};
        auto tokens = SubscribeToAllSurfaceEvents(context, delivered);
        PublishAllSurfaceEvents(editorEvents);

        CHECK(delivered == std::array{1, 2, 3, 4, 5, 6, 7, 8, 9});
        CHECK(context.ProviderOwnership().activationGeneration == 5);
        CHECK(context.Stats().acceptedSubscriptions == 9);
        CHECK(context.Stats().activeSubscriptions == 9);
        tokens.front().Reset();
        CHECK(context.Stats().activeSubscriptions == 8);
    }

    TEST_CASE("Surface Event Context Rejects Invalid Event And Ownership", "[unit][editor][data_bus]") {
        Horo::Editor::EditorDataBus editorEvents;
        const std::array allowed{Horo::EditorEventKind::ProjectOpened};
        Horo::Editor::EditorSurfaceEventContext context{
            editorEvents,
            Horo::Editor::EditorSurfaceProviderOwnership{},
            allowed,
        };
        CHECK(context.IsClosed());
        const auto invalid = context.Subscribe(static_cast<Horo::EditorEventKind>(255),
                                               Horo::Editor::EditorSurfaceEventHandler{[](const Horo::Editor::EditorSurfaceEvent &) {
        }});
        REQUIRE(invalid.HasError());
        CHECK(invalid.ErrorValue().code.Value() == "surface_event_context_closed");
        context.Close();

        Horo::Editor::EditorSurfaceEventContext activeContext{
            editorEvents,
            Horo::Editor::EditorSurfaceProviderOwnership{"com.example.tools", "com.example.editor", 6},
            allowed,
        };
        const auto invalidEvent =
            activeContext.Subscribe(static_cast<Horo::EditorEventKind>(255),
                                    Horo::Editor::EditorSurfaceEventHandler{[](const Horo::Editor::EditorSurfaceEvent &) {
        }});
        REQUIRE(invalidEvent.HasError());
        CHECK(invalidEvent.ErrorValue().code.Value() == "surface_event_invalid");
    }

    TEST_CASE("Surface Event Context Revocation Is Safe Across Threads", "[unit][editor][data_bus]") {
        Horo::Editor::EditorDataBus editorEvents;
        const std::array allowed{Horo::EditorEventKind::AssetImported};
        Horo::Editor::EditorSurfaceEventContext context{
            editorEvents,
            Horo::Editor::EditorSurfaceProviderOwnership{"com.example.tools", "com.example.editor", 4},
            allowed,
        };
        auto tokenResult = context.Subscribe<Horo::Editor::EditorAssetImportedEvent>([](const auto &) {
        });
        REQUIRE(tokenResult.HasValue());

        std::atomic<bool> ready{false};
        std::atomic<bool> release{false};
        std::atomic<bool> finished{false};
        std::thread revoker([token = std::move(tokenResult).Value(), &ready, &release, &finished]() mutable {
            ready.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire))
                std::this_thread::yield();
            token.Reset();
            finished.store(true, std::memory_order_release);
        });

        while (!ready.load(std::memory_order_acquire))
            std::this_thread::yield();
        release.store(true, std::memory_order_release);
        while (!finished.load(std::memory_order_acquire)) {
            auto candidate = context.Subscribe<Horo::Editor::EditorAssetImportedEvent>([](const auto &) {
            });
            if (candidate.HasValue())
                std::move(candidate).Value().Reset();
            std::this_thread::yield();
        }
        revoker.join();

        context.Close();
        CHECK(context.IsClosed());
        CHECK(context.Stats().activeSubscriptions == 0);
    }
}  // namespace
