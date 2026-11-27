#include "../../support/SourceDocumentTestProject.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Platform/PlatformErrors.h"
#include "LspProcess.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Editor::Language;
    using namespace std::chrono_literals;

    /** @brief Bounded polling for an asynchronous native process condition. */
    template <typename Step> bool Await(Step step) {
        const auto deadline = LspProcess::Clock::now() + 5s;
        while (LspProcess::Clock::now() < deadline) {
            if (step())
                return true;
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }

    /** @brief Supplies the real fixture path and short process-lifetime bounds. */
    ExternalProcessRequest FixtureRequest() {
        ExternalProcessRequest request;
        request.executable = HORO_LANGUAGE_SERVER_CHILD;
        request.timeout = 8s;
        request.gracefulTermination = 50ms;
        return request;
    }

    /** @brief Waits for one incoming protocol item while checking transport health. */
    std::optional<LspIncoming> AwaitIncoming(LspProcess &process) {
        std::optional<LspIncoming> output;
        REQUIRE(Await([&] {
            auto result = process.Poll(LspProcess::Clock::now());
            REQUIRE(result.HasValue());
            output = std::move(result).Value();
            return output.has_value();
        }));
        return output;
    }

    TEST_CASE("Native LSP worker initializes requests and shuts down over a real child process") {
        TestSupport::SourceProject project;
        project.Put("int answer;", "source.cpp");
        SourceFileOpenService open{project.directory};
        auto document = open.Open({.path = "source.cpp"});
        REQUIRE(document.HasValue());
        REQUIRE(document.Value().sourceSnapshot.has_value());
        const auto &snapshot = *document.Value().sourceSnapshot;

        NativeExternalProcessRunner runner;
        SourceLanguageSession source;
        LspProcess process{runner, source};
        REQUIRE(process.Start(FixtureRequest(), "file:///project", std::nullopt, LspProcess::Clock::now()).HasValue());
        CHECK(source.State() == SourceLanguageSessionState::Initializing);
        REQUIRE(Await([&] {
            REQUIRE(process.Poll(LspProcess::Clock::now()).HasValue());
            return source.State() == SourceLanguageSessionState::Ready;
        }));
        REQUIRE(process.Connection().SyncSource(snapshot, "cpp").HasValue());
        const auto diagnostics = AwaitIncoming(process);
        REQUIRE(diagnostics.has_value());
        const auto publication = process.Connection().ProjectReply(*diagnostics);
        REQUIRE(publication.HasValue());
        CHECK(std::get<LanguageDiagnostics>(publication.Value().payload).items.empty());
        REQUIRE(process.Connection().Request(snapshot, SourceLanguageRequestKind::Hover).HasValue());
        const auto reply = AwaitIncoming(process);
        REQUIRE(reply.has_value());
        REQUIRE(reply->reply.has_value());
        CHECK(reply->reply->source.SharesStateWith(snapshot));
        const auto projected = process.Connection().ProjectReply(*reply);
        REQUIRE(projected.HasValue());
        REQUIRE(std::holds_alternative<LanguageHover>(projected.Value().payload));
        CHECK(std::get<LanguageHover>(projected.Value().payload).text == "hover fixture");

        REQUIRE(process.Connection().Shutdown(LspProcess::Clock::now()).HasValue());
        CHECK(source.State() == SourceLanguageSessionState::Disconnected);
        CHECK(process.Connection().ProjectReply(*reply).HasError());
        REQUIRE(Await([&] {
            REQUIRE(process.Poll(LspProcess::Clock::now()).HasValue());
            return process.Finished();
        }));
        CHECK(process.TakeStandardError() == "language fixture ready\n");
        CHECK(process.TakeStandardError().empty());
    }

    TEST_CASE("Native LSP worker reports missing binaries and a new project connection can start afterwards") {
        NativeExternalProcessRunner runner;
        SourceLanguageSession source;
        LspProcess missing{runner, source};
        TestSupport::SourceProject project;
        auto request = FixtureRequest();
        request.executable = (project.directory / "absent-language-server").string();
        REQUIRE(missing.Start(std::move(request), "file:///project", std::nullopt, LspProcess::Clock::now()).HasValue());
        std::optional<Error> error;
        REQUIRE(Await([&] {
            auto result = missing.Poll(LspProcess::Clock::now());
            if (result.HasError())
                error = std::move(result).ErrorValue();
            return missing.Finished();
        }));
        REQUIRE(error.has_value());
        CHECK(error->code.Value() == PlatformErrors::ProcessLaunchFailed.code.Value());
        CHECK(source.State() == SourceLanguageSessionState::Disconnected);

        LspProcess next{runner, source};
        REQUIRE(next.Start(FixtureRequest(), "file:///other", std::nullopt, LspProcess::Clock::now()).HasValue());
        REQUIRE(next.Connection().Shutdown(LspProcess::Clock::now()).HasValue());
        REQUIRE(Await([&] {
            REQUIRE(next.Poll(LspProcess::Clock::now()).HasValue());
            return next.Finished();
        }));
        CHECK(source.State() == SourceLanguageSessionState::Disconnected);
    }
}  // namespace
