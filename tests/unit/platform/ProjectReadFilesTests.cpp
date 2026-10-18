#include "ProjectCodeFilesTesting.h"
#include "ProjectCodeQueryTestSupport.h"

#include <algorithm>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <utility>
#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace Horo;
using namespace Horo::Platform;
namespace QueryTest = Horo::Test::CodeQuery;

namespace {
    /** @brief Observes the second native transfer while retaining the fixture's distinct action.
     * @details The capability owns the action; its borrowed fixture/count state outlives that capability.
     */
    std::shared_ptr<const IProjectReadFiles> FilesWithSecondReadObserver(const QueryTest::Directory &directory, unsigned &chunks,
                                                                         std::function<void()> action) {
        auto files =
            CodeFilesTesting::Create(directory.root, "project-one", 3,
                                     [&chunks, action = std::move(action)](const CodeFilesTesting::Boundary boundary, std::string_view) {
            if (boundary == CodeFilesTesting::Boundary::BeforeRead && ++chunks == 2)
                action();
        });
        REQUIRE(files.HasValue());
        return std::move(files).Value();
    }
}  // namespace

TEST_CASE("Portable project paths reject complete device names and retain ordinary similar spellings", "[unit][platform][code-query]") {
    for (const auto name : {"con", "PrN.txt", "AUX", "nul.cpp", "CLOCK$", "conin$", "ConOut$.txt", "COM1", "lpt9.txt", "CoM\xc2\xb9.cpp",
                            "LPT\xc2\xb2", "com\xc2\xb3.log", "nested/LpT1.cpp"})
        CHECK_FALSE(IsSafeProjectReadPath(name));
    for (const auto name : {"COM", "LPT", "com0", "lpt10.cpp", "com\xc2\xb9x.cpp", "company.cpp", "nested/lpt9x.cpp", "ışık.cpp"})
        CHECK(IsSafeProjectReadPath(name));
}

TEST_CASE("Native recursive enumeration shares exact entry ceilings across nested directories", "[unit][platform][code-query]") {
    QueryTest::Directory directory;
    directory.Write("nested/a.cpp", "a");
    directory.Write("nested/deep/b.cpp", "b");
    directory.Write("nested/deep/c.cpp", "c");
    const auto files = directory.Files();
    ProjectReadLimits limits;
    limits.maximumEntries = 5;
    const auto complete = files->Files({}, QueryTest::Context(), limits);
    REQUIRE(complete.HasValue());
    REQUIRE(complete.Value().entries.size() == 3);
    CHECK(complete.Value().entries.front().path == "nested/a.cpp");
    CHECK(complete.Value().entries.back().path == "nested/deep/c.cpp");
    limits.maximumEntries = 4;
    const auto exhausted = files->Files({}, QueryTest::Context(), limits);
    REQUIRE(exhausted.HasError());
    CHECK(QueryTest::Matches(exhausted.ErrorValue(), ProjectReadErrors::Capacity));
}

TEST_CASE("Native project reads retain contained bytes and independent enumeration cursors", "[unit][platform][code-query]") {
    QueryTest::Directory directory;
    directory.Write("nested/a file.cpp", "abc");
    directory.Write(std::filesystem::path{u8"nested/ışık.cpp"}, "utf8");
    const auto files = directory.Files();
    const auto text = files->Text("nested/a file.cpp", QueryTest::Context(), {});
    REQUIRE(text.HasValue());
    CHECK(*text.Value().text == "abc");
    const auto capture = [&] {
        return files->Files("nested", QueryTest::Context(), {});
    };
    auto worker = std::async(std::launch::async, capture);
    const auto first = capture();
    const auto second = worker.get();
    REQUIRE(first.HasValue());
    REQUIRE(second.HasValue());
    CHECK(first.Value().entries.size() == 2);
    CHECK(second.Value().revision == first.Value().revision);
    directory.Write("nested/a file.cpp", "changed");
    CHECK(*text.Value().text == "abc");
    CHECK(files->Text("nested/a file.cpp", QueryTest::Context(), {}).Value().revision != text.Value().revision);
}

TEST_CASE("Native recursive enumeration cancellation retires cursors and leaves the root reusable", "[unit][platform][code-query]") {
    QueryTest::Directory directory;
    directory.Write("nested/a.cpp", "a");
    directory.Write("nested/deep/b.cpp", "b");
    bool observed{};
    bool revoked{};
    auto files = CodeFilesTesting::Create(directory.root, "project-one", 3,
                                          [&](const CodeFilesTesting::Boundary boundary, const std::string_view path) {
        if (!observed && boundary == CodeFilesTesting::Boundary::BeforeDirectoryEntryOpen && path.starts_with("nested/")) {
            observed = true;
            revoked = true;
        }
    });
    REQUIRE(files.HasValue());
    auto context = QueryTest::Context();
    context.authorityStopped = [&] {
        return revoked;
    };
    const auto cancelled = files.Value()->Files({}, context, {});
    REQUIRE(observed);
    REQUIRE(cancelled.HasError());
    CHECK(QueryTest::Matches(cancelled.ErrorValue(), ProjectReadErrors::Cancelled));
    revoked = false;
    const auto first = files.Value()->Files({}, context, {});
    const auto second = files.Value()->Files({}, context, {});
    REQUIRE(first.HasValue());
    REQUIRE(second.HasValue());
    CHECK(first.Value().entries.size() == 2);
    CHECK(second.Value().revision == first.Value().revision);
    CHECK(*files.Value()->Text("nested/a.cpp", context, {}).Value().text == "a");
}

TEST_CASE("Native read budgets stop oversized bytes entries and expired authority", "[unit][platform][code-query]") {
    QueryTest::Directory directory;
    directory.Write("a.cpp", "12345");
    directory.Write("b.cpp", "67890");
    const auto files = directory.Files();
    ProjectReadLimits limits;
    limits.maximumFileBytes = 4;
    CHECK(QueryTest::Matches(files->Text("a.cpp", QueryTest::Context(), limits).ErrorValue(), ProjectReadErrors::Capacity));
    limits = {};
    limits.maximumEntries = 1;
    CHECK(QueryTest::Matches(files->Files({}, QueryTest::Context(), limits).ErrorValue(), ProjectReadErrors::Capacity));
    limits = {};
    limits.maximumManifestBytes = 2;
    CHECK(QueryTest::Matches(files->Files({}, QueryTest::Context(), limits).ErrorValue(), ProjectReadErrors::Capacity));
    auto context = QueryTest::Context();
    context.projectIdentity = "foreign";
    CHECK(QueryTest::Matches(files->Text("a.cpp", context, {}).ErrorValue(), ProjectReadErrors::Stale));
    context = QueryTest::Context();
    context.authorityStopped = [] {
        return true;
    };
    CHECK(QueryTest::Matches(files->Text("a.cpp", context, {}).ErrorValue(), ProjectReadErrors::Cancelled));
    CHECK(files->Text("missing.cpp", QueryTest::Context(), {}).HasError());
    CHECK(CreateNativeProjectReadFiles(directory.root / std::string(4097, 'a'), "project-one", 3).HasError());
}

TEST_CASE("Native reads reject hard links and symlink descendants before reading", "[unit][platform][code-query]") {
    QueryTest::Directory directory;
    QueryTest::Directory external;
    external.Write("secret.cpp", "never disclose");
    directory.Write("a.cpp", "owned");
    const auto files = directory.Files();
    std::error_code error;
    std::filesystem::create_hard_link(directory.root / "a.cpp", directory.root / "hard.cpp", error);
    if (error)
        SKIP("The test filesystem cannot create hard links.");
    CHECK(files->Text("hard.cpp", QueryTest::Context(), {}).HasError());
    CHECK(files->SourceText("hard.cpp", QueryTest::Context(), {}).HasError());
    std::filesystem::remove(directory.root / "hard.cpp");
    std::filesystem::create_directory_symlink(external.root, directory.root / "escape", error);
    if (error)
        SKIP("The host does not grant symlink creation for this test.");
    CHECK(files->Text("escape/secret.cpp", QueryTest::Context(), {}).HasError());
    CHECK(files->SourceText("escape/secret.cpp", QueryTest::Context(), {}).HasError());
    CHECK(files->Files({}, QueryTest::Context(), {}).HasError());
    CHECK(CreateNativeProjectReadFiles(directory.root / "escape", "project-one", 3).HasError());
}

TEST_CASE("Native source capture admits its full byte ceiling without widening interactive queries", "[unit][platform][source-read]") {
    QueryTest::Directory directory;
    const ProjectSourceReadLimits hard;
    const std::string bytes(hard.maximumFileBytes, 's');
    directory.Write("large.scene", bytes);
    const auto files = directory.Files();
    auto context = QueryTest::Context();
    context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    const auto source = files->SourceText("large.scene", context, {});
    REQUIRE(source.HasValue());
    CHECK(*source.Value().text == bytes);
    CHECK(source.Value().projectIdentity == context.projectIdentity);
    CHECK(source.Value().projectGeneration == context.projectGeneration);
    CHECK(QueryTest::Matches(files->Text("large.scene", context, {}).ErrorValue(), ProjectReadErrors::Capacity));
    CHECK(QueryTest::Matches(files->Text("large.scene", context, {.maximumFileBytes = bytes.size()}).ErrorValue(),
                             ProjectReadErrors::Invalid));
    directory.Write("large.scene", std::string(hard.maximumFileBytes + 1, 's'));
    CHECK(QueryTest::Matches(files->SourceText("large.scene", context, {}).ErrorValue(), ProjectReadErrors::Capacity));
    CHECK(*source.Value().text == bytes);
}

TEST_CASE("Native source capture keeps caller stop fences and independent hard policy", "[unit][platform][source-read]") {
    QueryTest::Directory directory;
    directory.Write("source.scene", "bounded source");
    const auto files = directory.Files();
    ProjectSourceReadLimits limits;
    ++limits.maximumFileBytes;
    CHECK(QueryTest::Matches(files->SourceText("source.scene", QueryTest::Context(), limits).ErrorValue(), ProjectReadErrors::Invalid));
    limits = {};
    limits.maximumDuration += std::chrono::milliseconds{1};
    CHECK(QueryTest::Matches(files->SourceText("source.scene", QueryTest::Context(), limits).ErrorValue(), ProjectReadErrors::Invalid));
    auto context = QueryTest::Context();
    context.deadline = std::chrono::steady_clock::now();
    CHECK(QueryTest::Matches(files->SourceText("source.scene", context, {}).ErrorValue(), ProjectReadErrors::Deadline));
    context = QueryTest::Context();
    context.projectGeneration = 4;
    CHECK(QueryTest::Matches(files->SourceText("source.scene", context, {}).ErrorValue(), ProjectReadErrors::Stale));
    directory.Write("source.scene", std::string(20000, 's'));
    unsigned chunks{};
    bool revoked{};
    const auto observed = FilesWithSecondReadObserver(directory, chunks, [&revoked] {
        revoked = true;
    });
    context = QueryTest::Context();
    context.authorityStopped = [&revoked] {
        return revoked;
    };
    CHECK(QueryTest::Matches(observed->SourceText("source.scene", context, {}).ErrorValue(), ProjectReadErrors::Cancelled));
    CHECK(chunks == 2);
    CHECK(revoked);
    CHECK(files->Text("source.scene", QueryTest::Context(), {}).HasValue());
}

TEST_CASE("Component substitution cannot redirect a retained directory traversal", "[unit][platform][code-query]") {
    QueryTest::Directory directory;
    QueryTest::Directory external;
    directory.Write("nested/a.cpp", "owned");
    external.Write("a.cpp", "secret");
    bool observed{};
    std::error_code error;
    auto files = CodeFilesTesting::Create(directory.root, "project-one", 3,
                                          [&](const CodeFilesTesting::Boundary boundary, const std::string_view path) {
        if (!observed && boundary == CodeFilesTesting::Boundary::BeforeComponentOpen && path == "nested") {
            observed = true;
            std::filesystem::rename(directory.root / "nested", directory.root / "original");
            std::filesystem::create_directory_symlink(external.root, directory.root / "nested", error);
        }
    });
    REQUIRE(files.HasValue());
    const auto result = files.Value()->Text("nested/a.cpp", QueryTest::Context(), {});
    if (error)
        SKIP("The host does not grant symlink creation for the deterministic race test.");
    CHECK(observed);
    CHECK(result.HasError());
}

TEST_CASE("Enumeration substitutions are checked at the native child-open boundary", "[unit][platform][code-query]") {
    QueryTest::Directory directory;
    QueryTest::Directory external;
    directory.Write("a.cpp", "owned");
    external.Write("secret.cpp", "secret");
    bool observed{};
    std::error_code error;
    auto files = CodeFilesTesting::Create(directory.root, "project-one", 3,
                                          [&](const CodeFilesTesting::Boundary boundary, const std::string_view path) {
        if (!observed && boundary == CodeFilesTesting::Boundary::BeforeDirectoryEntryOpen && path == "a.cpp") {
            observed = true;
            std::filesystem::remove(directory.root / "a.cpp");
            std::filesystem::create_symlink(external.root / "secret.cpp", directory.root / "a.cpp", error);
        }
    });
    REQUIRE(files.HasValue());
    const auto result = files.Value()->Files({}, QueryTest::Context(), {});
    if (error)
        SKIP("The host does not grant symlink creation for the enumeration race test.");
    CHECK(observed);
    CHECK(result.HasError());
}

TEST_CASE("Root renaming cannot replace admitted native root identity", "[unit][platform][code-query]") {
    QueryTest::Directory directory;
    directory.Write("a.cpp", "owned");
    const auto files = directory.Files();
    const auto original = directory.root;
    const auto moved = original.parent_path() / (original.filename().string() + " moved");
    std::filesystem::rename(original, moved);
    directory.root = moved;
    std::filesystem::create_directory(original);
    {
        std::ofstream replacement{original / "a.cpp"};
        replacement << "replacement";
    }
    const auto result = files->Text("a.cpp", QueryTest::Context(), {});
    std::error_code error;
    std::filesystem::remove_all(original, error);
    REQUIRE(result.HasValue());
    CHECK(*result.Value().text == "owned");
}

TEST_CASE("Native chunk reads observe revocation without publishing a partial result", "[unit][platform][code-query]") {
    QueryTest::Directory directory;
    directory.Write("a.cpp", std::string(16384, 'a'));
    unsigned chunks{};
    bool revoked{};
    auto files =
        CodeFilesTesting::Create(directory.root, "project-one", 3, [&](const CodeFilesTesting::Boundary boundary, std::string_view) {
        if (boundary == CodeFilesTesting::Boundary::BeforeRead && ++chunks == 2)
            revoked = true;
    });
    REQUIRE(files.HasValue());
    auto context = QueryTest::Context();
    context.authorityStopped = [&] {
        return revoked;
    };
    const auto result = files.Value()->Text("a.cpp", context, {});
    REQUIRE(result.HasError());
    CHECK(QueryTest::Matches(result.ErrorValue(), ProjectReadErrors::Cancelled));
    CHECK(chunks == 2);
}

TEST_CASE("Native transfers preserve empty short exact and sequential chunk bytes", "[unit][platform][code-query]") {
    QueryTest::Directory directory;
    const auto files = directory.Files();
    for (const std::size_t length : {0U, 1U, 4095U, 4096U, 4097U, 8193U}) {
        DYNAMIC_SECTION("length " << length) {
            std::string expected(length, '\0');
            for (std::size_t index = 0; index < length; ++index)
                expected[index] = static_cast<char>(index % 251U);
            directory.Write("a.cpp", expected);
            ProjectReadLimits limits;
            limits.maximumFileBytes = std::max<std::size_t>(1, length);
            const auto captured = files->Text("a.cpp", QueryTest::Context(), limits);
            REQUIRE(captured.HasValue());
            CHECK(*captured.Value().text == expected);
            const auto repeated = files->Text("a.cpp", QueryTest::Context(), limits);
            REQUIRE(repeated.HasValue());
            CHECK(*repeated.Value().text == expected);
            CHECK(repeated.Value().revision == captured.Value().revision);
            if (length > 1) {
                limits.maximumFileBytes = length - 1;
                const auto exceeded = files->Text("a.cpp", QueryTest::Context(), limits);
                REQUIRE(exceeded.HasError());
                CHECK(QueryTest::Matches(exceeded.ErrorValue(), ProjectReadErrors::Capacity));
            }
        }
    }
}

TEST_CASE("Native transfer deadline fences the next chunk and leaves the capability reusable", "[unit][platform][code-query]") {
    QueryTest::Directory directory;
    directory.Write("a.cpp", std::string(8193, 'a'));
    auto context = QueryTest::Context();
    context.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    unsigned chunks{};
    const auto files = FilesWithSecondReadObserver(directory, chunks, [&] {
        std::this_thread::sleep_until(context.deadline);
    });
    const auto expired = files->Text("a.cpp", context, {});
    REQUIRE(expired.HasError());
    CHECK(QueryTest::Matches(expired.ErrorValue(), ProjectReadErrors::Deadline));
    CHECK(chunks == 2);
    context = QueryTest::Context();
    const auto repeated = files->Text("a.cpp", context, {});
    REQUIRE(repeated.HasValue());
    CHECK(*repeated.Value().text == std::string(8193, 'a'));
}

TEST_CASE("Native transfer mutation never publishes a truncated or grown capture", "[unit][platform][code-query]") {
    QueryTest::Directory directory;
    for (const std::size_t changedLength : {1U, 12289U}) {
        DYNAMIC_SECTION("changed length " << changedLength) {
            directory.Write("a.cpp", std::string(8193, 'a'));
            unsigned chunks{};
            const auto files = FilesWithSecondReadObserver(directory, chunks, [&] {
                directory.Write("a.cpp", std::string(changedLength, 'b'));
            });
            const auto changed = files->Text("a.cpp", QueryTest::Context(), {});
            REQUIRE(changed.HasError());
            CHECK(QueryTest::Matches(changed.ErrorValue(), ProjectReadErrors::Stale));
        }
    }
}

TEST_CASE("Replacing a pathname after file open never reads the replacement object", "[unit][platform][code-query]") {
    QueryTest::Directory directory;
    directory.Write("a.cpp", "owned");
    bool replaced{};
    auto files =
        CodeFilesTesting::Create(directory.root, "project-one", 3, [&](const CodeFilesTesting::Boundary boundary, std::string_view) {
        if (boundary == CodeFilesTesting::Boundary::AfterFileOpen && !replaced) {
            replaced = true;
            std::filesystem::rename(directory.root / "a.cpp", directory.root / "old.cpp");
            directory.Write("a.cpp", "replacement secret");
        }
    });
    REQUIRE(files.HasValue());
    const auto result = files.Value()->Text("a.cpp", QueryTest::Context(), {});
    CHECK(replaced);
    if (result.HasValue())
        CHECK(*result.Value().text == "owned");
    else
        CHECK(QueryTest::Matches(result.ErrorValue(), ProjectReadErrors::Stale));
}

#ifndef _WIN32
TEST_CASE("POSIX FIFO substitution cannot block or disclose bytes", "[unit][platform][code-query]") {
    QueryTest::Directory directory;
    directory.Write("a.cpp", "owned");
    auto files =
        CodeFilesTesting::Create(directory.root, "project-one", 3, [&](const CodeFilesTesting::Boundary boundary, std::string_view) {
        if (boundary == CodeFilesTesting::Boundary::BeforeComponentOpen) {
            std::filesystem::remove(directory.root / "a.cpp");
            REQUIRE(::mkfifo((directory.root / "a.cpp").c_str(), 0600) == 0);
        }
    });
    REQUIRE(files.HasValue());
    CHECK(files.Value()->Text("a.cpp", QueryTest::Context(), {}).HasError());
}
#endif
