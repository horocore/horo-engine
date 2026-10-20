#include "Horo/Platform/NativeProjectIoOwners.h"

#include "Horo/Foundation/Utf8.h"
#include "RootedProjectOwnersInternal.h"

#include <new>
#include <utility>

namespace Horo::Platform {
    namespace {
        /** @brief Rejects overlapping derived namespaces before any native access or output creation. */
        bool Disjoint(const std::string_view a, const std::string_view b) {
            return a != b && !a.starts_with(std::string{b} + '/') && !b.starts_with(std::string{a} + '/');
        }
    }  // namespace

    /** @copydoc CreateNativeProjectIoOwners */
    Result<NativeProjectIoOwners> CreateNativeProjectIoOwners(const std::filesystem::path &authorizedRoot, std::string projectIdentity,
                                                              const std::uint64_t projectGeneration, const std::string_view cacheNamespace,
                                                              const std::string_view publicationNamespace) {
        using Return = Result<NativeProjectIoOwners>;
        try {
            if (!authorizedRoot.is_absolute() || authorizedRoot.lexically_normal() != authorizedRoot || projectIdentity.empty() ||
                projectIdentity.size() > 256 || !IsValidUtf8ScalarSequence(projectIdentity) || projectGeneration == 0 ||
                !IsSafeProjectReadPath(cacheNamespace) || !IsSafeProjectReadPath(publicationNamespace) ||
                !Disjoint(cacheNamespace, publicationNamespace))
                return Return::Failure(MakeError(ProjectReadErrors::Invalid));
            auto root = ProjectReadNative::OpenRoot(authorizedRoot);
            if (root.HasError())
                return Return::Failure(root.ErrorValue());
            auto cacheRoot = ProjectReadNative::OpenDirectory(root.Value());
            if (cacheRoot.HasError())
                return Return::Failure(cacheRoot.ErrorValue());
            auto publicationRoot = ProjectReadNative::OpenDirectory(root.Value());
            if (publicationRoot.HasError())
                return Return::Failure(publicationRoot.ErrorValue());
            auto source = RootedProjectOwnersInternal::Source(std::move(root).Value(), std::move(projectIdentity), projectGeneration);
            if (source.HasError())
                return Return::Failure(source.ErrorValue());
            auto cache = RootedProjectOwnersInternal::Cache(std::move(cacheRoot).Value(), cacheNamespace);
            if (cache.HasError())
                return Return::Failure(cache.ErrorValue());
            auto publication =
                RootedProjectOwnersInternal::Publication(std::move(publicationRoot).Value(), authorizedRoot, publicationNamespace);
            if (publication.HasError())
                return Return::Failure(publication.ErrorValue());
            return Return::Success({std::move(source).Value(), std::move(cache).Value(), std::move(publication).Value()});
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(ProjectReadErrors::Capacity));
        }
    }
}  // namespace Horo::Platform
