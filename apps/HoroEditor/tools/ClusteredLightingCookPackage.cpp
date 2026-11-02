#include "AtomicShaderCookPackage.h"
#include "ClusteredLightingCook.h"
#include "EditorClusteredLightingAssets.h"

namespace Horo::Editor::Offline {
    namespace {
        constexpr std::string_view CandidatePrefix{".clustered-lighting-package-"};

        Result<void> ValidateWritten(const std::filesystem::path &path, const void *) {
            auto decoded = LoadPackagedMetalClusteredLighting(path);
            if (decoded.HasError())
                return Result<void>::Failure(decoded.ErrorValue());
            return Result<void>::Success();
        }
    }  // namespace

    Result<void> ValidateClusteredLightingCookDestination(const std::filesystem::path &destination) {
        return ValidateAtomicShaderCookDestination(destination, CandidatePrefix);
    }

    Result<void> ValidateClusteredLightingCookPaths(const std::filesystem::path &destination, const std::filesystem::path &scratch) {
        return ValidateAtomicShaderCookPaths(destination, scratch, CandidatePrefix);
    }

    Result<void> PublishClusteredLightingCook(const std::filesystem::path &destination, const ClusteredLightingCookFamily &family,
                                              const Application::Internal::AuthenticatedMetalCookConfiguration &host) {
        std::array<AtomicShaderCookProgram, 4> programs;
        const auto prefixes = PackagedMetalClusteredLightingPrefixes();
        for (std::size_t index = 0; index < programs.size(); ++index)
            programs[index] = {prefixes[index], &family.manifests[index], &family.compiled[index]};
        return PublishAtomicShaderCookPackage(destination, {programs, CandidatePrefix, "clustered-cook-provenance.json", ValidateWritten},
                                              host);
    }
}  // namespace Horo::Editor::Offline
