#include "ModuleScriptHostBridge.h"
#include "ScriptModuleProgram.h"

namespace Horo::Extensions {
    Result<void> ValidateScriptModuleActiveCapture(const ModuleScriptHostAuthority &host, const ScriptModuleProgram &program) {
        auto lane = host.RequireOpenOwner();
        if (lane.HasError())
            return lane;
        auto current = program.VerifyCurrent();
        if (current.HasError())
            return current;
        std::vector<ModuleScriptArtifactEvidence> artifacts;
        artifacts.reserve(program.Artifacts().size());
        for (const auto &actual : program.Artifacts())
            artifacts.push_back({actual.relative, actual.evidence});
        return host.ValidateActiveArtifacts(program.Manifest(), artifacts);
    }
}  // namespace Horo::Extensions
