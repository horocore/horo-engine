#include "Horo/Application/ProjectSurfaceSettings.h"

#include <new>

namespace Horo::Application {
    /** @copydoc LoadProjectSurfaceSettings */
    Result<ProjectSurfaceFileSnapshot> LoadProjectSurfaceSettings(const std::filesystem::path &root,
                                                                  const std::string_view expectedProjectId) {
        try {
            if (!root.is_absolute() || root != root.lexically_normal() || expectedProjectId.empty())
                return Result<ProjectSurfaceFileSnapshot>::Failure(
                    MakeError(ProjectSurfaceErrors::Invalid, "A canonical root and explicit project identity are required."));
            auto source = Assets::CaptureAssetSource(root, root / ".horo/surfaces.json", MaximumProjectSurfaceBytes);
            if (source.HasError())
                return Result<ProjectSurfaceFileSnapshot>::Failure(source.ErrorValue());
            if (!source.Value().identity.exists)
                return Result<ProjectSurfaceFileSnapshot>::Failure(
                    MakeError(ProjectSurfaceErrors::Unavailable,
                              "Missing .horo/surfaces.json requires explicit project migration/adoption; no surfaces were inferred."));
            const auto &bytes = source.Value().bytes;
            const std::string_view text{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
            auto decoded = DecodeProjectSurfaceSettings(text);
            if (decoded.HasError())
                return Result<ProjectSurfaceFileSnapshot>::Failure(decoded.ErrorValue());
            if (decoded.Value().Input().projectId != expectedProjectId)
                return Result<ProjectSurfaceFileSnapshot>::Failure(
                    MakeError(ProjectSurfaceErrors::Invalid, "Surface catalog belongs to another project."));
            return Result<ProjectSurfaceFileSnapshot>::Success(
                {std::make_shared<const ProjectSurfaceSettings>(std::move(decoded).Value()), source.Value().identity});
        } catch (const std::bad_alloc &) {
            return Result<ProjectSurfaceFileSnapshot>::Failure(MakeError(ProjectSurfaceErrors::Capacity));
        }
    }
}  // namespace Horo::Application
