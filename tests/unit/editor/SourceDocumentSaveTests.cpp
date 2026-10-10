#include "../../support/AllocationProbe.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Foundation/Platform.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <fstream>
#include <functional>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    struct Project final {
        std::filesystem::path root = std::filesystem::temp_directory_path() /
                                     ("horo-source-save-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        Project() {
            std::filesystem::create_directory(root);
        }

        ~Project() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        void Put(const std::filesystem::path &name, std::string_view bytes) {
            std::ofstream file(root / name, std::ios::binary);
            file << bytes;
            REQUIRE(file.good());
        }

        std::string Read(const std::filesystem::path &name) const {
            std::ifstream file(root / name, std::ios::binary);
            REQUIRE(file.good());
            return {std::istreambuf_iterator<char>{file}, {}};
        }
    };

    struct Files final : DurableFileSystem {
        NativeDurableFileSystem native;
        bool failWrite{};
        bool failPartialWrite{};
        std::optional<Error> writeError;
        bool failReplace{};
        bool failAfterCommit{};
        bool throwAfterCommit{};
        std::function<void()> beforeReplace;

        Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path, std::string_view owner) override {
            return native.TryAcquireExclusive(path, owner);
        }

        Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
            return native.AvailableBytes(path);
        }

        Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override {
            return native.WriteDurable(path, bytes);
        }

        Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                         std::filesystem::perms permissions) override {
            if (writeError)
                return Result<void>::Failure(*writeError);
            if (failWrite)
                return Result<void>::Failure(MakeError(SourceDocumentErrors::ReadFailed));
            if (failPartialWrite) {
                auto written = native.WritePrivateDurable(path, bytes.first(bytes.size() / 2), created, permissions);
                if (written.HasError())
                    return written;
                return Result<void>::Failure(MakeError(SourceDocumentErrors::ReadFailed));
            }
            return native.WritePrivateDurable(path, bytes, created, permissions);
        }

        Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) override {
            return native.CopyDurable(source, destination);
        }

        Result<void> AtomicReplace(const std::filesystem::path &, const std::filesystem::path &) override {
            return Result<void>::Failure(MakeError(SourceDocumentErrors::Invalid));
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                          AtomicFileReplacementReceipt &receipt) override {
            if (beforeReplace)
                beforeReplace();
            if (failReplace)
                return Result<void>::Failure(MakeError(SourceDocumentErrors::ReadFailed));
            auto result = native.AtomicReplaceTracked(prepared, destination, receipt);
            if (receipt.WasCommitted() && throwAfterCommit)
                throw std::bad_alloc{};
            if (receipt.WasCommitted() && failAfterCommit)
                return Result<void>::Failure(MakeError(SourceDocumentErrors::ReadFailed));
            return result;
        }

        Result<void> RemoveDurable(const std::filesystem::path &path) override {
            return native.RemoveDurable(path);
        }

        Result<void> SyncDirectory(const std::filesystem::path &path) override {
            return native.SyncDirectory(path);
        }
    };

    SourceDocumentSnapshot OpenEdit(SourceFileOpenService &open, const std::filesystem::path &path = "code.cpp") {
        auto opened = open.Open({.path = path});
        REQUIRE(opened.HasValue());
        REQUIRE(opened.Value().sourceSnapshot);
        const auto original = *opened.Value().sourceSnapshot;
        auto edited = open.Documents().Edit(original.Identity().instance, {original.Revision(), 0, original.Text().size(), "edited ü\r\n"});
        REQUIRE(edited.HasValue());
        return std::move(edited).Value();
    }

    SourceSaveRequest Request(const SourceDocumentSnapshot &snapshot) {
        return {snapshot.Identity().instance, snapshot.Revision(), {}};
    }
}  // namespace

TEST_CASE("Source save advances durable base while retained snapshots keep original bytes", "[source][save]") {
    Project project;
    project.Put("code.cpp", "base\n");
    SourceFileOpenService open(project.root);
    Files files;
    const auto edited = OpenEdit(open);
    const auto result = open.Documents().Save(Request(edited), files);
    REQUIRE(result.HasValue());
    CHECK(result.Value().disposition == SourceSaveDisposition::Durable);
    CHECK_FALSE(result.Value().snapshot.Dirty());
    CHECK(result.Value().snapshot.Revision() == edited.Revision() + 1);
    CHECK(result.Value().snapshot.BaseRevision() == result.Value().snapshot.Revision());
    CHECK(project.Read("code.cpp") == edited.Text());
    CHECK(edited.Dirty());
    CHECK(edited.DiskBase() == "base\n");
    CHECK(result.Value().snapshot.Metadata().newlines == SourceNewlines::CrLf);
    CHECK(open.Documents().Save(Request(edited), files).HasError());
}

TEST_CASE("Source conflict requires explicit exact disk consent and rejects later changes", "[source][save][conflict]") {
    Project project;
    project.Put("code.cpp", "base");
    SourceFileOpenService open(project.root);
    Files files;
    const auto edited = OpenEdit(open);
    project.Put("code.cpp", "external");
    auto denied = open.Documents().Save(Request(edited), files);
    REQUIRE(denied.HasError());
    CHECK(denied.ErrorValue().code.Value() == SourceDocumentErrors::SaveConflict.code.Value());
    CHECK(project.Read("code.cpp") == "external");
    auto request = Request(edited);
    request.approvedDiskBytes = "external";
    project.Put("code.cpp", "new external");
    CHECK(open.Documents().Save(request, files).HasError());
    request.approvedDiskBytes = "new external";
    REQUIRE(open.Documents().Save(request, files).HasValue());
    CHECK(project.Read("code.cpp") == edited.Text());
}

TEST_CASE("Source failures before replacement preserve dirty text base and original file", "[source][save][failure]") {
    const int failure = GENERATE(0, 1, 2);
    Project project;
    project.Put("code.cpp", "base");
    SourceFileOpenService open(project.root);
    Files files;
    const auto edited = OpenEdit(open);
    CancellationSource cancellation;
    files.failWrite = failure == 0;
    files.failReplace = failure == 1;
    if (failure == 2)
        cancellation.RequestCancellation();
    auto result = open.Documents().Save(Request(edited), files, cancellation.Token());
    REQUIRE(result.HasError());
    CHECK(project.Read("code.cpp") == "base");
    const auto current = open.Documents().Snapshot(edited.Identity().instance).Value();
    CHECK(current.Revision() == edited.Revision());
    CHECK(current.Text() == edited.Text());
    CHECK(current.DiskBase() == "base");
    CHECK_FALSE(std::filesystem::exists(project.root / "code.cpp.horo-source.temporary"));
}

TEST_CASE("Source visible unconfirmed replacement never reports rollback or clean base", "[source][save][durability]") {
    const bool throwing = GENERATE(false, true);
    Project project;
    project.Put("code.cpp", "base");
    SourceFileOpenService open(project.root);
    Files files;
    const auto edited = OpenEdit(open);
    files.failAfterCommit = !throwing;
    files.throwAfterCommit = throwing;
    auto result = open.Documents().Save(Request(edited), files);
    REQUIRE(result.HasValue());
    CHECK(result.Value().disposition == SourceSaveDisposition::VisibleDurabilityUnconfirmed);
    REQUIRE(result.Value().diagnostic);
    CHECK(result.Value().snapshot.Dirty());
    CHECK(result.Value().snapshot.DiskBase() == "base");
    CHECK(project.Read("code.cpp") == edited.Text());
    CHECK(open.ResolveClose(Request(result.Value().snapshot), SourceCloseDecision::Save, files).HasError());
    CHECK(open.DocumentRegistry().Find(edited.Identity().instance).has_value());
}

TEST_CASE("Source Save As publishes the same instance and leaves original file and leases unchanged", "[source][save][identity]") {
    Project project;
    project.Put("code.cpp", "base");
    SourceFileOpenService open(project.root);
    Files files;
    const auto edited = OpenEdit(open);
    auto saved = open.SaveAs(Request(edited), "saved ü.cpp", files);
    REQUIRE(saved.HasValue());
    const auto identity = saved.Value().snapshot.Identity();
    CHECK(identity.instance == edited.Identity().instance);
    CHECK(identity.key.source.Value() == "saved ü.cpp");
    CHECK(project.Read("code.cpp") == "base");
    CHECK(project.Read("saved ü.cpp") == edited.Text());
    CHECK(edited.Identity().key.source.Value() == "code.cpp");
    CHECK(open.DocumentRegistry().Find(identity.instance) == identity);
    auto focused = open.Open({.path = "saved ü.cpp"});
    REQUIRE(focused.HasValue());
    CHECK(focused.Value().document->disposition == DocumentOpenDisposition::FocusExisting);
    CHECK(open.SaveAs(Request(saved.Value().snapshot), "missing/target.cpp", files).HasError());
    CHECK(open.SaveAs(Request(saved.Value().snapshot), "../outside.cpp", files).HasError());
    CHECK(open.SaveAs(Request(saved.Value().snapshot), "canvas.uicanvas", files).HasError());
}

TEST_CASE("Source Save All retains per document failures and close choices remain explicit", "[source][save][batch]") {
    Project project;
    project.Put("code.cpp", "base");
    project.Put("other.cpp", "base");
    SourceFileOpenService open(project.root);
    Files files;
    const auto first = OpenEdit(open);
    const auto second = OpenEdit(open, "other.cpp");
    project.Put("code.cpp", "external");
    auto batch = open.Documents().SaveAll(files);
    REQUIRE(batch.HasValue());
    REQUIRE(batch.Value().size() == 2);
    CHECK(batch.Value()[0].instance == first.Identity().instance);
    CHECK(batch.Value()[0].result.HasError());
    CHECK(batch.Value()[1].result.HasValue());
    CHECK(project.Read("other.cpp") == second.Text());
    auto cancelled = open.ResolveClose(Request(first), SourceCloseDecision::Cancel, files);
    REQUIRE(cancelled.HasValue());
    CHECK_FALSE(cancelled.Value());
    CHECK(open.ResolveClose(Request(first), SourceCloseDecision::Save, files).HasError());
    const auto discarded = open.ResolveClose(Request(first), SourceCloseDecision::Discard, files);
    REQUIRE(discarded.HasValue());
    REQUIRE(discarded.Value());
    CHECK_FALSE(open.DocumentRegistry().Find(first.Identity().instance));
    CHECK(first.Text() == "edited ü\r\n");
}

TEST_CASE("Source publication blocks callback reentry and failed private creation cannot delete existing artifacts",
          "[source][save][lifetime]") {
    Project project;
    project.Put("code.cpp", "base");
    SourceFileOpenService open(project.root);
    Files files;
    const auto edited = OpenEdit(open);
    bool editRejected{}, shutdownRejected{}, closeRejected{};
    files.beforeReplace = [&] {
        editRejected = open.Documents().Edit(edited.Identity().instance, {edited.Revision(), 0, 0, "x"}).HasError();
        shutdownRejected = open.Documents().Shutdown().HasError();
        closeRejected = open.CloseDocument(edited.Identity().instance, true).HasError();
    };
    REQUIRE(open.Documents().Save(Request(edited), files).HasValue());
    CHECK(editRejected);
    CHECK(shutdownRejected);
    CHECK(closeRejected);
    const auto next = open.Documents().Edit(edited.Identity().instance, {edited.Revision() + 1, 0, 0, "next"}).Value();
    project.Put("code.cpp.horo-source.temporary", "abandoned precommit bytes");
    CHECK(open.Documents().Save(Request(next), files).HasError());
    CHECK(project.Read("code.cpp.horo-source.temporary") == "abandoned precommit bytes");
    CHECK(project.Read("code.cpp") == edited.Text());
}

TEST_CASE("Source permissions and full disk causes survive without publishing partial private bytes", "[source][save][filesystem]") {
    const bool diskFull = GENERATE(false, true);
    const ErrorCodeDescriptor failure{.domain = ErrorDomainId{"test.filesystem"},
                                      .code = ErrorCode{diskFull ? "disk_full" : "permission_denied"},
                                      .defaultSeverity = ErrorSeverity::Error,
                                      .summary = "Deterministic native write failure."};
    Project project;
    project.Put("code.cpp", "base");
    SourceFileOpenService open(project.root);
    Files files;
    const auto edited = OpenEdit(open);
    files.writeError = MakeError(failure);
    const auto denied = open.Documents().Save(Request(edited), files);
    REQUIRE(denied.HasError());
    CHECK(denied.ErrorValue().domain.Value() == failure.domain.Value());
    CHECK(denied.ErrorValue().code.Value() == failure.code.Value());
    CHECK(project.Read("code.cpp") == "base");
    CHECK_FALSE(std::filesystem::exists(project.root / "code.cpp.horo-source.temporary"));
    files.writeError.reset();
    files.failPartialWrite = true;
    CHECK(open.SaveAs(Request(edited), "partial.cpp", files).HasError());
    CHECK_FALSE(std::filesystem::exists(project.root / "partial.cpp"));
    CHECK_FALSE(std::filesystem::exists(project.root / "partial.cpp.horo-source.temporary"));
    CHECK(open.DocumentRegistry().Find(edited.Identity().instance) == edited.Identity());
    files.failPartialWrite = false;
    REQUIRE(open.SaveAs(Request(edited), "partial.cpp", files).HasValue());
}

TEST_CASE("Source Save As rejects occupied canonical identities and preserves unconfirmed relocation", "[source][save][identity]") {
    Project project;
    project.Put("code.cpp", "base");
    project.Put("other.cpp", "other");
    SourceFileOpenService open(project.root);
    Files files;
    const auto edited = OpenEdit(open);
    const auto other = open.Open({.path = "other.cpp"});
    REQUIRE(other.HasValue());
    auto approved = Request(edited);
    approved.approvedDiskBytes = "other";
    CHECK(open.SaveAs(approved, "./other.cpp", files).HasError());
    CHECK(project.Read("other.cpp") == "other");
    CHECK(open.DocumentRegistry().Find(edited.Identity().instance) == edited.Identity());
    bool nestedSaveRejected{}, nestedOpenRejected{};
    files.beforeReplace = [&] {
        nestedSaveRejected = open.Documents().Save(Request(edited), files).HasError();
        nestedOpenRejected = open.Open({.path = "code.cpp"}).HasError();
    };
    files.failAfterCommit = true;
    const auto relocated = open.SaveAs(Request(edited), "new.cpp", files);
    REQUIRE(relocated.HasValue());
    CHECK(nestedSaveRejected);
    CHECK(nestedOpenRejected);
    CHECK(relocated.Value().disposition == SourceSaveDisposition::VisibleDurabilityUnconfirmed);
    CHECK(open.DocumentRegistry().Find(edited.Identity().instance) == relocated.Value().snapshot.Identity());
    CHECK(project.Read("code.cpp") == "base");
    CHECK(project.Read("new.cpp") == edited.Text());
    CHECK(relocated.Value().snapshot.Dirty());
}

TEST_CASE("Source native private creation refuses symlink artifacts and permits empty durable files", "[source][save][filesystem]") {
    Project project;
    project.Put("code.cpp", "base");
    project.Put("unrelated.cpp", "unrelated");
    SourceFileOpenService open(project.root);
    Files files;
    const auto edited = OpenEdit(open);
    std::error_code error;
    std::filesystem::create_symlink(project.root / "unrelated.cpp", project.root / "code.cpp.horo-source.temporary", error);
    if (!error) {
        CHECK(open.Documents().Save(Request(edited), files).HasError());
        CHECK(project.Read("unrelated.cpp") == "unrelated");
        CHECK(project.Read("code.cpp") == "base");
        std::filesystem::remove(project.root / "code.cpp.horo-source.temporary");
    }
    const auto empty = open.Documents().Edit(edited.Identity().instance, {edited.Revision(), 0, edited.Text().size(), ""});
    REQUIRE(empty.HasValue());
    const auto saved = open.Documents().Save(Request(empty.Value()), files);
    REQUIRE(saved.HasValue());
    CHECK_FALSE(saved.Value().snapshot.Dirty());
    CHECK(project.Read("code.cpp").empty());
}

TEST_CASE("Source missing parent cannot create filesystem state or change the published revision", "[source][save][filesystem]") {
    Project project;
    std::filesystem::create_directory(project.root / "source");
    project.Put("source/code.cpp", "base");
    SourceFileOpenService open(project.root);
    Files files;
    const auto edited = OpenEdit(open, "source/code.cpp");
    std::filesystem::remove_all(project.root / "source");
    CHECK(open.Documents().Save(Request(edited), files).HasError());
    CHECK_FALSE(std::filesystem::exists(project.root / "source"));
    const auto current = open.Documents().Snapshot(edited.Identity().instance);
    REQUIRE(current.HasValue());
    CHECK(current.Value().Revision() == edited.Revision());
    CHECK(current.Value().Dirty());
}

TEST_CASE("Source native permissions survive atomic replacement and read-only files stay unchanged", "[source][save][permissions]") {
    Project project;
    project.Put("code.cpp", "base");
    SourceFileOpenService open(project.root);
    Files files;
    const auto edited = OpenEdit(open);
    constexpr auto mode = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec;
    std::filesystem::permissions(project.root / "code.cpp", mode);
    const auto saved = open.Documents().Save(Request(edited), files);
    REQUIRE(saved.HasValue());
#if !defined(_WIN32)
    CHECK(std::filesystem::status(project.root / "code.cpp").permissions() == mode);
#endif
    const auto next = open.Documents().Edit(edited.Identity().instance, {saved.Value().snapshot.Revision(), 0, 0, "next"});
    REQUIRE(next.HasValue());
    std::filesystem::permissions(project.root / "code.cpp", std::filesystem::perms::owner_read);
    const auto rejected = open.Documents().Save(Request(next.Value()), files);
    std::filesystem::permissions(project.root / "code.cpp", mode);
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == "save.read_only");
    CHECK(project.Read("code.cpp") == edited.Text());
    CHECK_FALSE(std::filesystem::exists(project.root / "code.cpp.horo-source.temporary"));
}

TEST_CASE("Source allocation failure before publication preserves the disk and current root", "[source][save][allocation]") {
    Project project;
    project.Put("code.cpp", "base");
    SourceFileOpenService open(project.root);
    Files files;
    const auto edited = OpenEdit(open);
    const auto request = Request(edited);
    std::optional<Result<SourceSaveResult>> result;
    {
        Horo::Tests::AllocationProbe::ScopedFailure failure;
        result = open.Documents().Save(request, files);
    }
    REQUIRE(result);
    REQUIRE(result->HasError());
    CHECK(project.Read("code.cpp") == "base");
    const auto current = open.Documents().Snapshot(edited.Identity().instance);
    REQUIRE(current.HasValue());
    CHECK(current.Value().Revision() == edited.Revision());
    CHECK(current.Value().Dirty());
    CHECK_FALSE(std::filesystem::exists(project.root / "code.cpp.horo-source.temporary"));
}

TEST_CASE("Source Save All preserves an earlier durable result when a later callback throws", "[source][save][batch]") {
    Project project;
    project.Put("code.cpp", "base");
    project.Put("other.cpp", "base");
    SourceFileOpenService open(project.root);
    Files files;
    const auto first = OpenEdit(open);
    const auto second = OpenEdit(open, "other.cpp");
    unsigned replacements{};
    files.beforeReplace = [&] {
        if (++replacements == 2)
            throw std::bad_alloc{};
    };
    const auto result = open.Documents().SaveAll(files);
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().size() == 2);
    CHECK(result.Value()[0].result.HasValue());
    CHECK(result.Value()[1].result.HasError());
    CHECK(project.Read("code.cpp") == first.Text());
    CHECK(project.Read("other.cpp") == "base");
    CHECK_FALSE(std::filesystem::exists(project.root / "other.cpp.horo-source.temporary"));
    const auto retained = open.Documents().Snapshot(second.Identity().instance);
    REQUIRE(retained.HasValue());
    CHECK(retained.Value().Revision() == second.Revision());
    CHECK(retained.Value().Dirty());
}

TEST_CASE("Source equal-byte unconfirmed save requires explicit recovery even when old base matches", "[source][save][durability]") {
    Project project;
    project.Put("code.cpp", "base");
    SourceFileOpenService open(project.root);
    Files files;
    const auto opened = open.Open({.path = "code.cpp"});
    REQUIRE(opened.HasValue());
    REQUIRE(opened.Value().sourceSnapshot);
    const auto initial = *opened.Value().sourceSnapshot;
    files.failAfterCommit = true;
    const auto uncertain = open.Documents().Save(Request(initial), files);
    REQUIRE(uncertain.HasValue());
    CHECK(uncertain.Value().snapshot.Dirty());
    CHECK(uncertain.Value().snapshot.Text() == uncertain.Value().snapshot.DiskBase());
    files.failAfterCommit = false;
    const auto implicit = open.Documents().Save(Request(uncertain.Value().snapshot), files);
    REQUIRE(implicit.HasError());
    CHECK(implicit.ErrorValue().code.Value() == SourceDocumentErrors::SaveOutcomeUnknown.code.Value());
    CHECK(open.ResolveClose(Request(uncertain.Value().snapshot), SourceCloseDecision::Save, files).HasError());
    auto approved = Request(uncertain.Value().snapshot);
    approved.approvedDiskBytes = "base";
    const auto recovered = open.Documents().Save(approved, files);
    REQUIRE(recovered.HasValue());
    CHECK_FALSE(recovered.Value().snapshot.Dirty());
    CHECK(recovered.Value().disposition == SourceSaveDisposition::Durable);
}

TEST_CASE("Source Save As honors disabled symlink policy and canonical destination collisions", "[source][save][policy]") {
    Project project;
    project.Put("code.cpp", "base");
    project.Put("other.cpp", "other");
    std::error_code error;
    std::filesystem::create_symlink(project.root / "other.cpp", project.root / "alias.cpp", error);
    if (error)
        SKIP("Host does not permit native symlink creation.");
    auto policy = SourceFilePolicy::Default();
    policy.allowSymlinkedFiles = false;
    SourceFileOpenService restricted(project.root, policy);
    Files files;
    const auto edited = OpenEdit(restricted);
    auto request = Request(edited);
    request.approvedDiskBytes = "other";
    const auto denied = restricted.SaveAs(request, "alias.cpp", files);
    REQUIRE(denied.HasError());
    CHECK(denied.ErrorValue().code.Value() == SourceOpenErrors::Unsafe.code.Value());
    CHECK(project.Read("other.cpp") == "other");
    CHECK(restricted.DocumentRegistry().Find(edited.Identity().instance) == edited.Identity());
    SourceFileOpenService allowed(project.root);
    const auto allowedEdit = OpenEdit(allowed);
    const auto other = allowed.Open({.path = "other.cpp"});
    REQUIRE(other.HasValue());
    auto approved = Request(allowedEdit);
    approved.approvedDiskBytes = "other";
    CHECK(allowed.SaveAs(approved, "alias.cpp", files).HasError());
    CHECK(project.Read("other.cpp") == "other");
    CHECK(allowed.DocumentRegistry().Find(allowedEdit.Identity().instance) == allowedEdit.Identity());
    const auto missing = restricted.SaveAs(Request(edited), "new.cpp", files);
    REQUIRE(missing.HasValue());
    CHECK(missing.Value().snapshot.Identity().key.source.Value() == "new.cpp");
}
