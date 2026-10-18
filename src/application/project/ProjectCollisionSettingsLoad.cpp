#include "Horo/Application/ProjectCollisionSettings.h"

#include <algorithm>
#include <cstddef>
#include <new>

namespace Horo::Application {
    namespace {
        const ErrorCodeDescriptor Invalid{.domain = ErrorDomainId{"horo.project.collision"},
                                          .code = ErrorCode{"collision.source.unavailable"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "The exact project collision document is unavailable.",
                                          .remediationHint =
                                              "Create or repair .horo/collision.json for this project before Physics activation.",
                                          .userActionable = true};
    }

    /** @copydoc LoadProjectCollisionSettings */
    Result<ProjectCollisionFileSnapshot> LoadProjectCollisionSettings(const std::filesystem::path &root,
                                                                      const std::string_view expectedProjectId) {
        try {
            if (!root.is_absolute() || root != root.lexically_normal() || expectedProjectId.empty())
                return Result<ProjectCollisionFileSnapshot>::Failure(
                    MakeError(Invalid, "Canonical root and explicit project ID required."));
            auto source = Assets::CaptureAssetSource(root, root / ".horo/collision.json", 4 * 1024 * 1024);
            if (source.HasError())
                return Result<ProjectCollisionFileSnapshot>::Failure(source.ErrorValue());
            if (!source.Value().identity.exists)
                return Result<ProjectCollisionFileSnapshot>::Failure(MakeError(Invalid, "The project collision document is missing."));
            const auto &bytes = source.Value().bytes;
            std::string text(bytes.size(), '\0');
            std::ranges::transform(bytes, text.begin(), [](const std::byte value) {
                return std::to_integer<char>(value);
            });
            auto decoded = DecodeProjectCollisionSettings(text);
            if (decoded.HasError())
                return Result<ProjectCollisionFileSnapshot>::Failure(decoded.ErrorValue());
            if (decoded.Value().Input().projectId != expectedProjectId)
                return Result<ProjectCollisionFileSnapshot>::Failure(MakeError(Invalid, "Collision document belongs to another project."));
            auto settings = std::make_shared<const ProjectCollisionSettings>(std::move(decoded).Value());
            return Result<ProjectCollisionFileSnapshot>::Success({std::move(settings), source.Value().identity});
        } catch (const std::bad_alloc &) {
            return Result<ProjectCollisionFileSnapshot>::Failure(MakeError(Invalid, "Collision source capacity is unavailable."));
        }
    }
}  // namespace Horo::Application
