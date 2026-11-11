#include "ModuleScriptHostBridge.h"
#include "ScriptModuleProgram.h"

namespace Horo::Extensions {
    Result<void> ValidateScriptModuleActiveCapture(const ModuleScriptHostAuthority &host, const ScriptModuleProgram &program) {
        if (auto lane = host.RequireOpenOwner(); lane.HasError())
            return lane;
        if (auto current = program.VerifyCurrent(); current.HasError())
            return current;
        std::vector<ModuleScriptArtifactEvidence> artifacts;
        artifacts.reserve(program.Artifacts().size());
        for (const auto &actual : program.Artifacts())
            artifacts.emplace_back(actual.relative, actual.evidence);
        return host.ValidateActiveArtifacts(program.Manifest(), artifacts);
    }
}  // namespace Horo::Extensions
