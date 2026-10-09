#include "../../support/AllocationProbe.h"
#include "Horo/Editor/SourceFileOpenService.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    struct SourceProject final {
        std::filesystem::path directory = std::filesystem::temp_directory_path() /
                                          ("horo-document-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        SourceProject() {
            std::filesystem::create_directory(directory);
        }

        ~SourceProject() {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }

        void Put(std::string_view bytes, const char *name = "code.cpp") const {
            std::ofstream stream(directory / name, std::ios::binary);
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            REQUIRE(stream.good());
        }

        DocumentIdentity Identity(const char *name = "code.cpp", std::uint64_t instance = 1) const {
            return {{DocumentKind::Source, SourceDocumentId::Parse(name).Value()}, DocumentInstanceId::Create(instance).Value()};
        }
    };

    template <class T> void ErrorIs(const Result<T> &result, const ErrorCodeDescriptor &descriptor) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
    }

    void CancelBufferGrowth(const std::size_t byteCount, void *context) noexcept {
        // Path and stream setup are smaller; this is reached synchronously inside chunked buffer growth.
        if (byteCount >= 64 * 1024)
            static_cast<CancellationSource *>(context)->RequestCancellation();
    }
}  // namespace

TEST_CASE("Source routing admits owned text and focuses without discarding edits", "[unit][editor][source][document]") {
    SourceProject project;
    project.Put("int x;\r\n");
    SourceFileOpenService open(project.directory);
    const auto first = open.Open({.path = "code.cpp", .mode = SourceOpenMode::EmbeddedOnly});
    REQUIRE(first.HasValue());
    REQUIRE(first.Value().sourceSnapshot);
    const auto held = *first.Value().sourceSnapshot;
    REQUIRE(held.Text() == "int x;\r\n");
    CHECK(held.Metadata().newlines == SourceNewlines::CrLf);
    const auto changed = open.Documents().Edit(held.Identity().instance, {held.Revision(), 4, 1, "y"});
    REQUIRE(changed.HasValue());
    const auto focus = open.Open({.path = project.directory / "code.cpp"});
    REQUIRE(focus.HasValue());
    REQUIRE(focus.Value().sourceSnapshot);
    CHECK(focus.Value().document->disposition == DocumentOpenDisposition::FocusExisting);
    CHECK(focus.Value().sourceSnapshot->Text() == "int y;\r\n");
    CHECK(focus.Value().sourceSnapshot->Dirty());
    CHECK(held.Text() == "int x;\r\n");
    ErrorIs(open.CloseDocument(held.Identity().instance), SourceDocumentErrors::Invalid);
    REQUIRE(open.CloseDocument(held.Identity().instance, true).HasValue());
    CHECK(changed.Value().Text() == "int y;\r\n");
    const auto reopened = open.Open({.path = "code.cpp"});
    REQUIRE(reopened.HasValue());
    CHECK(reopened.Value().sourceSnapshot->Identity().instance != held.Identity().instance);
    CHECK_FALSE(reopened.Value().sourceSnapshot->Dirty());
}

TEST_CASE("Source byte patches preserve scalar boundaries and deterministic dirty transitions", "[unit][editor][source][document]") {
    SourceProject project;
    project.Put("a\xc3\xa9\n");
    SourceDocumentService owner(project.directory);
    const auto original = owner.Open(project.Identity(), project.directory / "code.cpp");
    REQUIRE(original.HasValue());
    const auto instance = project.Identity().instance;
    ErrorIs(owner.Edit(instance, {1, 2, 0, "x"}), SourceDocumentErrors::Invalid);
    ErrorIs(owner.Edit(instance, {1, 0, 100, "x"}), SourceDocumentErrors::Invalid);
    ErrorIs(owner.Edit(instance, {1, 0, 0, std::string_view("\0", 1)}), SourceDocumentErrors::Binary);
    ErrorIs(owner.Edit(instance, {1, 0, 0, "\xc0\xaf"}), SourceDocumentErrors::Encoding);
    REQUIRE(owner.Snapshot(instance).Value().Revision() == 1);
    const auto changed = owner.Edit(instance, {1, 0, 1, "b"});
    REQUIRE(changed.HasValue());
    CHECK(changed.Value().Revision() == 2);
    CHECK(changed.Value().BaseRevision() == 1);
    CHECK(changed.Value().DiskBase() == original.Value().Text());
    CHECK(changed.Value().Dirty());
    ErrorIs(owner.Edit(instance, {1, 0, 1, "a"}), SourceDocumentErrors::Stale);
    const auto unchanged = owner.Edit(instance, {2, 0, 1, "b"});
    REQUIRE(unchanged.HasValue());
    CHECK(unchanged.Value().Revision() == 2);
    const auto reverted = owner.Edit(instance, {2, 0, 1, "a"});
    REQUIRE(reverted.HasValue());
    CHECK(reverted.Value().Revision() == 3);
    CHECK_FALSE(reverted.Value().Dirty());
}

TEST_CASE("Source external inspection never silently reloads a dirty buffer", "[unit][editor][source][document]") {
    SourceProject project;
    project.Put("base");
    SourceDocumentService owner(project.directory);
    const auto first = owner.Open(project.Identity(), project.directory / "code.cpp");
    REQUIRE(first.HasValue());
    const auto instance = project.Identity().instance;
    REQUIRE(owner.Edit(instance, {1, 0, 4, "local"}).HasValue());
    project.Put("external\n");
    const auto observed = owner.InspectDisk(instance);
    REQUIRE(observed.HasValue());
    CHECK(observed.Value().ExternalState() == SourceExternalState::Changed);
    CHECK(observed.Value().Text() == "local");
    CHECK(observed.Value().DiskBase() == "base");
    CHECK(observed.Value().Revision() == 3);
    CHECK(owner.InspectDisk(instance).Value().Revision() == 3);
    ErrorIs(owner.Reload(instance, 2), SourceDocumentErrors::Stale);
    const auto loaded = owner.Reload(instance, 3);
    REQUIRE(loaded.HasValue());
    CHECK(loaded.Value().Text() == "external\n");
    CHECK_FALSE(loaded.Value().Dirty());
    CHECK(loaded.Value().BaseRevision() == 4);
    CHECK(observed.Value().Text() == "local");
    CHECK(first.Value().DiskBase() == "base");
}

TEST_CASE("Removed and malformed external files preserve readable prior source roots", "[unit][editor][source][document]") {
    SourceProject project;
    project.Put("base");
    SourceDocumentService owner(project.directory);
    REQUIRE(owner.Open(project.Identity(), project.directory / "code.cpp").HasValue());
    const auto instance = project.Identity().instance;
    std::filesystem::remove(project.directory / "code.cpp");
    ErrorIs(owner.InspectDisk(instance), SourceDocumentErrors::Removed);
    CHECK(owner.Snapshot(instance).Value().ExternalState() == SourceExternalState::Removed);
    CHECK(owner.Snapshot(instance).Value().Text() == "base");
    ErrorIs(owner.Reload(instance, 2), SourceDocumentErrors::Removed);
    project.Put(std::string_view("bad\0", 4));
    ErrorIs(owner.InspectDisk(instance), SourceDocumentErrors::Binary);
    CHECK(owner.Snapshot(instance).Value().ExternalState() == SourceExternalState::Unreadable);
    CHECK(owner.Snapshot(instance).Value().DiskBase() == "base");
    project.Put("recovered");
    REQUIRE(owner.InspectDisk(instance).HasValue());
    const auto revision = owner.Snapshot(instance).Value().Revision();
    REQUIRE(owner.Reload(instance, revision).HasValue());
    CHECK(owner.Snapshot(instance).Value().Text() == "recovered");
}

TEST_CASE("Source admission rejects unsupported bytes without leaving routing identities", "[unit][editor][source][document]") {
    SourceProject project;
    SourceFileOpenService open(project.directory);
    for (const auto malformed : {std::string_view("\xff\xfe"), std::string_view("a\0b", 3), std::string_view("\xed\xa0\x80")}) {
        project.Put(malformed);
        CHECK(open.Open({.path = "code.cpp"}).HasError());
        CHECK(open.DocumentRegistry().Size() == 0);
    }
    project.Put("text");
    REQUIRE(open.Open({.path = "code.cpp"}).HasValue());
}

TEST_CASE("Source encoding metadata retains BOM and every newline family", "[unit][editor][source][document]") {
    SourceProject project;
    const std::array examples{std::pair{"plain", SourceNewlines::None}, std::pair{"a\nb\n", SourceNewlines::Lf},
                              std::pair{"a\rb\r", SourceNewlines::Cr}, std::pair{"a\r\nb\r\n", SourceNewlines::CrLf},
                              std::pair{"a\n\r\nb\r", SourceNewlines::Mixed}};
    for (const auto &[bytes, expected] : examples) {
        const std::string encoded = std::string("\xef\xbb\xbf") + bytes;
        project.Put(encoded);
        SourceDocumentService owner(project.directory);
        const auto captured = owner.Open(project.Identity(), project.directory / "code.cpp");
        REQUIRE(captured.HasValue());
        CHECK(captured.Value().Metadata() == SourceTextMetadata{true, expected});
        CHECK(captured.Value().Text() == encoded);
        ErrorIs(owner.Edit(project.Identity().instance, {1, 1, 0, "x"}), SourceDocumentErrors::Invalid);
    }
}

TEST_CASE("Source limits admit exact boundaries and reserve replacement overlap", "[unit][editor][source][document]") {
    SourceProject project;
    project.Put("12345678");
    SourceDocumentService owner(project.directory, {.maximumDocumentBytes = 8, .maximumResidentBytes = 16, .maximumDocuments = 1});
    REQUIRE(owner.Open(project.Identity(), project.directory / "code.cpp").HasValue());
    ErrorIs(owner.Edit(project.Identity().instance, {1, 8, 0, "x"}), SourceDocumentErrors::TooLarge);
    REQUIRE(owner.Edit(project.Identity().instance, {1, 0, 8, "abcdefgh"}).HasValue());
    ErrorIs(owner.Edit(project.Identity().instance, {2, 0, 8, "87654321"}), SourceDocumentErrors::TooLarge);
    project.Put("x", "other.cpp");
    ErrorIs(owner.Open(project.Identity("other.cpp", 2), project.directory / "other.cpp"), SourceDocumentErrors::TooLarge);
    project.Put("oversized");
    ErrorIs(owner.Reload(project.Identity().instance, 2), SourceDocumentErrors::TooLarge);
    CHECK(owner.Snapshot(project.Identity().instance).Value().Text() == "abcdefgh");
}

TEST_CASE("Detached source snapshots survive close shutdown move and synchronized worker reads", "[unit][editor][source][document]") {
    SourceProject project;
    project.Put("owned");
    SourceDocumentSnapshot held;
    {
        SourceDocumentService original(project.directory);
        held = original.Open(project.Identity(), project.directory / "code.cpp").Value();
        SourceDocumentService owner(std::move(original));
        ErrorIs(original.Snapshot(project.Identity().instance), SourceDocumentErrors::Closed);
        bool immutable = false;
        bool wrongThread = false;
        std::thread worker([held, &owner, &immutable, &wrongThread] {
            immutable = held.Text() == "owned" && held.Revision() == 1 && !held.Dirty();
            const auto query = owner.Snapshot(held.Identity().instance);
            wrongThread = query.HasError() && query.ErrorValue().code.Value() == SourceDocumentErrors::WrongThread.code.Value();
        });
        worker.join();
        CHECK(immutable);
        CHECK(wrongThread);
        REQUIRE(owner.Close(project.Identity().instance).HasValue());
        REQUIRE(owner.Shutdown().HasValue());
        REQUIRE(owner.Shutdown().HasValue());
        ErrorIs(owner.Open(project.Identity(), project.directory / "code.cpp"), SourceDocumentErrors::Closed);
    }
    CHECK(held.Text() == "owned");
    auto moved = std::move(held);
    CHECK(held.Revision() == 0);
    CHECK(held.Text().empty());
    CHECK(moved.Text() == "owned");
}

TEST_CASE("Cancelled and allocation-failed source operations do not publish partial roots", "[unit][editor][source][document]") {
    SourceProject project;
    project.Put("base");
    SourceDocumentService owner(project.directory);
    CancellationSource cancellation;
    cancellation.RequestCancellation();
    const auto instance = project.Identity().instance;
    ErrorIs(owner.Open(project.Identity(), project.directory / "code.cpp", cancellation.Token()), SourceDocumentErrors::Cancelled);
    REQUIRE(owner.Open(project.Identity(), project.directory / "code.cpp").HasValue());
    ErrorIs(owner.Edit(instance, {1, 0, 4, "new"}, cancellation.Token()), SourceDocumentErrors::Cancelled);
    ErrorIs(owner.InspectDisk(instance, cancellation.Token()), SourceDocumentErrors::Cancelled);
    ErrorIs(owner.Reload(instance, 1, cancellation.Token()), SourceDocumentErrors::Cancelled);
    Result<SourceDocumentSnapshot> failed = Result<SourceDocumentSnapshot>::Success({});
    {
        Tests::AllocationProbe::ScopedFailure inject;
        failed = owner.Edit(instance, {1, 0, 4, "new"});
    }
    ErrorIs(failed, SourceDocumentErrors::TooLarge);
    CHECK(owner.Snapshot(instance).Value().Text() == "base");
    CHECK(owner.Snapshot(instance).Value().Revision() == 1);
}

TEST_CASE("Source ownership rejects mismatched paths and foreign session keys", "[unit][editor][source][document]") {
    SourceProject project;
    project.Put("source");
    SourceDocumentService owner(project.directory);
    ErrorIs(owner.Open(project.Identity("other.cpp"), project.directory / "code.cpp"), SourceDocumentErrors::Invalid);
    ErrorIs(owner.Open(project.Identity(), "code.cpp"), SourceDocumentErrors::Invalid);
    auto canvas = project.Identity();
    canvas.key.kind = DocumentKind::UiCanvas;
    ErrorIs(owner.Open(canvas, project.directory / "code.cpp"), SourceDocumentErrors::Invalid);
    REQUIRE(owner.Open(project.Identity(), project.directory / "code.cpp").HasValue());
    ErrorIs(owner.Open(project.Identity("code.cpp", 2), project.directory / "code.cpp"), SourceDocumentErrors::TooLarge);
}

TEST_CASE("Source disk reinspection rejects a retargeted escaping symlink without losing local text", "[unit][editor][source][document]") {
    SourceProject project;
    SourceProject outside;
    project.Put("base");
    outside.Put("foreign");
    SourceDocumentService owner(project.directory);
    const auto instance = project.Identity().instance;
    REQUIRE(owner.Open(project.Identity(), project.directory / "code.cpp").HasValue());
    const auto local = owner.Edit(instance, {1, 0, 4, "local"});
    REQUIRE(local.HasValue());
    std::filesystem::remove(project.directory / "code.cpp");
    std::error_code error;
    std::filesystem::create_symlink(outside.directory / "code.cpp", project.directory / "code.cpp", error);
    if (error)
        SKIP("Host cannot create file symlinks; cross-platform privileged path remains untested here.");
    ErrorIs(owner.InspectDisk(instance), SourceDocumentErrors::Invalid);
    const auto rejected = owner.Snapshot(instance);
    REQUIRE(rejected.HasValue());
    CHECK(rejected.Value().ExternalState() == SourceExternalState::Unreadable);
    CHECK(rejected.Value().Text() == "local");
    CHECK(rejected.Value().DiskBase() == "base");
    ErrorIs(owner.Reload(instance, rejected.Value().Revision()), SourceDocumentErrors::Invalid);
    CHECK(local.Value().Text() == "local");
    CHECK(owner.Snapshot(instance).Value().Text() == "local");
}

TEST_CASE("Source owner survives presentation lease detachment and cancellation during bulk loading", "[unit][editor][source][document]") {
    SourceProject project;
    project.Put("base");
    SourceFileOpenService open(project.directory);
    auto presentation = open.Open({.path = "code.cpp"});
    REQUIRE(presentation.HasValue());
    const auto identity = presentation.Value().sourceSnapshot->Identity();
    presentation = Result<SourceOpenResult>::Success({});
    CHECK(open.Documents().Snapshot(identity.instance).Value().Text() == "base");
    REQUIRE(open.CloseDocument(identity.instance).HasValue());

    SourceDocumentService owner(project.directory);
    const auto prior = owner.Open(project.Identity(), project.directory / "code.cpp");
    REQUIRE(prior.HasValue());

    const std::string large(8 * 1024 * 1024, 'x');
    project.Put(large);
    project.Put(large, "large.cpp");
    CancellationSource cancellation;
    Result<SourceDocumentSnapshot> cancelled = prior;
    {
        const Tests::AllocationProbe::ScopedObserver observer{CancelBufferGrowth, &cancellation};
        cancelled = owner.Reload(identity.instance, 1, cancellation.Token());
    }
    REQUIRE(cancellation.Token().IsCancellationRequested());
    ErrorIs(cancelled, SourceDocumentErrors::Cancelled);
    CHECK(owner.Snapshot(identity.instance).Value().Text() == "base");
    CHECK(owner.Snapshot(identity.instance).Value().Revision() == 1);
    CHECK(prior.Value().DiskBase() == "base");

    CancellationSource admissionCancellation;
    {
        const Tests::AllocationProbe::ScopedObserver observer{CancelBufferGrowth, &admissionCancellation};
        cancelled = owner.Open(project.Identity("large.cpp", 2), project.directory / "large.cpp", admissionCancellation.Token());
    }
    REQUIRE(admissionCancellation.Token().IsCancellationRequested());
    ErrorIs(cancelled, SourceDocumentErrors::Cancelled);
    ErrorIs(owner.Snapshot(project.Identity("large.cpp", 2).instance), SourceDocumentErrors::Stale);
    CHECK(owner.Snapshot(identity.instance).Value().Revision() == 1);
}
