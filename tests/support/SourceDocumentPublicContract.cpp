#include "Horo/Editor/SequenceDocument.h"
#include "Horo/Editor/SourceDocumentService.h"
#include "Horo/Editor/SourceEditingCommands.h"
#include "Horo/Editor/SourceFileOpenService.h"

#include <type_traits>

static_assert(std::is_copy_constructible_v<Horo::Editor::SourceDocumentSnapshot>);
static_assert(std::is_copy_constructible_v<Horo::Editor::SequenceDocument>);
static_assert(std::is_same_v<decltype(Horo::Editor::SequenceTimelineState::playhead), std::uint64_t>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Editor::SourceDocumentService>);
static_assert(!std::is_copy_constructible_v<Horo::Editor::SourceDocumentService>);
static_assert(noexcept(std::declval<const Horo::Editor::SourceDocumentSnapshot &>().SharesStateWith(
    std::declval<const Horo::Editor::SourceDocumentSnapshot &>())));

/** @brief Qualifies the consumer through only its target-owned public surface. */
void QualifySourceDocumentPublicContract(Horo::Editor::SourceFileOpenService &open, const Horo::Editor::SourceOpenResult &route) {
    if (!route.sourceSnapshot)
        return;
    const auto snapshot = *route.sourceSnapshot;
    static_cast<void>(snapshot.SharesStateWith(*route.sourceSnapshot));
    const Horo::Editor::SourceTextEdit edit{snapshot.Revision(), 0, 0, "// comment\n"};
    const auto result = open.Documents().Edit(snapshot.Identity().instance, edit);
    static_cast<void>(result);
}

/** @brief Qualifies source persistence without importing editor implementation headers. */
void QualifySourceSavePublicContract(Horo::Editor::SourceFileOpenService &open, Horo::DurableFileSystem &files,
                                     const Horo::Editor::SourceDocumentSnapshot &snapshot) {
    const Horo::Editor::SourceSaveRequest request{snapshot.Identity().instance, snapshot.Revision(), {}};
    static_cast<void>(open.Documents().Save(request, files));
    static_cast<void>(open.Documents().SaveAll(files));
    static_cast<void>(open.SaveAs(request, "copy.cpp", files));
    static_cast<void>(open.ResolveClose(request, Horo::Editor::SourceCloseDecision::Cancel, files));
}

static_assert(static_cast<unsigned>(Horo::Editor::DocumentKind::Prefab) > static_cast<unsigned>(Horo::Editor::DocumentKind::PCGGraph));

/** @brief Qualifies source history and editing plans through the owning staged public surface. */
void QualifySourceHistoryPublicContract(Horo::Editor::SourceDocumentService &documents,
                                        const Horo::Editor::SourceDocumentSnapshot &snapshot) {
    const auto instance = snapshot.Identity().instance;
    static_cast<void>(documents.History(instance));
    static_cast<void>(documents.Undo(instance, snapshot.Revision()));
    static_cast<void>(documents.Redo(instance, snapshot.Revision()));
    static_cast<void>(Horo::Editor::NavigateSourceLine(snapshot, 1));
    static_cast<void>(Horo::Editor::PlanSourceReplacement(snapshot, {}, "text"));
    static_cast<void>(Horo::Editor::PlanSourceNewline(snapshot, {}));
    static_cast<void>(Horo::Editor::PlanSourceIndentation(snapshot, {}, 4, false));
    static_cast<void>(Horo::Editor::FindSourceText(snapshot, {}, "text", Horo::Editor::SourceSearchDirection::Forward, true));
    static_cast<void>(Horo::Editor::PlanSourceReplaceAll(snapshot, {}, "text", "replacement"));
}
