#include "AtomicShaderCookPackage.h"
#include "BaselineLightingCook.h"
#include "EditorBaselineLightingAssets.h"

namespace Horo::Editor::Offline {
    namespace {
        constexpr std::string_view CandidatePrefix{".baseline-lighting-package-"};

        /** @brief Requires the real read-only loader to admit the complete just-written candidate. */
        Result<void> ValidateWritten(const std::filesystem::path &path, const void *context) {
            const auto &expected = *static_cast<const BaselineLightingCookFamily *>(context);
            const auto decoded = LoadPackagedMetalBaselineLighting(path);
            if (decoded.HasError())
                return Result<void>::Failure(decoded.ErrorValue());
            for (std::size_t i = 0; i < 3; ++i) {
                if (expected.compiled[i].artifacts.size() != 1)
                    return Result<void>::Failure(MakeError(BaselineLightingAssetErrors::Unavailable));
                const auto &actual = decoded.Value().programs[i].artifacts.front();
                const auto &original = expected.compiled[i].artifacts.front();
                if (actual.payload != original.payload || actual.artifactKey != original.artifactKey)
                    return Result<void>::Failure(MakeError(BaselineLightingAssetErrors::Unavailable));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateBaselineLightingCookPaths */
    Result<void> ValidateBaselineLightingCookPaths(const std::filesystem::path &destination, const std::filesystem::path &scratch) {
        return ValidateAtomicShaderCookPaths(destination, scratch, CandidatePrefix);
    }

    /** @copydoc PublishBaselineLightingCook */
    Result<void> PublishBaselineLightingCook(const std::filesystem::path &destination, const BaselineLightingCookFamily &family,
                                             const Application::Internal::AuthenticatedMetalCookConfiguration &host) {
        std::array<AtomicShaderCookProgram, 3> programs;
        const auto prefixes = PackagedMetalBaselineLightingPrefixes();
        for (std::size_t i = 0; i < 3; ++i)
            programs[i] = {prefixes[i], &family.manifests[i], &family.compiled[i]};
        return PublishAtomicShaderCookPackage(destination,
                                              {programs, CandidatePrefix, "baseline-cook-provenance.json", ValidateWritten, &family}, host);
    }
}  // namespace Horo::Editor::Offline
