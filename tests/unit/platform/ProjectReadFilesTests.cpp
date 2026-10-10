#include "ProjectCodeFilesTesting.h"
#include "ProjectCodeQueryTestSupport.h"

#include <future>
#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace Horo;
using namespace Horo::Platform;
namespace QueryTest = Horo::Test::CodeQuery;

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
    std::filesystem::remove(directory.root / "hard.cpp");
    std::filesystem::create_directory_symlink(external.root, directory.root / "escape", error);
    if (error)
        SKIP("The host does not grant symlink creation for this test.");
    CHECK(files->Text("escape/secret.cpp", QueryTest::Context(), {}).HasError());
    CHECK(files->Files({}, QueryTest::Context(), {}).HasError());
    CHECK(CreateNativeProjectReadFiles(directory.root / "escape", "project-one", 3).HasError());
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
