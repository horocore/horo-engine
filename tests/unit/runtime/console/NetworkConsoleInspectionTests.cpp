#include "NetworkConsoleInspection.h"
#include "NetworkConsoleOverlay.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <thread>

using namespace Horo;
using namespace Horo::Application;
using namespace Horo::Application::Internal;
using namespace Horo::Debug;

namespace {
    /** @brief Observes audit admission in this single-threaded submit fixture without a production log store. */
    struct ObservedAudit final : ICommandAudit {
        bool accepts{};
        std::size_t records{};
        std::string text;

        /** @copydoc ICommandAudit::Record */
        Result<void> Record(DebugCommandId, std::string_view redacted) override {
            ++records;
            text = redacted;
            return accepts ? Result<void>::Success() : Result<void>::Failure(MakeCommandError(CommandError::MissingCapability));
        }
    };

    /** @brief Counts reads of the real application owner; malformed mode exercises only boundary rejection. */
    struct ObservedQuery final : INetworkDebuggerQuery {
        enum class Malformed {
            None,
            Capacity,
            State,
            Revision
        };
        std::shared_ptr<NetworkDebuggerService> service{std::make_shared<NetworkDebuggerService>(std::chrono::hours{1})};
        mutable std::size_t queries{};
        Malformed malformed{};
        bool throws{};

        NetworkDebuggerProjection Query() const override {
            ++queries;
            if (throws)
                throw std::runtime_error("private producer exception");
            auto projection = service->Query();
            if (malformed == Malformed::Capacity)
                projection.snapshot.capture.size = projection.snapshot.capture.records.size() + 1;
            if (malformed == Malformed::State)
                projection.state = static_cast<NetworkDebuggerState>(255);
            if (malformed == Malformed::Revision)
                projection.snapshot.revision = 0;
            return projection;
        }
    };

    /** @brief Builds only explicitly requested optional application metadata, independently of product-default catalogs. */
    std::shared_ptr<CommandRegistry> Registry(ProductProfile profile = ProductProfile::Editor) {
        RegistryPolicy policy;
        policy.profile = profile;
        const std::array descriptors{NetworkConsoleStatusDescriptor()};
        auto created = CommandRegistry::Create(descriptors, policy);
        REQUIRE(created.HasValue());
        return std::make_shared<CommandRegistry>(std::move(created).Value());
    }
}  // namespace

TEST_CASE("Network console inspection binds inertly and uses the same permission owner queue and operation authority",
          "[console][network][host]") {
    auto query = std::make_shared<ObservedQuery>();
    auto registry = Registry();
    auto bindings = BindNetworkConsoleInspection(registry, query);
    REQUIRE(bindings.HasValue());
    REQUIRE(bindings.Value().size() == 1);
    CHECK(query->queries == 0);
    CHECK(BindNetworkConsoleInspection(registry, {}).HasError());
    const auto shipping = BindNetworkConsoleInspection(Registry(ProductProfile::Shipping), {});
    REQUIRE(shipping.HasValue());
    CHECK(shipping.Value().empty());
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(32, 32);
    CHECK(CommandExecutor::Create(registry, bindings.Value(), jobs, operations, std::this_thread::get_id(), std::this_thread::get_id())
              .HasError());
    auto audit = std::make_shared<ObservedAudit>();
    auto made = CommandExecutor::Create(registry, bindings.Value(), jobs, operations, std::this_thread::get_id(),
                                        std::this_thread::get_id(), 128, audit);
    REQUIRE(made.HasValue());
    auto executor = std::move(made).Value();
    CHECK(executor->Submit("net.debug_status", {}).HasError());
    CHECK(executor->Submit("net.debug_status private-payload", {3}).HasError());
    CHECK(query->queries == 0);
    CHECK(audit->records == 0);
    CHECK_FALSE(operations->SnapshotIfChanged(0));
    CHECK(executor->Submit("net.debug_status", {3}).HasError());
    CHECK(audit->records == 1);
    CHECK(audit->text == "net.debug_status");
    CHECK(query->queries == 0);
    CHECK_FALSE(operations->SnapshotIfChanged(0));
    audit->accepts = true;
    const auto inspect = [&] {
        const auto accepted = executor->Submit("net.debug_status", {3});
        REQUIRE(accepted.HasValue());
        CHECK(audit->text == "net.debug_status");
        REQUIRE(accepted.Value().operation);
        const auto before = query->queries;
        REQUIRE(executor->DrainRender(1).HasValue());
        CHECK(query->queries == before);
        REQUIRE(executor->DrainOwner(1).HasValue());
        CHECK(query->queries == before + 1);
        const auto snapshot = operations->SnapshotIfChanged(0);
        REQUIRE(snapshot);
        const auto record = std::ranges::find(snapshot->operations, *accepted.Value().operation, &OperationRecord::id);
        REQUIRE(record != snapshot->operations.end());
        REQUIRE(record->state == OperationState::Succeeded);
        CHECK(record->message.size() <= registry->Limits().outputBytes);
        return record->message;
    };
    CHECK(inspect().find("state=detached") != std::string::npos);
    REQUIRE(query->service->Begin(1, 1, false));
    CHECK(inspect().find("state=disabled") != std::string::npos);
    REQUIRE(query->service->Begin(1, 2, true, Network::NetworkDiagnosticProvider::Deterministic));
    const auto first = query->service->Query().snapshot.source;
    CHECK(inspect().find("state=stale") != std::string::npos);
    REQUIRE(query->service->Publish(first));
    const auto live = inspect();
    CHECK(live.find("state=live") != std::string::npos);
    CHECK(live.find("provider=deterministic") != std::string::npos);
    auto pending = executor->Submit("net.debug_status", {3});
    REQUIRE(pending.HasValue());
    REQUIRE(pending.Value().operation);
    REQUIRE(query->service->Begin(2, 3, true));
    const auto replacement = query->service->Query().snapshot.source;
    REQUIRE(query->service->Publish(replacement));
    REQUIRE(executor->DrainOwner(1).HasValue());
    const auto replaced = operations->SnapshotIfChanged(0);
    REQUIRE(replaced);
    const auto result = std::ranges::find(replaced->operations, *pending.Value().operation, &OperationRecord::id);
    REQUIRE(result != replaced->operations.end());
    CHECK(result->state == OperationState::Succeeded);
    CHECK(result->message.find(" scene=2 generation=3") != std::string::npos);
    query->service->Producer().Detach();
    CHECK(inspect().find("state=detached") != std::string::npos);
    const auto beforeStop = query->queries;
    REQUIRE(executor->Submit("net.debug_status", {3}).HasValue());
    executor->Stop();
    CHECK(executor->DrainOwner(1).HasError());
    CHECK(query->queries == beforeStop);
}

TEST_CASE("Network console handlers reject foreign cancelled and malformed inspection without leaking partial output",
          "[console][network][host]") {
    auto query = std::make_shared<ObservedQuery>();
    auto made = BindNetworkConsoleInspection(Registry(), query);
    REQUIRE(made.HasValue());
    auto bindings = std::move(made).Value();
    auto handler = bindings.front().handler;
    bool rejected{};
    std::thread foreign([&] {
        rejected = handler->Execute({3}, {}, {}, nullptr).HasError();
    });
    foreign.join();
    CHECK(rejected);
    CHECK(query->queries == 0);
    CancellationSource canceled;
    canceled.RequestCancellation();
    CHECK(handler->Execute({3}, {}, canceled.Token(), nullptr).HasError());
    CHECK(query->queries == 0);
    REQUIRE(query->service->Begin(1, 1, true));
    for (const auto malformed : {ObservedQuery::Malformed::Capacity, ObservedQuery::Malformed::State, ObservedQuery::Malformed::Revision}) {
        query->malformed = malformed;
        CHECK(handler->Execute({3}, {}, {}, nullptr).HasError());
    }
    CHECK(query->queries == 3);
    query->service->Producer().Detach();
    std::weak_ptr<ObservedQuery> lifetime = query;
    query.reset();
    CHECK_FALSE(lifetime.expired());
    handler.reset();
    bindings.clear();
    CHECK(lifetime.expired());
}

TEST_CASE("Network overlay reads only visible bounded summaries at four Hz and preserves valid state on failed queries",
          "[console][network][overlay]") {
    auto query = std::make_shared<ObservedQuery>();
    auto made = NetworkConsoleOverlay::Create(query, ProductProfile::Editor, {3});
    REQUIRE(made.HasValue());
    auto overlay = std::move(made).Value();
    CHECK(query->queries == 0);
    CHECK_FALSE(overlay.Snapshot());
    CHECK_FALSE(overlay.Refresh({}).Value());
    CHECK(query->queries == 0);
    REQUIRE(overlay.SetVisible(true).HasValue());
    REQUIRE(overlay.Refresh({}).Value());
    REQUIRE(overlay.Snapshot());
    CHECK(overlay.Snapshot()->state == NetworkDebuggerState::Detached);
    const auto revision = overlay.Revision();
    const auto at = [](const int milliseconds) {
        return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds{milliseconds};
    };
    CHECK_FALSE(overlay.Refresh(at(249)).Value());
    CHECK(query->queries == 1);
    CHECK_FALSE(overlay.Refresh(at(250)).Value());
    CHECK(query->queries == 2);
    CHECK(overlay.Revision() == revision);
    REQUIRE(query->service->Begin(3, 4, true, Network::NetworkDiagnosticProvider::Deterministic));
    const auto source = query->service->Query().snapshot.source;
    REQUIRE(query->service->Producer().Observe(source, Network::NetworkRpcRecord{1, 1, 0, 0}));
    REQUIRE(query->service->Publish(source));
    CHECK_FALSE(overlay.Refresh(at(499)).Value());
    REQUIRE(overlay.Refresh(at(500)).Value());
    REQUIRE(overlay.Snapshot());
    CHECK(overlay.Snapshot()->source == source);
    CHECK(overlay.Snapshot()->state == NetworkDebuggerState::Live);
    CHECK(overlay.Snapshot()->provider == Network::NetworkDiagnosticProvider::Deterministic);
    CHECK(overlay.Snapshot()->records[2] == 1);
    const auto valid = *overlay.Snapshot();
    const auto validRevision = overlay.Revision();
    query->malformed = ObservedQuery::Malformed::Capacity;
    CHECK(overlay.Refresh(at(750)).HasError());
    CHECK(overlay.RefreshFailure());
    CHECK(*overlay.Snapshot() == valid);
    CHECK(overlay.Revision() == validRevision);
    query->malformed = ObservedQuery::Malformed::None;
    query->throws = true;
    CHECK_FALSE(overlay.Refresh(at(751)).Value());
    CHECK(overlay.RefreshFailure());
    const auto failed = overlay.Refresh(at(1000));
    REQUIRE(failed.HasError());
    CHECK(failed.ErrorValue().message.find("private producer exception") == std::string::npos);
    CHECK(*overlay.Snapshot() == valid);
    CHECK(overlay.Revision() == validRevision);
    query->throws = false;
    REQUIRE(overlay.SetVisible(false).HasValue());
    CHECK_FALSE(overlay.RefreshFailure());
    CHECK_FALSE(overlay.Snapshot());
    const auto beforeHidden = query->queries;
    query->service->Producer().Detach();
    CHECK_FALSE(overlay.Refresh(at(2000)).Value());
    CHECK(query->queries == beforeHidden);
    REQUIRE(overlay.SetVisible(true).HasValue());
    REQUIRE(overlay.Refresh(at(2000)).Value());
    CHECK(overlay.Snapshot()->state == NetworkDebuggerState::Detached);
}

TEST_CASE("Network overlay admission and owner fences do not query or activate a service", "[console][network][overlay]") {
    auto query = std::make_shared<ObservedQuery>();
    CHECK(NetworkConsoleOverlay::Create({}, ProductProfile::Editor, {3}).HasError());
    CHECK(NetworkConsoleOverlay::Create(query, ProductProfile::Editor, {}).HasError());
    CHECK(NetworkConsoleOverlay::Create(query, ProductProfile::Shipping, {15, true, true}).HasError());
    CHECK(NetworkConsoleOverlay::Create(query, ProductProfile::Server, {15, true, true}).HasError());
    auto made = NetworkConsoleOverlay::Create(query, ProductProfile::Development, {3});
    REQUIRE(made.HasValue());
    auto overlay = std::move(made).Value();
    bool rejected{};
    std::thread foreign([&] {
        rejected = overlay.SetVisible(true).HasError() && overlay.Refresh({}).HasError();
    });
    foreign.join();
    CHECK(rejected);
    CHECK(query->queries == 0);
    CHECK_FALSE(query->service->Query().snapshot.attached);
    CHECK_FALSE(overlay.Snapshot());
}
