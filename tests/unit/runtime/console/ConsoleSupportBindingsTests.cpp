#include "Horo/Debug/ConsoleProduct.h"
#include "Horo/Debug/ConsoleSupportBindings.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <mutex>

using namespace Horo;
using namespace Horo::Debug;

namespace {
    class ApprovedRequests final : public IConsoleSupportBundleRequests {
    public:
        std::atomic<unsigned> calls{};

        void Approve(Diagnostics::DiagnosticBundleRequest request) {
            std::lock_guard lock(mutex_);
            request_ = std::move(request);
        }

        Result<Diagnostics::DiagnosticBundleRequest> TakeApproved(const DebugCommandContext &principal,
                                                                  const CancellationToken &cancellation) override {
            ++calls;
            std::lock_guard lock(mutex_);
            if (!principal.authenticated || cancellation.IsCancellationRequested() || !request_)
                return Result<Diagnostics::DiagnosticBundleRequest>::Failure(MakeCommandError(CommandError::PermissionDenied));
            auto request = std::move(*request_);
            request_.reset();
            return Result<Diagnostics::DiagnosticBundleRequest>::Success(std::move(request));
        }

    private:
        std::mutex mutex_;
        std::optional<Diagnostics::DiagnosticBundleRequest> request_;
    };

    class AdmissionAudit final : public ICommandAudit {
    public:
        bool reject{};

        Result<void> Record(DebugCommandId, std::string_view) override {
            if (reject)
                return Result<void>::Failure(MakeCommandError(CommandError::MissingCapability));
            return Result<void>::Success();
        }
    };

    struct SupportDirectory final {
        std::filesystem::path path;

        SupportDirectory() {
            path = std::filesystem::temp_directory_path() / std::filesystem::path(u8"horo console destek \u00F6 ");
            path += std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            REQUIRE(std::filesystem::create_directory(path));
        }

        ~SupportDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

    void Await(JobSystem &jobs, JobId id) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!jobs.Query(id).terminalResult && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        REQUIRE(jobs.Query(id).terminalResult);
    }
}  // namespace

TEST_CASE("Console support export consumes explicit consent only after authentication audit and I/O admission",
          "[console][support][profile]") {
    const auto catalog = ProductConsoleDescriptors();
    const auto descriptor = std::ranges::find(catalog, SupportBundleCommandId, &DebugCommandDescriptor::id);
    RegistryPolicy policy;
    policy.profile = CompiledConsoleProfile();
    const auto inventory =
        descriptor == catalog.end() ? std::span<const DebugCommandDescriptor>{} : std::span<const DebugCommandDescriptor>{&*descriptor, 1};
    auto composed = CommandRegistry::Create(inventory, policy);
    REQUIRE(composed.HasValue());
    auto registry = std::make_shared<CommandRegistry>(std::move(composed).Value());
    auto approvals = std::make_shared<ApprovedRequests>();
    auto bound = BindConsoleSupportBundleCommands(registry, approvals);
    REQUIRE(bound.HasValue());
    if (descriptor == catalog.end()) {
        CHECK(bound.Value().empty());
        CHECK(registry->Prepare("diag.support_bundle", {15, true, true}).HasError());
        return;
    }
    REQUIRE(bound.Value().size() == 1);
    CHECK(bound.Value()[0].resource == JobResource::Io);
    CHECK(approvals->calls == 0);
    CHECK(BindConsoleSupportBundleCommands(registry, {}).HasError());
    SupportDirectory directory;
    const auto source = directory.path / "engine.jsonl";
    const auto output = directory.path / "diagnostics.zip";
    {
        std::ofstream stream(source);
        stream << R"({"password":"private-approval-token","message":"ok"})";
        REQUIRE(stream.good());
    }
    approvals->Approve({.outputPath = output,
                        .entries = {{.sourcePath = source, .archivePath = "logs/engine.jsonl", .redactSensitiveText = true}},
                        .metadata = {{"app", "console-test"}},
                        .maxInputBytes = 1024});
    JobSystem jobs(JobSystemConfig{.workerCount = 0, .ioWorkerCount = 1});
    auto operations = std::make_shared<OperationStore>(4, 4);
    auto audit = std::make_shared<AdmissionAudit>();
    auto created = CommandExecutor::Create(registry, bound.Value(), jobs, operations, std::this_thread::get_id(),
                                           std::this_thread::get_id(), 4, audit);
    REQUIRE(created.HasValue());
    auto &executor = *created.Value();
    CHECK(executor.Submit("diag.support_bundle", {15, true, false}).HasError());
    CHECK(approvals->calls == 0);
    audit->reject = true;
    CHECK(executor.Submit("diag.support_bundle", {15, true, true}).HasError());
    CHECK(approvals->calls == 0);
    CHECK_FALSE(std::filesystem::exists(output));
    audit->reject = false;
    auto submitted = executor.Submit("diag.support_bundle", {15, true, true});
    REQUIRE(submitted.HasValue());
    REQUIRE(submitted.Value().job);
    REQUIRE(submitted.Value().operation);
    Await(jobs, *submitted.Value().job);
    CHECK(jobs.Query(*submitted.Value().job).state == JobState::Succeeded);
    CHECK(approvals->calls == 1);
    REQUIRE(std::filesystem::exists(output));
    std::ifstream stream(output, std::ios::binary);
    const std::string archive{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    CHECK(archive.starts_with("PK"));
    CHECK(archive.find("logs/engine.jsonl") != std::string::npos);
    CHECK(archive.find("private-approval-token") == std::string::npos);
    const auto snapshot = operations->SnapshotIfChanged(0);
    REQUIRE(snapshot);
    const auto record = std::ranges::find(snapshot->operations, *submitted.Value().operation, &OperationRecord::id);
    REQUIRE(record != snapshot->operations.end());
    CHECK(record->message == "[REDACTED]");
    CHECK(record->message.find(directory.path.string()) == std::string::npos);
    auto reused = executor.Submit("diag.support_bundle", {15, true, true});
    REQUIRE(reused.HasValue());
    REQUIRE(reused.Value().job);
    Await(jobs, *reused.Value().job);
    CHECK(jobs.Query(*reused.Value().job).state == JobState::Failed);
    CHECK(approvals->calls == 2);
}
