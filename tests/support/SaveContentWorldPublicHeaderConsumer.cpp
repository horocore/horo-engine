#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "Horo/Runtime/Scene/SaveContentWorld.h"
#include "Horo/Runtime/Scene/SavedSceneBootstrap.h"

#include <type_traits>

static_assert(!std::is_default_constructible_v<Horo::Runtime::ReconciledSaveContent>);
static_assert(!std::is_default_constructible_v<Horo::Runtime::SaveContentWorld>);
static_assert(!std::is_default_constructible_v<Horo::Runtime::ScenePublicationReceipt>);
static_assert(!std::is_copy_constructible_v<Horo::Runtime::PreparedSavedSceneBootstrap>);
static_assert(!std::is_copy_constructible_v<Horo::Runtime::GameplayPersistenceInstallation>);
static_assert(!std::is_default_constructible_v<Horo::Runtime::SaveContentSnapshot>);
static_assert(!std::is_aggregate_v<Horo::Runtime::SaveContentSnapshot>);
static_assert(!std::is_constructible_v<Horo::Runtime::SaveContentSnapshot, Horo::Runtime::RuntimeSaveSnapshot,
                                       std::shared_ptr<const Horo::Runtime::SaveContentDetail::AcceptedCaptureSeal>>);

static_assert(noexcept(std::declval<const Horo::Runtime::SaveContentSnapshot &>().Diagnostics()));
static_assert(noexcept(std::declval<const Horo::Runtime::SaveContentWorld &>().Diagnostics()));
static_assert(std::is_same_v<decltype(std::declval<const Horo::Runtime::SaveContentSnapshot &>().Diagnostics()),
                             std::span<const Horo::Runtime::SaveContentDiagnostic>>);
