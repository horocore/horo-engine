#include "Horo/Extensions/ScriptCapabilityContext.h"

int main() {
    Horo::Extensions::ScriptCapabilityScope project{Horo::Extensions::ScriptCapabilityScopeKind::Project};
    const auto lease = project.Lease();
    project.Revoke();
    return lease.IsUsable() ? 1 : 0;
}
