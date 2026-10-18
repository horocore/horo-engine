#include "AssetRegistryPaths.h"

#include "../AssetErrors.h"
#include "Horo/Foundation/Utf8.h"

#include <cctype>

namespace Horo::Assets::Detail {
    namespace {
        /** @brief Classify portable Prefab extensions through an explicit UTF-8 native path conversion. */
        [[nodiscard]] bool IsPrefabSourcePath(const std::string_view sourcePath) {
            const std::filesystem::path path{std::u8string{sourcePath.begin(), sourcePath.end()}};
            return PortableFold(Utf8PathText(path.extension())) == ".prefab";
        }
    }  // namespace

    /** @copydoc PortableFold */
    [[nodiscard]] std::string PortableFold(const std::string_view value) {
        std::string folded{value};
        for (char &character : folded)
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        return folded;
    }

    /** @copydoc Utf8PathText */
    [[nodiscard]] std::string Utf8PathText(const std::filesystem::path &path) {
        const auto encoded = path.generic_u8string();
        return {encoded.begin(), encoded.end()};
    }

    /** @copydoc HasValidUtf8PathText */
    [[nodiscard]] bool HasValidUtf8PathText(const std::string_view value) {
        return IsValidUtf8ScalarSequence(value) && value.find('\0') == std::string_view::npos;
    }

    /** @copydoc ParseAssetPath */
    [[nodiscard]] Result<ProjectPath> ParseAssetPath(const std::string_view value) {
        if ((!value.starts_with("Assets/") && !value.starts_with("assets/")) || value.size() > 4096 || !HasValidUtf8PathText(value))
            return Result<ProjectPath>::Failure(MakeError(AssetErrors::RootInvalid, "Asset path is outside the project asset root."));
        return ProjectPath::Parse(value);
    }

    /** @copydoc ValidateSourceType */
    [[nodiscard]] Result<void> ValidateSourceType(const ProjectPath &sourcePath, const AssetTypeId &type) {
        if (!HasValidUtf8PathText(sourcePath.String()))
            return Result<void>::Failure(MakeError(AssetErrors::RootInvalid));
        if (constexpr std::string_view kPrefabAssetType = "core.prefab";
            IsPrefabSourcePath(sourcePath.String()) != (type.Value() == kPrefabAssetType))
            return Result<void>::Failure(MakeError(AssetErrors::TypeMismatch));
        return Result<void>::Success();
    }

}  // namespace Horo::Assets::Detail
