#include "../../support/SourceDocumentTestProject.h"
#include "ClangdProvider.h"
#include "Horo/Application/GameplayBuildService.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Platform/ExternalProcess.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Editor::Language;
    using namespace std::chrono_literals;

    /** @brief Creates a discovery candidate for existence and permission checks without running it. */
    std::filesystem::path StubClangd(const TestSupport::SourceProject &owner) {
#ifdef _WIN32
        constexpr auto name = "clangd.exe";
#else
        constexpr auto name = "clangd";
#endif
        owner.Put("fixture", name);
        auto file = owner.directory / name;
#ifndef _WIN32
        std::filesystem::permissions(file, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
#endif
        return file;
    }

    /** @brief Encodes fixture paths in the UTF-8 representation read by Clangd. */
    std::string Utf8(const std::filesystem::path &path) {
        const auto text = path.generic_u8string();
        return {reinterpret_cast<const char *>(text.data()), text.size()};
    }

    /** @brief Pumps real language workers for a single bounded integration step. */
    template <typename Step> bool Await(Step step) {
        const auto deadline = ProjectLanguageWorkspace::Clock::now() + 10s;
        while (ProjectLanguageWorkspace::Clock::now() < deadline) {
            if (step(ProjectLanguageWorkspace::Clock::now()))
                return true;
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }

    TEST_CASE("Clangd discovery skips relative and project-owned executables and consumes the gameplay database") {
        TestSupport::SourceProject project;
        TestSupport::SourceProject trustedTools;
        static_cast<void>(StubClangd(project));
        const auto installed = StubClangd(trustedTools);
        SourceFileOpenService sources{project.directory};
        auto compiled = Application::GameplayLocalBuildDirectory(sources.ProjectRoot());
        std::filesystem::create_directories(compiled);
        std::ofstream{compiled / "compile_commands.json"} << R"([{"directory":".","file":"test.cpp","arguments":["clang++","test.cpp"]}])";
        ClangdSettings settings;
        settings.searchDirectories = {".", project.directory, trustedTools.directory};
        const auto provider = PrepareClangdProvider(sources, settings);
        REQUIRE(provider.HasValue());
        CHECK(std::filesystem::equivalent(provider.Value().executable, installed));
        CHECK(provider.Value().extensions == sources.Policy().nativeSourceExtensions);
        constexpr std::string_view prefix = "--compile-commands-dir=";
        const std::string_view argument{provider.Value().arguments.front()};
        REQUIRE(argument.starts_with(prefix));
        CHECK(std::filesystem::equivalent(std::filesystem::u8path(argument.substr(prefix.size())), compiled));
    }

    TEST_CASE("Clangd explicit paths preserve the chosen database and report missing binary or metadata") {
        TestSupport::SourceProject project;
        TestSupport::SourceProject trustedTools;
        SourceFileOpenService sources{project.directory};
        ClangdSettings settings;
        const auto absent = PrepareClangdProvider(sources, settings);
        REQUIRE(absent.HasError());
        CHECK(absent.ErrorValue().code.Value() == "clangd_executable_unavailable");

        settings.executable = StubClangd(trustedTools);
        const auto withoutCommands = PrepareClangdProvider(sources, settings);
        REQUIRE(withoutCommands.HasError());
        CHECK(withoutCommands.ErrorValue().code.Value() == "clangd_compile_commands_unavailable");
        const auto build = project.directory / "custom build";
        std::filesystem::create_directories(build);
        project.Put(R"([{"directory":".","file":"test.cpp","arguments":["clang++","test.cpp"]}])", "custom build/compile_commands.json");
        settings.compileCommands = std::filesystem::u8path("custom build/compile_commands.json");
        settings.backgroundIndex = false;
        const auto selected = PrepareClangdProvider(sources, settings);
        REQUIRE(selected.HasValue());
        CHECK(selected.Value().arguments.front().find("custom build") != std::string::npos);
        CHECK(selected.Value().arguments.back() == "--background-index=false");
        settings.executable = "clangd";
        CHECK(PrepareClangdProvider(sources, settings).HasError());
    }

    TEST_CASE("Clangd discovery rejects a project database symlink outside the project") {
        TestSupport::SourceProject project;
        TestSupport::SourceProject trustedTools;
        trustedTools.Put("[]", "compile_commands.json");
        std::error_code error;
        std::filesystem::create_symlink(trustedTools.directory / "compile_commands.json", project.directory / "compile_commands.json",
                                        error);
        if (error)
            SKIP("This host cannot create a file symlink.");

        SourceFileOpenService sources{project.directory};
        ClangdSettings settings{.executable = StubClangd(trustedTools)};
        const auto discovered = PrepareClangdProvider(sources, settings);
        REQUIRE(discovered.HasError());
        CHECK(discovered.ErrorValue().code.Value() == "clangd_compile_commands_unavailable");
        settings.compileCommands = trustedTools.directory / "compile_commands.json";
        CHECK(PrepareClangdProvider(sources, settings).HasValue());
    }

    TEST_CASE("Installed Clangd publishes diagnostics, navigates symbols and completes project C++", "[integration][clangd]") {
        const auto *installed = std::getenv("HORO_TEST_CLANGD");
        if (!installed)
            SKIP("Set HORO_TEST_CLANGD to the installed host executable.");
        TestSupport::SourceProject project;
        std::filesystem::create_directory(project.directory / "include");
        project.Put("inline int horo_fixture_symbol() { return 37; }\n", "include/gameplay_api.hpp");
        constexpr std::string_view code = "#include \"gameplay_api.hpp\"\nint gameplay() { return horo_fixture_symbol(); }\n"
                                          "int repeat() { return horo_fixture_symbol(); }\nint broken = missing_fixture_symbol;\n"
                                          "int repair() { return horo_fixture_symbl(); }\n"
                                          "int later() { return horo_fixture_symb; }\n";
        project.Put(code, "gameplay.cpp");
        const auto sourcePath = Utf8(project.directory / "gameplay.cpp");
        const auto commands = nlohmann::json::array({{{"directory", Utf8(project.directory)},
                                                      {"file", sourcePath},
                                                      {"arguments", {"clang++", "-std=c++20", "-Iinclude", sourcePath}}}});
        project.Put(commands.dump(), "compile_commands.json");
        SourceFileOpenService sources{project.directory};
        const auto opened = sources.Open({.path = "gameplay.cpp"});
        REQUIRE(opened.HasValue());
        REQUIRE(opened.Value().sourceSnapshot.has_value());
        const std::array documents{*opened.Value().sourceSnapshot};
        const auto provider = PrepareClangdProvider(sources, {.executable = std::filesystem::u8path(installed), .backgroundIndex = false});
        REQUIRE(provider.HasValue());
        NativeExternalProcessRunner runner;
        auto workspace = ProjectLanguageWorkspace::Create(runner, sources, std::span{&provider.Value(), 1});
        REQUIRE(workspace.HasValue());
        auto &owner = *workspace.Value();
        bool diagnosed = false;
        bool navigated = false;
        bool referencesFound = false;
        bool described = false;
        bool foundDocumentSymbol = false;
        bool repairDiagnosed = false;
        bool repairOffered = false;
        const auto repairOffset = code.find("horo_fixture_symbl");
        std::optional<SourceEditingPlan> completionPlan;
        const auto firstReference = code.find("horo_fixture_symbol");
        const auto secondReference = code.find("horo_fixture_symbol", firstReference + 1);
        const auto completionOffset = code.rfind("horo_fixture_symb") + std::string_view{"horo_fixture_symb"}.size();
        const auto observe = [&](const auto now) {
            for (const auto &event : owner.Poll(documents, now)) {
                if (const auto *diagnostics = std::get_if<LanguageDiagnostics>(&event.payload)) {
                    diagnosed |= event.context.source.SharesStateWith(documents.front()) &&
                                 std::ranges::any_of(diagnostics->items, [&](const auto &item) {
                        return item.severity == LanguageSeverity::Error && item.span.start == code.find("missing_fixture_symbol");
                    });
                    repairDiagnosed |= std::ranges::any_of(diagnostics->items, [&](const auto &item) {
                        return item.span.start == repairOffset;
                    });
                }
                if (const auto *actions = std::get_if<LanguageCodeActions>(&event.payload)) {
                    repairOffered |= std::ranges::any_of(actions->items, [&](const LanguageCodeAction &action) {
                        auto plan = PlanLanguageCodeAction(documents.front(), {.anchor = repairOffset, .caret = repairOffset}, action);
                        return plan.HasValue() && plan.Value().offset == repairOffset && plan.Value().insert == "horo_fixture_symbol";
                    });
                }
                if (const auto *navigation = std::get_if<LanguageNavigation>(&event.payload);
                    navigation && event.context.kind == SourceLanguageRequestKind::Definition) {
                    navigated |= std::ranges::any_of(navigation->targets, [](const auto &target) {
                        return target.source.Value() == "include/gameplay_api.hpp";
                    });
                }
                if (const auto *navigation = std::get_if<LanguageNavigation>(&event.payload);
                    navigation && event.context.kind == SourceLanguageRequestKind::References) {
                    const auto containsOffset = [&](const std::size_t offset) {
                        return std::ranges::any_of(navigation->targets, [&](const LanguageLocation &target) {
                            const auto span = ResolveLanguageLocation(documents.front(), target);
                            return span && span->start == offset;
                        });
                    };
                    referencesFound = containsOffset(firstReference) && containsOffset(secondReference);
                }
                if (const auto *hover = std::get_if<LanguageHover>(&event.payload))
                    described |= hover->text.find("horo_fixture_symbol") != std::string::npos;
                if (const auto *symbols = std::get_if<LanguageSymbols>(&event.payload)) {
                    const auto declaration = code.find("repeat()");
                    foundDocumentSymbol |= event.context.source.SharesStateWith(documents.front()) &&
                                           std::ranges::any_of(symbols->items, [&](const LanguageSymbol &symbol) {
                        return symbol.name == "repeat" && symbol.selection.start <= declaration &&
                               symbol.selection.end >= declaration + std::string_view{"repeat"}.size();
                    });
                }
                if (const auto *completion = std::get_if<LanguageCompletions>(&event.payload)) {
                    const auto item = std::ranges::find_if(completion->items, [](const auto &candidate) {
                        return candidate.label.find("horo_fixture_symbol") != std::string::npos;
                    });
                    if (item != completion->items.end()) {
                        auto edit =
                            PlanLanguageCompletion(documents.front(), {.anchor = completionOffset, .caret = completionOffset}, *item);
                        if (edit.HasValue())
                            completionPlan = std::move(edit).Value();
                    }
                }
            }
        };
        REQUIRE(Await([&](const auto now) {
            observe(now);
            return owner.Request(documents.front(), SourceLanguageRequestKind::Definition, {.offset = code.find("horo_fixture_symbol")})
                .HasValue();
        }));
        REQUIRE(Await([&](const auto now) {
            observe(now);
            return diagnosed && navigated;
        }));
        REQUIRE(owner.Request(documents.front(), SourceLanguageRequestKind::References, {.offset = firstReference}).HasValue());
        REQUIRE(Await([&](const auto now) {
            observe(now);
            return referencesFound;
        }));
        REQUIRE(owner.Request(documents.front(), SourceLanguageRequestKind::Hover, {.offset = firstReference}).HasValue());
        REQUIRE(Await([&](const auto now) {
            observe(now);
            return described;
        }));
        REQUIRE(owner.Request(documents.front(), SourceLanguageRequestKind::Symbols).HasValue());
        REQUIRE(Await([&](const auto now) {
            observe(now);
            return foundDocumentSymbol;
        }));
        REQUIRE(Await([&](const auto now) {
            observe(now);
            return repairDiagnosed;
        }));
        REQUIRE(owner.Request(documents.front(), SourceLanguageRequestKind::CodeActions, {.offset = repairOffset}).HasValue());
        REQUIRE(Await([&](const auto now) {
            observe(now);
            return repairOffered;
        }));
        REQUIRE(owner.Request(documents.front(), SourceLanguageRequestKind::Completion, {.offset = completionOffset}).HasValue());
        REQUIRE(Await([&](const auto now) {
            observe(now);
            return completionPlan.has_value();
        }));
        const auto completed = SubmitSourceEditingCommand(sources.Documents(), SourceEditSubmission{documents.front().Identity().instance,
                                                                                                    std::move(*completionPlan)});
        REQUIRE(completed.HasValue());
        CHECK(completed.Value().snapshot.Text().find("return horo_fixture_symbol", code.find("int later()")) != std::string_view::npos);
        owner.Shutdown(ProjectLanguageWorkspace::Clock::now());
        REQUIRE(Await([&](const auto now) {
            static_cast<void>(owner.Poll({}, now));
            return owner.Finished();
        }));
    }
}  // namespace
