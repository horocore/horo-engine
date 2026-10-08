#pragma once

#include "Horo/Editor/WorkspaceLayout.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Horo::Editor {
    /** @brief Serializes and restores the versioned editor workspace layout document. */
    class WorkspaceLayoutPersistence {
    public:
        static constexpr std::uint32_t CurrentSchemaVersion = 3;

        /**
         * @brief Copies a workspace into its versioned bounded JSON envelope.
         * @param layout Layout with at most 512 surfaces, 8192 state bytes each and 1 MiB total surface state.
         * @return JSON or an empty string when the surface bounds are exceeded; file reads are capped at 8 MiB.
         */
        [[nodiscard]] static std::string Serialize(const WorkspaceLayout &layout);
        [[nodiscard]] static std::optional<WorkspaceLayout> Deserialize(std::string_view json, std::string *error = nullptr);
        [[nodiscard]] static bool Save(const std::filesystem::path &path, const WorkspaceLayout &layout, std::string *error = nullptr);
        [[nodiscard]] static std::optional<WorkspaceLayout> Load(const std::filesystem::path &path, std::string *error = nullptr);
    };
}  // namespace Horo::Editor
