#include "Horo/Editor/FractureAssetDocument.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Editor::FractureAssetDocument>);
static_assert(std::is_move_constructible_v<Horo::Editor::FractureAssetDocument>);
static_assert(!std::is_convertible_v<Horo::Editor::FractureDocumentRevision, Horo::Editor::FractureSourceRevision>);
static_assert(!std::is_convertible_v<Horo::Editor::FractureRecipeId, Horo::Editor::FractureRecipeRevision>);

int main() {
    return Horo::Editor::FractureSourceSchemaVersion == 1 ? 0 : 1;
}
