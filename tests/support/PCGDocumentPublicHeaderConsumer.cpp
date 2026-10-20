#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Editor/DocumentRecoveryService.h"
#include "Horo/Editor/PCGGraphDocument.h"
#include "Horo/Editor/PCGGraphDocumentService.h"
#include "Horo/Editor/PCGPreviewSession.h"
#include "Horo/Editor/SemanticDocumentHistory.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Editor::PCGGraphDocument>);
static_assert(!std::is_copy_constructible_v<Horo::Editor::PCGGraphDocumentService>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Editor::PCGGraphDocument>);

int main() {
    const auto parsed = Horo::Editor::ParseDocumentKind("pcg_graph");
    return parsed.HasValue() && parsed.Value() == Horo::Editor::DocumentKind::PCGGraph ? 0 : 1;
}
