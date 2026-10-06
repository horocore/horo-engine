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

static_assert(std::is_aggregate_v<Horo::Runtime::SaveContentCaptureRequest>);
static_assert(std::is_trivially_copyable_v<Horo::Runtime::SaveContentCaptureRequest>);
static_assert(std::is_same_v<
              decltype(std::declval<const Horo::Runtime::SaveContentWorld &>().CaptureAtSafePoint(
                  std::declval<Horo::Runtime::SaveCaptureBarrier &>(), std::declval<const Horo::Runtime::SaveContentCaptureRequest &>(),
                  std::declval<Horo::Runtime::SaveParticipantRegistrySnapshot>(), Horo::Runtime::SaveDegradedWorldPolicy::Reject)),
              Horo::Result<Horo::Runtime::SaveContentCaptureOutcome>>);
static_assert(noexcept(std::declval<const Horo::Runtime::InstalledSaveContent &>().Close()));

static_assert(
    std::is_same_v<decltype(std::declval<const Horo::Runtime::SaveContentSnapshot &>().ReSave(
                       std::declval<const Horo::Runtime::SaveArchiveHeader &>(), std::declval<Horo::Runtime::ArchiveFormatVersion>())),
                   Horo::Result<Horo::Runtime::FinalizedSaveArchive>>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Runtime::InstalledSaveContent &>().Replace(
                                 std::declval<std::shared_ptr<const Horo::Assets::AssetArchiveProvider>>(),
                                 std::declval<std::vector<Horo::Runtime::GameplayPersistenceInstallation>>())),
                             Horo::Result<void>>);
