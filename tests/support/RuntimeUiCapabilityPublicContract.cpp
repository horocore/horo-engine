#include "Horo/Runtime/Ui/RuntimeUiClient.h"
#include "Horo/Runtime/Ui/RuntimeUiService.h"

#include <type_traits>

static_assert(std::is_nothrow_invocable_v<decltype(&Horo::Runtime::Ui::RuntimeUiService::RevokeScene),
                                          Horo::Runtime::Ui::RuntimeUiService &, Horo::Runtime::SceneRuntimeId>);
static_assert(std::is_nothrow_invocable_v<decltype(&Horo::Runtime::Ui::RuntimeUiService::RevokeScene),
                                          const Horo::Runtime::Ui::RuntimeUiService &, Horo::Runtime::SceneRuntimeId>);
static_assert(std::is_invocable_v<decltype(&Horo::Runtime::Ui::RuntimeUiService::ScenePublication::CanPublish),
                                  const Horo::Runtime::Ui::RuntimeUiService::ScenePublication &>);

using namespace Horo::Runtime::Ui;
static_assert(!std::is_aggregate_v<RuntimeUiClient::ConstructionKey>);
static_assert(!std::is_default_constructible_v<RuntimeUiClient::ConstructionKey>);
static_assert(!std::is_copy_constructible_v<RuntimeUiClient>);
static_assert(!std::is_invocable_v<decltype(&RuntimeUiClient::Derive), const RuntimeUiClient &, const Horo::CancellationToken &>);
static_assert(!std::is_copy_constructible_v<RuntimeUiService>);
static_assert(std::is_nothrow_move_constructible_v<RuntimeUiService>);
static_assert(!std::is_copy_constructible_v<RuntimeUiService::ScenePublication>);
static_assert(std::is_nothrow_move_constructible_v<RuntimeUiService::ScenePublication>);
static_assert(noexcept(std::declval<RuntimeUiService::ScenePublication &>().Publish()));
static_assert(std::is_same_v<decltype(UiSceneTransitionRequest::previous), std::optional<Horo::Runtime::SceneRuntimeId>>);
static_assert(std::is_copy_constructible_v<RuntimeUiProviderUpdate>);
static_assert(std::is_copy_constructible_v<RuntimeUiCommandResult>);

using CoordinatedServiceReload = Horo::Result<UiAnimationReloadResult> (RuntimeUiService::*)(RuntimeUiInstanceId, UiAnimationOwner &,
                                                                                             UiAnimationReloadPreparation,
                                                                                             const Horo::CancellationToken &) const;
static_assert(std::is_same_v<decltype(static_cast<CoordinatedServiceReload>(&RuntimeUiService::Reload)), CoordinatedServiceReload>);
using ServiceAnimationCreate = Horo::Result<UiAnimationOwner> (*)(const RuntimeUiService &, RuntimeUiInstanceId, UiElementSlotAllocator &,
                                                                  RuntimeStyleRegistry, UiStyleResolver, UiAnimationCanvasDefinition,
                                                                  UiAnimationLimits);
static_assert(std::is_same_v<decltype(static_cast<ServiceAnimationCreate>(&UiAnimationOwner::Create)), ServiceAnimationCreate>);

/** @brief Checks the complete gameplay facade through the owning staged public boundary. */
void VerifyRuntimeUiCapabilityPublicContract(const RuntimeUiClient &client, RuntimeUiService &service) {
    (void)client.Validate();
    (void)client.Derive();
    (void)client.Find({});
    (void)client.Read({});
    (void)client.ScreenGuard();
    (void)client.Poll({});
    (void)client.NextEvent();
    (void)service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands);
    (void)service.CollectRetired();
}
