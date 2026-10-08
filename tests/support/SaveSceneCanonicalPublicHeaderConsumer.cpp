#include "Horo/Runtime/Save/SaveArchiveContainerWriter.h"
#include "Horo/Runtime/Save/SaveSceneCanonicalState.h"

#include <type_traits>

static_assert(!std::is_default_constructible_v<Horo::Runtime::FinalizedSaveArchive>);
static_assert(Horo::Runtime::SaveSceneCanonicalSchemaVersion == 2);

static_assert(!std::is_default_constructible_v<Horo::Runtime::ValidatedSaveChunkDirectory>);
static_assert(std::is_same_v<decltype(Horo::Runtime::ValidateSaveChunkDirectory(Horo::Runtime::SaveChunkDirectory{},
                                                                                std::declval<const Horo::Runtime::SaveGameManifest &>())),
                             Horo::Result<Horo::Runtime::ValidatedSaveChunkDirectory>>);

static_assert(!std::is_default_constructible_v<Horo::Runtime::ValidatedSaveSceneCanonicalPreservation>);
static_assert(!std::is_copy_constructible_v<Horo::Runtime::ValidatedSaveSceneCanonicalPreservation>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Runtime::ValidatedSaveSceneCanonicalPreservation>);
