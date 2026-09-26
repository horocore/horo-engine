#include "Horo/Cli/CliDispatcher.h"
#include "Horo/Foundation/OperationStore.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>

namespace Horo::Cli {
    TEST_CASE("CLI progress mailbox coalesces without consumer participation", "[unit][cli][progress]") {
        CliProgressMailbox mailbox;
        for (int index = 0; index < 1024; ++index)
            mailbox.Report({"cook", static_cast<float>(index) / 1024.0F, "safe"});
        const auto latest = mailbox.Take();
        REQUIRE(latest.has_value());
        REQUIRE(latest->phase == "cook");
        REQUIRE(latest->completion == 1023.0F / 1024.0F);
        REQUIRE_FALSE(mailbox.Take().has_value());
    }

    TEST_CASE("CLI human progress cadence changes only presentation frequency", "[unit][cli][progress]") {
        CliProgressCadence tty{true};
        CliProgressCadence nonTty{false, std::chrono::seconds{1}};
        const auto start = std::chrono::steady_clock::now();
        REQUIRE(tty.ShouldPresent({"cook", 0.1F, {}}, start));
        REQUIRE(tty.ShouldPresent({"cook", 0.2F, {}}, start + std::chrono::milliseconds{10}));
        REQUIRE(tty.ShouldPresent({"cook", 0.2F, "still working"}, start + std::chrono::milliseconds{11}));
        REQUIRE(nonTty.ShouldPresent({"cook", 0.1F, {}}, start));
        REQUIRE_FALSE(nonTty.ShouldPresent({"cook", 0.2F, {}}, start + std::chrono::milliseconds{10}));
        REQUIRE(nonTty.ShouldPresent({"link", 0.0F, {}}, start + std::chrono::milliseconds{20}));
        REQUIRE(nonTty.ShouldPresent({"link", 0.5F, {}}, start + std::chrono::seconds{2}));
    }

    TEST_CASE("CLI progress presentation isolates streams and human cadence", "[unit][cli][progress]") {
        const auto start = std::chrono::steady_clock::now();
        CliProgressMailbox mailbox;
        std::ostringstream output;
        std::ostringstream diagnostics;
        CliProgressPresenter human{mailbox, output, diagnostics, CliProgressOutputMode::Human, false};
        mailbox.Report({"cook", 0.1F, "begin"});
        human.Pump(start);
        mailbox.Report({"cook", 0.2F, "working"});
        human.Pump(start + std::chrono::milliseconds{10});
        CHECK(output.str().empty());
        CHECK(diagnostics.str() == "cook 10% begin\n");
        mailbox.Report({"link", 0.5F, "done"});
        human.Pump(start + std::chrono::milliseconds{20});
        CHECK(diagnostics.str() == "cook 10% begin\nlink 50% done\n");

        CliProgressPresenter json{mailbox, output, diagnostics, CliProgressOutputMode::Json, false};
        mailbox.Report({"quiet", 0.5F, {}});
        json.Pump(start);
        CHECK(output.str().empty());
        CHECK(diagnostics.str() == "cook 10% begin\nlink 50% done\n");

        CliProgressPresenter jsonl{mailbox, output, diagnostics, CliProgressOutputMode::JsonLines, false};
        mailbox.Report({"cook", 1.0F, "safe \"value\""});
        jsonl.Pump(start);
        CHECK(output.str().find("\"type\":\"progress\"") != std::string::npos);
        CHECK(output.str().find("safe \\\"value\\\"") != std::string::npos);
        CHECK(diagnostics.str() == "cook 10% begin\nlink 50% done\n");

        std::ostringstream ttyDiagnostics;
        CliProgressPresenter tty{mailbox, output, ttyDiagnostics, CliProgressOutputMode::Human, true};
        mailbox.Report({"cook", 0.25F, "line\nreset\x1b"});
        tty.Pump(start);
        CHECK(ttyDiagnostics.str().starts_with("\rcook 25%"));
        CHECK(ttyDiagnostics.str().find("\x1b") == std::string::npos);
    }

    TEST_CASE("CLI invocation stop distinguishes first interrupt escalation and timeout", "[unit][cli][cancellation]") {
        CliInvocationStopController interrupted{std::chrono::seconds{5}};
        interrupted.Interrupt();
        REQUIRE(interrupted.Token().IsCancellationRequested());
        REQUIRE_FALSE(interrupted.ForceToken().IsCancellationRequested());
        REQUIRE(interrupted.Reason() == CliStopReason::Interrupted);
        interrupted.Interrupt();
        REQUIRE(interrupted.ForceToken().IsCancellationRequested());

        CancellationSource parent;
        CliInvocationStopController child{std::chrono::seconds{5}, parent.Token()};
        parent.RequestCancellation();
        REQUIRE(child.Token().IsCancellationRequested());
        REQUIRE(child.Reason() == CliStopReason::ParentCancelled);

        CliInvocationStopController shuttingDown{std::chrono::seconds{5}};
        shuttingDown.Shutdown();
        REQUIRE(shuttingDown.Token().IsCancellationRequested());
        REQUIRE(shuttingDown.Reason() == CliStopReason::Shutdown);

        CliInvocationStopController timedOut{std::chrono::milliseconds{10}};
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        REQUIRE(timedOut.Token().IsCancellationRequested());
        REQUIRE(timedOut.Reason() == CliStopReason::TimedOut);
    }

    TEST_CASE("CLI progress projects authoritative operation snapshots", "[unit][cli][progress]") {
        OperationStore operations{4, 4};
        const auto id = operations.Begin({.kind = OperationKind::Build, .title = "Build", .phase = "prepare", .progress = 0.0F});
        REQUIRE(id.has_value());
        CliProgressProjection projection{&operations, nullptr, {.invocation = {1}, .operation = CliOperationId{*id}}};
        const auto initial = projection.Poll();
        REQUIRE(initial.has_value());
        REQUIRE(initial->phase == "prepare");
        REQUIRE_FALSE(projection.Poll().has_value());
        REQUIRE(operations.Update(*id, {.state = OperationState::Running, .phase = "cook", .progress = 0.5F}));
        const auto updated = projection.Poll();
        REQUIRE(updated.has_value());
        REQUIRE(updated->phase == "cook");
        REQUIRE(updated->completion == 0.5F);
    }
}  // namespace Horo::Cli
