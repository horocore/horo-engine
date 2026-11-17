#pragma once

#include "Horo/Foundation/Result.h"

#include <filesystem>
#include <memory>
#include <string_view>

namespace Horo::Platform {
    /**
     * @brief An abstract representation of a loaded dynamic library.
     */
    class DynamicLibrary {
    public:
        virtual ~DynamicLibrary() = default;

        /**
         * @brief Retrieves a symbol from the loaded dynamic library.
         * @param name The name of the symbol to retrieve.
         * @return A pointer to the symbol, or nullptr if it could not be found.
         */
        [[nodiscard]] virtual void *GetSymbol(std::string_view name) const noexcept = 0;  // NOSONAR(cpp:S5008)
    };

    /**
     * @brief Loads a dynamic library from the specified path.
     * @param path Full path to the library (.dll, .so, .dylib).
     * @return Result containing the loaded library, or an error if loading failed.
     */
    [[nodiscard]] Result<std::unique_ptr<DynamicLibrary>> LoadDynamicLibrary(std::string_view path);

    /**
     * @brief Loads a library from an absolute native filesystem path without a text encoding round trip.
     * @param path Path to the library. It must resolve to a regular file and contain no traversal components.
     * @return The loaded library or a typed validation or load error.
     */
    [[nodiscard]] Result<std::unique_ptr<DynamicLibrary>> LoadDynamicLibraryAtPath(const std::filesystem::path &path);
}  // namespace Horo::Platform
