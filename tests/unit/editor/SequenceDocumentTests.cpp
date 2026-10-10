#include "Horo/Editor/SequenceDocument.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Editor/WorkspacePanelHost.h"
#include "SequenceDocumentTestSupport.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>

namespace {
    using namespace Horo::Editor;

    struct Project {
        std::filesystem::path root = std::filesystem::temp_directory_path() /
                                     ("horo-sequence-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        Project() {
            std::filesystem::create_directories(root);
        }

        ~Project() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        void Write(const std::string &bytes) const {
            std::ofstream output(root / "Başlangıç sequence.hsequence", std::ios::binary);
            output << bytes;
            REQUIRE(static_cast<bool>(output));
        }
    };
}  // namespace

TEST_CASE("Sequence opener owns a persistent typed document with exact revision", "[unit][editor][sequence]") {
    Project project;
    const std::string bytes = SequenceDocumentTests::Source();
    project.Write(bytes);
    DocumentIdentityRegistry registry;
    SourceFileOpenService opener(project.root, registry);
    WorkspacePanelHost host;
    host.AttachDocumentIdentityRegistry(registry);
    CHECK(opener.Classify("INTRO.HSEQUENCE").kind == SourceFileKind::Sequence);
    CHECK(Horo::Editor::ParseDocumentKind("sequence").Value() == DocumentKind::Sequence);
    const auto opened = opener.Open({.path = "Başlangıç sequence.hsequence", .origin = SourceOpenOrigin::AssetActivation});
    REQUIRE(opened.HasValue());
    REQUIRE(opened.Value().document);
    const auto identity = opened.Value().document->identity;
    CHECK(identity.key.kind == DocumentKind::Sequence);
    CHECK_FALSE(opened.Value().sourceSnapshot);
    auto source = SequenceDocument::Open(identity, opened.Value().location.absolutePath);
    REQUIRE(source.HasValue());
    CHECK(source.Value().asset.Data().tracks.size() == 4);
    CHECK(host.OpenDocument(identity.key).Value().identity == identity);
    CHECK(host.DocumentTabs().size() == 1);
    const auto again = opener.Open({.path = "./Başlangıç sequence.hsequence"});
    REQUIRE(again.HasValue());
    CHECK(again.Value().document->disposition == DocumentOpenDisposition::FocusExisting);
    CHECK(host.OpenDocument(identity.key).Value().disposition == DocumentOpenDisposition::FocusExisting);
    CHECK(host.DocumentTabs().size() == 1);
    project.Write(bytes + "\n");
    const auto changed = SequenceDocument::Open(identity, opened.Value().location.absolutePath);
    REQUIRE(changed.HasValue());
    CHECK(changed.Value().revision != source.Value().revision);
    REQUIRE(host.CloseDocument(identity.instance).HasValue());
    CHECK(host.DocumentTabs().empty());
    CHECK_FALSE(registry.Find(identity.instance));
}

TEST_CASE("Sequence source errors preserve schema and path admission boundaries", "[unit][editor][sequence]") {
    Project project;
    project.Write("{}");
    SourceFileOpenService opener(project.root);
    CHECK(opener.Open({.path = "../outside.hsequence"}).HasError());
    CHECK(opener.Open({.path = "missing.hsequence"}).HasError());
    const auto opened = opener.Open({.path = "Başlangıç sequence.hsequence"});
    REQUIRE(opened.HasValue());
    CHECK(SequenceDocument::Open(opened.Value().document->identity, opened.Value().location.absolutePath).HasError());
    project.Write(std::string(Horo::Cinematic::SequenceSchemaHardLimits::SourceBytes + 1, ' '));
    CHECK(SequenceDocument::Open(opened.Value().document->identity, opened.Value().location.absolutePath).HasError());
}

TEST_CASE("Timeline navigation is bounded for empty enormous and invalid ranges", "[unit][editor][sequence]") {
    SequenceTimelineState state{.firstFrame = 999, .playhead = 999, .zoom = 2.0};
    state.Clamp(240);
    CHECK(state.firstFrame == 120);
    CHECK(state.playhead == 239);
    CHECK(state.FrameAt(240, 0) == 120);
    CHECK(state.FrameAt(240, 1) == 239);
    CHECK(state.FrameAt(240, 0.5) == 179);
    state.zoom = std::numeric_limits<double>::quiet_NaN();
    state.Clamp(240);
    CHECK(state.zoom == 1.0);
    CHECK(state.firstFrame == 0);
    state.Clamp(0);
    CHECK(state.playhead == 0);
    CHECK(state.FrameAt(0, 1) == 0);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    CHECK(state.VisibleFrames(maximum) == maximum);
    CHECK(state.FrameAt(maximum, 1) == maximum - 1);
    CHECK(state.FrameAt(maximum, 0.5) < maximum);
    state.zoom = 1024;
    state.firstFrame = maximum;
    state.Clamp(maximum);
    CHECK(state.FrameAt(maximum, 1) == maximum - 1);
    const auto source = SequenceDocumentTests::Asset();
    const auto before = source.Data();
    state.playhead = state.FrameAt(source.Data().durationFrames, 0.8);
    CHECK(source.Data() == before);
    using enum Horo::Cinematic::SequenceTrackType;
    for (const auto type : {Transform, Property, CameraCut, Event})
        CHECK(IsTimelineTrackAvailable(type));
    for (const auto type : {Audio, SubSequence, Count})
        CHECK_FALSE(IsTimelineTrackAvailable(type));
}
