#include "Horo/Debug/ConsolePresentation.h"
#include "Horo/Foundation/Utf8.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <utility>

using namespace Horo;
using namespace Horo::Debug;

namespace {
    /** @brief Owned query fixture; the adapter receives the same independent snapshot values as a real process store. */
    struct OperationSource final : IOperationQuery {
        OperationStoreSnapshot value;
        mutable std::size_t queries{};

        OperationSource() {
            value.revision = 1;
            value.activeCapacity = 4;
            value.recentCapacity = 4;
            OperationRecord record;
            record.id = 1;
            record.state = OperationState::Running;
            record.title = "valid";
            record.progress = 0.5F;
            value.operations.push_back(std::move(record));
        }

        std::optional<OperationStoreSnapshot> SnapshotIfChanged(std::uint64_t revision) const override {
            ++queries;
            if (revision == value.revision)
                return {};
            return value;
        }
    };
}  // namespace

TEST_CASE("Console operation rows bound UTF8 fields and copied rows retain their original immutable source", "[console][overlays]") {
    OperationSource operations;
    auto &record = operations.value.operations[0];
    record.title = std::string(511, 'x') + "\xc3\xa9";
    record.phase = std::string(127, 'x') + "\xc3\xa9";
    record.message = std::string(4095, 'x') + "\xc3\xa9";
    record.error = MakeCommandError(CommandError::HandlerFailed);
    record.error->message = "private exception payload";
    Log::StructuredLogStore logs(4);
    ConsoleStoreOverlays model(logs, operations);
    const auto now = std::chrono::steady_clock::time_point{};
    model.SetVisible(false, true);
    REQUIRE(model.Refresh(now).HasValue());
    REQUIRE(model.Jobs(0, 1000).size() == 1);
    std::optional<ConsoleOperationRow> retained = model.Jobs(0, 1).front();
    CHECK(retained->source->id == 1);
    CHECK(retained->title.size() == 511);
    CHECK(retained->phase.size() == 127);
    CHECK(retained->message.size() == 4095);
    CHECK(retained->truncated);
    CHECK(IsValidUtf8ScalarSequence(retained->title));
    CHECK(IsValidUtf8ScalarSequence(retained->phase));
    CHECK(IsValidUtf8ScalarSequence(retained->message));
    CHECK(retained->errorCode == "console.handler_failed");
    CHECK(retained->message.find("private exception") == std::string_view::npos);
    const auto revision = model.JobProjectionRevision();
    const std::weak_ptr<const OperationStoreSnapshot> lifetime = retained->snapshot;
    model.SetVisible(false, false);
    CHECK(model.Jobs(0, 1).empty());
    CHECK(model.JobProjectionRevision() > revision);
    CHECK_FALSE(lifetime.expired());
    CHECK(retained->source->title.size() == 513);
    retained.reset();
    CHECK(lifetime.expired());
    CHECK(operations.queries == 1);
}

TEST_CASE("Console operation projection rejection preserves accepted source revision and independent log refresh", "[console][overlays]") {
    OperationSource operations;
    Log::StructuredLogStore logs(4);
    ConsoleStoreOverlays model(logs, operations);
    model.SetVisible(true, true);
    auto now = std::chrono::steady_clock::time_point{};
    REQUIRE(model.Refresh(now).HasValue());
    const auto accepted = model.JobProjectionRevision();
    operations.value.revision = 2;
    operations.value.operations[0].progress = std::numeric_limits<float>::quiet_NaN();
    logs.Append({0, {}, Log::Level::Info, "game", "still refreshes", {}});
    now += std::chrono::milliseconds{250};
    CHECK(model.Refresh(now).HasError());
    CHECK(model.JobProjectionRevision() == accepted);
    REQUIRE(model.Jobs(0, 1).size() == 1);
    CHECK(model.Jobs(0, 1)[0].source->progress == 0.5F);
    CHECK(model.LogRowCount() == 1);
    operations.value.operations[0].progress = 0.75F;
    now += std::chrono::milliseconds{250};
    REQUIRE(model.Refresh(now).HasValue());
    CHECK(model.JobProjectionRevision() > accepted);
    CHECK(model.Jobs(0, 1)[0].source->progress == 0.75F);
    const auto recovered = model.JobProjectionRevision();
    for (const auto invalid : {0, 1, 2, 3, 4}) {
        ++operations.value.revision;
        operations.value.operations.resize(1);
        auto &record = operations.value.operations[0];
        record.title = "valid";
        record.id = 1;
        record.state = OperationState::Running;
        record.kind = OperationKind::Other;
        record.progress = 0.5F;
        switch (invalid) {
            case 0:
                record.id = 0;
                break;
            case 1:
                record.state = static_cast<OperationState>(255);
                break;
            case 2:
                record.title = "\xff";
                break;
            case 3:
                operations.value.operations.push_back(record);
                break;
            case 4:
                record.kind = static_cast<OperationKind>(255);
                break;
        }
        now += std::chrono::milliseconds{250};
        CHECK(model.Refresh(now).HasError());
        CHECK(model.JobProjectionRevision() == recovered);
        CHECK(model.JobRowCount() == 1);
        CHECK(model.Jobs(0, 1)[0].title == "valid");
    }
}

TEST_CASE("Console operation projection rejects impossible source capacity without dropping accepted rows", "[console][overlays]") {
    OperationSource operations;
    Log::StructuredLogStore logs(4);
    ConsoleStoreOverlays model(logs, operations);
    model.SetVisible(false, true);
    auto now = std::chrono::steady_clock::time_point{};
    REQUIRE(model.Refresh(now).HasValue());
    const auto accepted = model.JobProjectionRevision();
    for (const auto capacities : {std::pair<std::size_t, std::size_t>{0, 0}, {4097, 0}, {4096, 1}}) {
        ++operations.value.revision;
        operations.value.activeCapacity = capacities.first;
        operations.value.recentCapacity = capacities.second;
        now += std::chrono::milliseconds{250};
        CHECK(model.Refresh(now).HasError());
        CHECK(model.JobProjectionRevision() == accepted);
        CHECK(model.JobRowCount() == 1);
        CHECK(model.Jobs(0, 1)[0].source->id == 1);
    }
    operations.value.activeCapacity = 1;
    operations.value.recentCapacity = 0;
    now += std::chrono::milliseconds{250};
    REQUIRE(model.Refresh(now).HasValue());
    CHECK(model.JobProjectionRevision() > accepted);
}
