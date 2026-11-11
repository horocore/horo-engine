#include "Horo/Debug/CommandExecutor.h"
#include "Horo/Foundation/Utf8.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

using namespace Horo;
using namespace Horo::Debug;

namespace {
    class Echo final : public ICommandHandler {
    public:
        int calls{};

        Result<DebugCommandOutput> Execute(const DebugCommandContext &, std::span<const ArgumentValue> arguments, const CancellationToken &,
                                           const JobExecutionContext *) override {
            ++calls;
            return Result<DebugCommandOutput>::Success({std::get<std::string>(arguments.front())});
        }
    };

    class Audit final : public ICommandAudit {
    public:
        bool reject{};
        bool throws{};
        std::string text;

        Result<void> Record(DebugCommandId, std::string_view redacted) override {
            text = redacted;
            if (throws)
                throw std::runtime_error("private exception text");
            if (reject)
                return Result<void>::Failure(MakeCommandError(CommandError::MissingCapability));
            return Result<void>::Success();
        }
    };
}  // namespace

TEST_CASE("Console audits redact sensitive values and fail closed before handler side effects", "[console][privacy]") {
    const std::array<DebugArgumentDescriptor, 1> arguments{{{"token", {}, DebugArgumentType::String, true, {}, true}}};
    const std::array descriptors{DebugCommandDescriptor{1, "game.secret", "Test privacy", "game.secret <token>", arguments,
                                                        CommandPermission::Public, CommandThreadPolicy::ImmediateConsoleThread,
                                                        CommandAvailability::ShippingAllowlist, CommandFlags::AuditLogged}};
    RegistryPolicy policy;
    policy.limits.outputBytes = 4;
    auto created = CommandRegistry::Create(descriptors, policy);
    REQUIRE(created.HasValue());
    auto registry = std::make_shared<CommandRegistry>(std::move(created).Value());
    auto handler = std::make_shared<Echo>();
    const std::array bindings{CommandBinding{1, handler}};
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(2, 2);
    CHECK(CommandExecutor::Create(registry, bindings, jobs, operations, std::this_thread::get_id(), std::this_thread::get_id()).HasError());
    auto audit = std::make_shared<Audit>();
    auto executor =
        CommandExecutor::Create(registry, bindings, jobs, operations, std::this_thread::get_id(), std::this_thread::get_id(), 2, audit);
    REQUIRE(executor.HasValue());
    auto submitted = executor.Value()->Submit("game.secret private-value", {});
    REQUIRE(submitted.HasValue());
    CHECK(handler->calls == 1);
    CHECK(submitted.Value().output.text.size() <= 4);
    CHECK(submitted.Value().redactedText.find("private-value") == std::string::npos);
    CHECK(audit->text.find("private-value") == std::string::npos);
    CHECK(audit->text.find("[REDACTED]") != std::string::npos);
    audit->reject = true;
    CHECK(executor.Value()->Submit("game.secret private-value", {}).HasError());
    CHECK(handler->calls == 1);
    audit->reject = false;
    audit->throws = true;
    auto thrown = executor.Value()->Submit("game.secret private-value", {});
    REQUIRE(thrown.HasError());
    CHECK(thrown.ErrorValue().code.Value() == "console.handler_failed");
    CHECK(handler->calls == 1);
    audit->text.clear();
    executor.Value()->Stop();
    CHECK(executor.Value()->Submit("game.secret private-value", {}).HasError());
    CHECK(audit->text.empty());
}

TEST_CASE("Console bounded history preserves UTF-8 scalar boundaries", "[console][privacy]") {
    ConsoleLimits limits;
    limits.inputBytes = 3;
    limits.outputBytes = 3;
    limits.totalHistoryBytes = 4;
    ConsoleHistory history(limits);
    CommandInvocation invocation{1, {}, "\xC3\xA9\xC3\xA9"};
    history.Append(invocation, "\xC3\xA9\xC3\xA9");
    REQUIRE(history.Entries().size() == 1);
    CHECK(history.Entries()[0].command.size() == 2);
    CHECK(history.Entries()[0].output.size() == 2);
    CHECK(IsValidUtf8ScalarSequence(history.Entries()[0].command));
    CHECK(IsValidUtf8ScalarSequence(history.Entries()[0].output));
}
