#include "Horo/Runtime/Ui/RuntimeUiClient.h"
#include "Horo/Runtime/Ui/RuntimeUiService.h"

#include <type_traits>

using namespace Horo::Runtime::Ui;
static_assert(!std::is_aggregate_v<RuntimeUiClient::ConstructionKey>);
static_assert(!std::is_default_constructible_v<RuntimeUiClient::ConstructionKey>);
static_assert(!std::is_copy_constructible_v<RuntimeUiClient>);
static_assert(!std::is_invocable_v<decltype(&RuntimeUiClient::Derive), const RuntimeUiClient &, const Horo::CancellationToken &>);
static_assert(!std::is_copy_constructible_v<RuntimeUiService>);
static_assert(std::is_nothrow_move_constructible_v<RuntimeUiService>);
static_assert(std::is_copy_constructible_v<RuntimeUiProviderUpdate>);
static_assert(std::is_copy_constructible_v<RuntimeUiCommandResult>);

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
