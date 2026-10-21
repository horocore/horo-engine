#pragma once

#include "Horo/Foundation/ImmutableFileStorage.h"
#include "Horo/Foundation/Platform.h"
#include "ProjectReadNative.h"

namespace Horo::Platform::RootedProjectOwnersInternal {
    /** @brief Transfers an admitted directory identity into the existing read owner without reopening a native path. */
    [[nodiscard]] Result<std::shared_ptr<const IProjectReadFiles>> Source(ProjectReadNative::Handle root, std::string identity,
                                                                          std::uint64_t generation);
    /** @brief Transfers an admitted directory identity into a fixed immutable-cache owner without output creation. */
    [[nodiscard]] Result<std::shared_ptr<const ImmutableFileStorage>> Cache(ProjectReadNative::Handle root,
                                                                            std::string_view relativeNamespace);
    /** @brief Transfers an admitted directory identity into a fixed publication owner without output creation. */
    [[nodiscard]] Result<std::shared_ptr<DurableFileSystem>> Publication(ProjectReadNative::Handle root,
                                                                         const std::filesystem::path &logicalRoot,
                                                                         std::string_view relativeNamespace);
}  // namespace Horo::Platform::RootedProjectOwnersInternal
