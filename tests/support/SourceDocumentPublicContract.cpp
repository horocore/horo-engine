#include "Horo/Editor/SourceDocumentService.h"
#include "Horo/Editor/SourceFileOpenService.h"

#include <type_traits>

static_assert(std::is_copy_constructible_v<Horo::Editor::SourceDocumentSnapshot>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Editor::SourceDocumentService>);
static_assert(!std::is_copy_constructible_v<Horo::Editor::SourceDocumentService>);

/** @brief Qualifies the consumer through only its target-owned public surface. */
void QualifySourceDocumentPublicContract(Horo::Editor::SourceFileOpenService &open, const Horo::Editor::SourceOpenResult &route) {
    if (!route.sourceSnapshot)
        return;
    const auto snapshot = *route.sourceSnapshot;
    const Horo::Editor::SourceTextEdit edit{snapshot.Revision(), 0, 0, "// comment\n"};
    const auto result = open.Documents().Edit(snapshot.Identity().instance, edit);
    static_cast<void>(result);
}
