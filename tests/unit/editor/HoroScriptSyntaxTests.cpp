#include "../../support/SourceDocumentTestProject.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "HoroScriptSyntax.h"

#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Editor::Language;
    using namespace std::chrono_literals;

    TEST_CASE("Horo Script syntax analysis never executes author code") {
        TestSupport::SourceProject project;
        project.Put("error('execution would fail')\nreturn 42", "check.horo_script");
        SourceFileOpenService sources{project.directory};
        const auto opened = sources.Open({.path = "check.horo_script"});
        REQUIRE(opened.HasValue());
        REQUIRE(opened.Value().sourceSnapshot);
        const auto result = CheckHoroScriptSyntax(*opened.Value().sourceSnapshot);
        REQUIRE(result.HasValue());
        CHECK(result.Value().context.source.SharesStateWith(*opened.Value().sourceSnapshot));
        CHECK(std::get<LanguageDiagnostics>(result.Value().payload).items.empty());
    }

    TEST_CASE("Horo Script diagnostics preserve the BOM and the original byte range of a mixed-ending line") {
        TestSupport::SourceProject project;
        constexpr std::string_view invalid = "\xEF\xBB\xBF-- \xC3\xA9\n\rlocal = 12\r\n";
        project.Put(invalid, "check.lua");
        SourceFileOpenService sources{project.directory};
        const auto opened = sources.Open({.path = "check.lua"});
        REQUIRE(opened.HasValue());
        REQUIRE(opened.Value().sourceSnapshot);
        const auto result = CheckHoroScriptSyntax(*opened.Value().sourceSnapshot);
        REQUIRE(result.HasValue());
        const auto &items = std::get<LanguageDiagnostics>(result.Value().payload).items;
        REQUIRE(items.size() == 1);
        CHECK(items.front().span == LanguageByteSpan{invalid.find("local"), invalid.find("\r\n")});
        CHECK(items.front().severity == LanguageSeverity::Error);
        CHECK(items.front().message.find("horo-source") == std::string::npos);
    }

    TEST_CASE("Horo Script syntax workspace retires the replaced revision before publishing diagnostics") {
        TestSupport::SourceProject project;
        project.Put("local =", "check.lua");
        SourceFileOpenService sources{project.directory};
        const auto opened = sources.Open({.path = "check.lua"});
        REQUIRE(opened.HasValue());
        REQUIRE(opened.Value().sourceSnapshot);
        const auto original = *opened.Value().sourceSnapshot;
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
        HoroScriptSyntaxWorkspace workspace;
        CHECK_FALSE(workspace.Poll(jobs, sources, original));
        const auto current =
            sources.Documents().Edit(original.Identity().instance, {original.Revision(), 0, original.Text().size(), "return 1"});
        REQUIRE(current.HasValue());
        std::optional<LanguageResult> output;
        for (auto deadline = std::chrono::steady_clock::now() + 3s; std::chrono::steady_clock::now() < deadline && !output;) {
            output = workspace.Poll(jobs, sources, current.Value());
            std::this_thread::sleep_for(2ms);
        }
        REQUIRE(output);
        CHECK(output->context.source.SharesStateWith(current.Value()));
        CHECK(std::get<LanguageDiagnostics>(output->payload).items.empty());
    }
}  // namespace
