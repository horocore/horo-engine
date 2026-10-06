// Deliberately reject transport/backend tokens in the declaration's transitive include closure.
// These macros are local to this compile-only contract probe, never product headers.
#define INetworkTransport         static_assert(false, "Gameplay declarations must not expose transport")
#define ITransportBackendInstance static_assert(false, "Gameplay declarations must not expose backend instances")
#define NetworkTransportConfig    static_assert(false, "Gameplay declarations must not expose transport configuration")
#define ISteamNetworkingSockets   static_assert(false, "Gameplay declarations must not expose native networking")
#define lua_State                 static_assert(false, "Gameplay declarations must not expose Lua implementation state")
#include "Horo/Gameplay/LuaBehavior.h"
#include "Horo/Gameplay/ReplicationRegistration.h"
#undef INetworkTransport
#undef ITransportBackendInstance
#undef NetworkTransportConfig
#undef ISteamNetworkingSockets
#undef lua_State

#include <type_traits>

using namespace Horo;
using namespace Horo::Gameplay;

static_assert(std::is_same_v<GameplayReplicationOwner, std::variant<ComponentTypeId, BehaviorTypeId, GameplayServiceId>>);
static_assert(std::is_same_v<decltype(GameplayReplicationRegistration::schema), Network::ReplicationSchemaDescriptor>);
static_assert(std::is_same_v<decltype(GameplayReplicationRegistration::schedule), GameplayReplicationSchedule>);
static_assert(std::is_same_v<decltype(GameplayReplicationBinding::schema), Network::ReplicationSchemaId>);
static_assert(std::is_same_v<decltype(GameplayReplicationSchedule::affinity), GameplayThreadAffinity>);

#if __has_include("Horo/Network/DeterministicTransport.h") || __has_include("steam/steamnetworkingsockets.h") || __has_include("lua.h")
#error "The declaration consumer must compile without concrete transport and scripting backend headers"
#endif

int main() {
    const auto owner = BehaviorTypeId::Parse("game.consumer.replicated");
    if (owner.HasError())
        return 1;
    auto program = LuaBehaviorProgram::Compile(
        R"(return horo.behavior {display_name="Consumer", replication={major=1, minor=0,
        minimum_minor=0, maximum_minor=0, capture_phase="gameplay", apply_phase="pre_physics",
        fields={{id=1, value_type=1, codec=1, kind="number", introduced_major=1, introduced_minor=0,
        condition="always", requirement="required", maximum_bytes=8, maximum_elements=1}}, tombstones={}}})",
        owner.Value(), "consumer.horo_script", {}, Network::ReplicationSchemaId::Create(1).Value(), {.value = "game.consumer"});
    if (program.HasError())
        return 1;
    auto lease = program.Value()->AcquireReplication();
    return lease.HasValue() && lease.Value().IsValid() && lease.Value().Registrations().size() == 1 ? 0 : 1;
}
