#include "Horo/Editor/SceneDocumentTransaction.h"
#include "Horo/Foundation/Platform.h"

#include <type_traits>

static_assert(std::is_nothrow_default_constructible_v<Horo::FileEntryIdentity>);
static_assert(std::is_copy_constructible_v<Horo::Editor::SceneDocumentTransactions::PrefabReplacement>);

/** @brief Compiles the public prepared-placement and unavailable-identity contracts without source-private headers. */
void PrefabCreationPublicContract() {
    const Horo::FileEntryInfo unsupported;
    const bool unavailable = !unsupported.identity;
    static_cast<void>(unavailable);
}
