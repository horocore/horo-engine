#include "editor/screens/workspace/GameplayBehaviorRequestValidation.h"

#include "Horo/Foundation/Utf8.h"
#include "WorkspacePathText.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>

namespace Horo::Editor {
    namespace {
        constexpr std::string_view kForbiddenNameCharacters{R"(<>:"/\|?*)"};

        [[nodiscard]] std::string FoldPortableStem(const std::string_view name) {
            std::string stem{name};
            std::ranges::transform(stem, stem.begin(), [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return stem;
        }

        [[nodiscard]] bool ContainsForbiddenNameCharacter(const std::string_view name) {
            return std::ranges::any_of(name, [](const unsigned char character) {
                return character < 32U || kForbiddenNameCharacters.find(static_cast<char>(character)) != std::string_view::npos;
            });
        }

        [[nodiscard]] bool IsReservedPortableStem(const std::string_view stem) {
            if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul")
                return true;
            return stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' && stem[3] <= '9';
        }

        [[nodiscard]] bool IsPortableBehaviorName(const std::string_view name) {
            if (name.empty() || name == "." || name == ".." || name.ends_with(' ') || name.ends_with('.') ||
                ContainsForbiddenNameCharacter(name)) {
                return false;
            }
            const std::size_t extensionSeparator = name.find('.');
            return !IsReservedPortableStem(FoldPortableStem(name.substr(0, extensionSeparator)));
        }

        [[nodiscard]] bool IsCppIdentifier(const std::string_view name) {
            if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_'))
                return false;
            if (!std::ranges::all_of(name, [](const unsigned char character) {
                return std::isalnum(character) || character == '_';
            }))
                return false;
            constexpr auto keywords = std::to_array<std::string_view>({"alignas",
                                                                       "alignof",
                                                                       "and",
                                                                       "asm",
                                                                       "auto",
                                                                       "bitand",
                                                                       "bitor",
                                                                       "bool",
                                                                       "break",
                                                                       "case",
                                                                       "catch",
                                                                       "char",
                                                                       "class",
                                                                       "const",
                                                                       "constexpr",
                                                                       "continue",
                                                                       "default",
                                                                       "delete",
                                                                       "do",
                                                                       "double",
                                                                       "else",
                                                                       "enum",
                                                                       "explicit",
                                                                       "export",
                                                                       "extern",
                                                                       "false",
                                                                       "float",
                                                                       "for",
                                                                       "friend",
                                                                       "goto",
                                                                       "if",
                                                                       "inline",
                                                                       "int",
                                                                       "long",
                                                                       "mutable",
                                                                       "namespace",
                                                                       "new",
                                                                       "noexcept",
                                                                       "not",
                                                                       "nullptr",
                                                                       "operator",
                                                                       "or",
                                                                       "private",
                                                                       "protected",
                                                                       "public",
                                                                       "register",
                                                                       "reinterpret_cast",
                                                                       "return",
                                                                       "short",
                                                                       "signed",
                                                                       "sizeof",
                                                                       "static",
                                                                       "static_assert",
                                                                       "static_cast",
                                                                       "struct",
                                                                       "switch",
                                                                       "template",
                                                                       "this",
                                                                       "thread_local",
                                                                       "throw",
                                                                       "true",
                                                                       "try",
                                                                       "typedef",
                                                                       "typeid",
                                                                       "typename",
                                                                       "union",
                                                                       "unsigned",
                                                                       "using",
                                                                       "virtual",
                                                                       "void",
                                                                       "volatile",
                                                                       "wchar_t",
                                                                       "while",
                                                                       "xor"});
            return std::ranges::find(keywords, name) == std::ranges::end(keywords);
        }

        [[nodiscard]] Error InvalidRequestError() {
            static const ErrorCodeDescriptor descriptor{
                .domain = ErrorDomainId{"horo.editor.workspace"},
                .code = ErrorCode{"workspace.gameplay_behavior.invalid_request"},
                .defaultSeverity = ErrorSeverity::Error,
                .summary = "Gameplay behavior creation request is invalid.",
                .userActionable = true,
            };
            return MakeError(descriptor);
        }
    }  // namespace

    Result<void> ValidateCreateGameplayBehaviorRequest(const CreateGameplayBehaviorRequest &request) {
        if (!IsValidUtf8ScalarSequence(request.destination) || !IsValidUtf8ScalarSequence(request.baseName) ||
            request.destination.find('\0') != std::string::npos)
            return Result<void>::Failure(InvalidRequestError());
        const std::filesystem::path destination = WorkspaceNativePath(request.destination);
        const std::filesystem::path baseName = WorkspaceNativePath(request.baseName);
        // Project-relative paths are serialized with a portable '/' root. On Windows
        // `std::filesystem::path::is_absolute()` rejects that form because it has no
        // drive root, even though it is an absolute project path for the editor.
        const bool absoluteDestination = destination.is_absolute() || destination.has_root_directory();
        const bool validPath = !request.destination.empty() && absoluteDestination && !request.baseName.empty() &&
                               baseName.filename() == baseName && baseName.extension().empty();
        if (const bool validName =
                request.kind == GameplayBehaviorKind::Native ? IsCppIdentifier(request.baseName) : IsPortableBehaviorName(request.baseName);
            !validPath || !validName)
            return Result<void>::Failure(InvalidRequestError());
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
