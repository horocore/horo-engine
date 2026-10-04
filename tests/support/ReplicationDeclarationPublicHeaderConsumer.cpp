#include "Horo/Network/NetworkProjectSettings.h"
#include "Horo/Network/ReplicationDeclarationPolicy.h"

#include <array>
#include <type_traits>

static_assert(!std::is_default_constructible_v<Horo::Network::ReplicationDeclarationAssessment>);
static_assert(std::is_nothrow_move_assignable_v<Horo::Network::NetworkProjectSettings>);

int main() {
    using namespace Horo::Network;
    const ReplicationDeclarationPolicyLimits limits{.descriptors = {.maximumSchemas = 4,
                                                                    .maximumFieldsPerSchema = 8,
                                                                    .maximumOwnerIdentityBytes = 64,
                                                                    .maximumDefaultBytesPerField = 16,
                                                                    .maximumTotalDefaultBytes = 32},
                                                    .maximumModules = 4,
                                                    .maximumDiagnostics = 16};
    const std::array<ReplicationDeclaration, 0> declarations{};
    const std::array<Horo::ModuleId, 0> modules{};
    const auto result = AssessReplicationDeclarations(declarations, {}, modules, ReplicationDeclarationUse::EditorInspection, limits);
    const auto input = DefaultNetworkProjectSettings(NetworkProjectSettingsId::Create(7).Value());
    if (input.HasError())
        return 1;
    const auto settings = NetworkProjectSettings::Create(input.Value());
    return result.HasValue() && !result.Value().Admitted() && result.Value().AdmittedSchemas().empty() && settings.HasValue() &&
                   ParseNetworkProjectSettings(SerializeNetworkProjectSettings(settings.Value())).HasValue()
               ? 0
               : 1;
}
