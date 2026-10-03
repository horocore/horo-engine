#include "Horo/Release/ReleaseErrors.h"
#include "Horo/Release/ReleaseProcess.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <optional>
#include <stdexcept>

namespace {
    using namespace Horo;
    using namespace Horo::Release;

    class RecordingProcessRunner final : public IExternalProcessRunner {
    public:
        ExternalProcessRequest seen;
        ExternalProcessResult completion;
        int calls{};

        [[nodiscard]] Result<ExternalProcessResult> Run(const ExternalProcessRequest &request, const CancellationToken &) override {
            ++calls;
            seen = request;
            if (request.onOutput)
                request.onOutput({ProcessOutputStream::StandardError, "diagnostic", false});
            return Result<ExternalProcessResult>::Success(completion);
        }
    };

    [[nodiscard]] ReleaseStageContext Context(const std::chrono::steady_clock::duration remaining = std::chrono::seconds{5}) {
        return {{1}, {2}, 3, ReleaseStage::Building, {4}, {}, std::chrono::steady_clock::now() + remaining};
    }
}  // namespace

TEST_CASE("Release child process receives stage identity and bounded native limits", "[unit][application][release][process]") {
    RecordingProcessRunner native;
    ReleaseProcessRequest request;
    request.executable = "compiler";
    request.timeout = std::chrono::hours{1};
    std::optional<ReleaseProcessOutput> observed;
    const auto result = ReleaseProcessRunner{native}.Run(Context(), request, [&](ReleaseProcessOutput line) {
        observed = std::move(line);
    });

    REQUIRE(result.HasValue());
    CHECK(native.calls == 1);
    CHECK(native.seen.timeout > std::chrono::milliseconds::zero());
    CHECK(native.seen.timeout <= std::chrono::seconds{5});
    CHECK(native.seen.gracefulTermination == std::chrono::seconds{2});
    CHECK(native.seen.maximumDrainDuration == std::chrono::seconds{1});
    CHECK(native.seen.maximumLineBytes == 16U * 1024U);
    CHECK(native.seen.maximumOutputBytes == 1024U * 1024U);
    REQUIRE(observed.has_value());
    CHECK(observed->job == ReleaseJobId{1});
    CHECK(observed->target == ReleaseTargetId{2});
    CHECK(observed->operation == 3);
    CHECK(observed->stage == ReleaseStage::Building);
    CHECK(observed->attempt == ReleaseStageAttemptId{4});
    CHECK(observed->line.text == "diagnostic");
}

TEST_CASE("Release child process rejects expired deadline and failed terminal results", "[unit][application][release][process]") {
    RecordingProcessRunner native;
    ReleaseProcessRequest request;
    request.executable = "compiler";
    CHECK(ReleaseProcessRunner{native}.Run(Context(-std::chrono::seconds{1}), request).HasError());
    CHECK(native.calls == 0);

    native.completion = {.reason = ProcessTerminationReason::TimedOut, .stopCause = ProcessStopCause::Timeout};
    CHECK(ReleaseProcessRunner{native}.Run(Context(), request).HasError());
    native.completion = {.reason = ProcessTerminationReason::Exited, .exitCode = 7};
    CHECK(ReleaseProcessRunner{native}.Run(Context(), request).HasError());
    native.completion = {.reason = ProcessTerminationReason::Exited, .exitCode = 0};
    CHECK(ReleaseProcessRunner{native}.Run(Context(), request).HasValue());
    CHECK(native.calls == 3);
}

TEST_CASE("Release child process drains after an observer throws", "[unit][application][release][process]") {
    RecordingProcessRunner native;
    ReleaseProcessRequest request;
    request.executable = "compiler";
    const auto result = ReleaseProcessRunner{native}.Run(Context(), request, [](ReleaseProcessOutput) {
        throw std::runtime_error{"observer failed"};
    });
    CHECK(result.HasError());
    CHECK(result.ErrorValue().code.Value() == ReleaseErrors::PipelineStageException.code.Value());
    CHECK(native.calls == 1);
}
