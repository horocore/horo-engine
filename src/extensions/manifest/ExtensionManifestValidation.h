#pragma once

#include "ExtensionManifestParsing.h"
#include "Horo/Extensions/ExtensionManifest.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Extensions::ManifestValidation {
    using ManifestParsing::ChildPath;
    using ManifestParsing::Json;
    using ManifestParsing::ManifestError;

    constexpr std::size_t MaximumSemanticVersionBytes = 64;
    constexpr std::size_t MaximumEntryBytes = 512;

    [[nodiscard]] inline bool IsAsciiDigit(const unsigned char character) noexcept {
        return character >= '0' && character <= '9';
    }

    [[nodiscard]] inline bool IsAsciiLower(const unsigned char character) noexcept {
        return character >= 'a' && character <= 'z';
    }

    [[nodiscard]] inline bool IsCanonicalTokenCharacter(const unsigned char character) noexcept {
        return IsAsciiLower(character) || IsAsciiDigit(character) || character == '_' || character == '-';
    }

    [[nodiscard]] inline bool IsPrereleaseCharacter(const unsigned char character) noexcept {
        const bool upper = character >= 'A' && character <= 'Z';
        return IsAsciiLower(character) || upper || IsAsciiDigit(character) || character == '-';
    }

    [[nodiscard]] inline bool IsCanonicalNumericComponent(const std::string_view value) {
        const bool leadingZero = value.size() > 1 && value.front() == '0';
        return !value.empty() && !leadingZero && std::ranges::all_of(value, IsAsciiDigit);
    }

    [[nodiscard]] inline bool IsValidPrereleaseIdentifier(const std::string_view value) {
        const bool numeric = std::ranges::all_of(value, IsAsciiDigit);
        const bool leadingZero = numeric && value.size() > 1 && value.front() == '0';
        return !value.empty() && !leadingZero && std::ranges::all_of(value, IsPrereleaseCharacter);
    }

    [[nodiscard]] inline bool IsPrereleaseValid(const std::string_view prerelease) {
        if (prerelease.empty())
            return false;
        std::size_t identifierStart = 0;
        while (identifierStart <= prerelease.size()) {
            const std::size_t end = prerelease.find('.', identifierStart);
            if (const std::string_view identifier =
                    prerelease.substr(identifierStart,
                                      end == std::string_view::npos ? prerelease.size() - identifierStart : end - identifierStart);
                !IsValidPrereleaseIdentifier(identifier)) {
                return false;
            }
            if (end == std::string_view::npos)
                return true;
            identifierStart = end + 1;
        }
        return false;
    }

    [[nodiscard]] inline bool IsCanonicalCoreVersion(const std::string_view core) {
        std::size_t componentStart = 0;
        for (int component = 0; component < 3; ++component) {
            const std::size_t end = component == 2 ? core.size() : core.find('.', componentStart);
            if (end == std::string_view::npos || end == componentStart)
                return false;
            if (const std::string_view digits = core.substr(componentStart, end - componentStart); !IsCanonicalNumericComponent(digits)) {
                return false;
            }
            componentStart = end + 1;
        }
        return componentStart == core.size() + 1;
    }

    [[nodiscard]] inline bool IsCanonicalSemanticVersion(const std::string_view value) {
        if (value.empty() || value.size() > MaximumSemanticVersionBytes || value.find('+') != std::string_view::npos)
            return false;
        const std::size_t dash = value.find('-');
        if (!IsCanonicalCoreVersion(value.substr(0, dash)))
            return false;
        return dash == std::string_view::npos || IsPrereleaseValid(value.substr(dash + 1));
    }

    [[nodiscard]] inline bool IsCanonicalIdSegment(const std::string_view segment) {
        if (segment.empty())
            return false;
        if (!IsAsciiLower(static_cast<unsigned char>(segment.front())) || segment.back() == '-')
            return false;
        return std::ranges::all_of(segment, [](const unsigned char character) {
            return IsAsciiLower(character) || IsAsciiDigit(character) || character == '-';
        });
    }

    /** @brief Applies the owning identity grammar independently to each bounded dotted segment. */
    template <typename SegmentPredicate>
    [[nodiscard]] inline bool IsCanonicalDottedName(const std::string_view value, const std::size_t maximumBytes,
                                                    const SegmentPredicate &validSegment) {
        if (value.empty() || value.size() > maximumBytes)
            return false;
        std::size_t start = 0;
        while (start <= value.size()) {
            const std::size_t end = value.find('.', start);
            if (const auto segment = value.substr(start, end == std::string_view::npos ? value.size() - start : end - start);
                !validSegment(segment))
                return false;
            if (end == std::string_view::npos)
                return true;
            start = end + 1;
        }
        return false;
    }

    [[nodiscard]] inline bool IsCanonicalId(const std::string_view value, const std::size_t maximumBytes) {
        return IsCanonicalDottedName(value, maximumBytes, IsCanonicalIdSegment);
    }

    /** @brief Validates extension-point names independently of package and capability identity grammar. */
    [[nodiscard]] inline bool IsCanonicalContributionType(const std::string_view value, const std::size_t maximumBytes) {
        return IsCanonicalDottedName(value, maximumBytes, [](const std::string_view segment) {
            return !segment.empty() && IsAsciiLower(static_cast<unsigned char>(segment.front())) && segment.back() != '_' &&
                   segment.back() != '-' && std::ranges::all_of(segment, IsCanonicalTokenCharacter);
        });
    }

    [[nodiscard]] inline bool IsCanonicalToken(const std::string_view value, const std::size_t maximumBytes) {
        if (value.empty() || value.size() > maximumBytes)
            return false;
        if (value.front() == '_' || value.back() == '_')
            return false;
        return std::ranges::all_of(value, IsCanonicalTokenCharacter);
    }

    [[nodiscard]] inline bool HasSafeEntryPrefix(const std::string_view value) {
        if (value.empty() || value.size() > MaximumEntryBytes)
            return false;
        if (value.front() == '/' || value.front() == '\\')
            return false;
        return value.find('\\') == std::string_view::npos && value.find(':') == std::string_view::npos;
    }

    [[nodiscard]] inline bool IsSafeEntryComponent(const std::string_view value) {
        return !value.empty() && value != "." && value != "..";
    }

    [[nodiscard]] inline bool IsSafeEntry(const std::string_view value) {
        if (!HasSafeEntryPrefix(value))
            return false;
        std::size_t start = 0;
        while (start <= value.size()) {
            const std::size_t end = value.find('/', start);
            if (const std::string_view component = value.substr(start, end == std::string_view::npos ? value.size() - start : end - start);
                !IsSafeEntryComponent(component)) {
                return false;
            }
            if (end == std::string_view::npos)
                return true;
            start = end + 1;
        }
        return false;
    }

    [[nodiscard]] inline std::optional<ExtensionHostPlatform> ParseHostPlatform(const std::string_view value) noexcept {
        using enum ExtensionHostPlatform;
        if (value == "windows")
            return Windows;
        if (value == "macos")
            return MacOS;
        if (value == "linux")
            return Linux;
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<ExtensionHostArchitecture> ParseHostArchitecture(const std::string_view value) noexcept {
        using enum ExtensionHostArchitecture;
        if (value == "x86_64")
            return X86_64;
        if (value == "arm64")
            return Arm64;
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<ExtensionBuildProfile> ParseBuildProfile(const std::string_view value) noexcept {
        using enum ExtensionBuildProfile;
        if (value == "debug")
            return Debug;
        if (value == "release")
            return Release;
        return std::nullopt;
    }

    [[nodiscard]] inline std::optional<ExtensionModuleRole> ParseRole(const std::string_view value) {
        using enum ExtensionModuleRole;
        if (value == "backend-capability")
            return BackendCapability;
        if (value == "editor-presentation")
            return EditorPresentation;
        if (value == "headless-tooling")
            return HeadlessTooling;
        if (value == "script-provider")
            return ScriptProvider;
        if (value == "runtime-participant")
            return RuntimeParticipant;
        return std::nullopt;
    }

    [[nodiscard]] inline bool HasOwningModule(const std::vector<ExtensionModuleManifest> &modules, const std::string_view moduleId) {
        return std::ranges::any_of(modules, [moduleId](const ExtensionModuleManifest &moduleManifest) {
            return moduleManifest.id == moduleId;
        });
    }

    class ManifestReader {
    protected:
        explicit ManifestReader(const ExtensionManifestLimits &limits) : limits_(limits) {}

        [[nodiscard]] bool Reject(const std::string_view path, const std::string_view code, const std::string_view reason) {
            failure_ = ManifestError(path, code, reason);
            return false;
        }

        [[nodiscard]] Result<ExtensionManifest> Failure(const std::string_view path, const std::string_view code,
                                                        const std::string_view reason) const {
            return Result<ExtensionManifest>::Failure(ManifestError(path, code, reason));
        }

        [[nodiscard]] Result<ExtensionManifest> CurrentFailure() {
            return Result<ExtensionManifest>::Failure(std::move(*failure_));
        }

        [[nodiscard]] const ExtensionManifestLimits &Limits() const noexcept {
            return limits_;
        }

        [[nodiscard]] bool AllowFields(const Json &object, const std::string_view path,
                                       const std::initializer_list<std::string_view> allowed) {
            for (const auto &[key, value] : object.items()) {
                static_cast<void>(value);
                if (std::ranges::find(allowed, std::string_view{key}) == allowed.end())
                    return Reject(ChildPath(path, key), "extension.manifest.unknown_field", "Unknown field is not allowed.");
            }
            return true;
        }

        [[nodiscard]] bool ReadString(const Json &object, const std::string_view key, const std::string_view path, std::string &output,
                                      const std::size_t maximumBytes, const bool required) {
            const auto found = object.find(key);
            if (found == object.end())
                return !required || Reject(ChildPath(path, key), "extension.manifest.missing_field", "Required field is missing.");
            if (!found->is_string())
                return Reject(ChildPath(path, key), "extension.manifest.invalid_type", "Field must be a string.");
            output = found->get<std::string>();
            if ((required && output.empty()) || output.size() > maximumBytes)
                return Reject(ChildPath(path, key), "extension.manifest.invalid_value", "String value is empty or exceeds its limit.");
            return true;
        }

        [[nodiscard]] bool ReadId(const Json &object, const std::string_view key, const std::string_view path, std::string &output) {
            return ReadString(object, key, path, output, limits_.maximumIdentifierBytes, true) &&
                   (IsCanonicalId(output, limits_.maximumIdentifierBytes) ||
                    Reject(ChildPath(path, key), "extension.manifest.invalid_identifier",
                           "Identity must use canonical lowercase dot-separated segments."));
        }

        [[nodiscard]] bool ValidateRootFields(const Json &document) {
            return AllowFields(document, "$",
                               {"schemaVersion", "package", "id", "version", "kind", "displayName", "description", "author",
                                "compatibility", "modules", "contributions"});
        }

        [[nodiscard]] bool ValidateSchemaVersion(const Json &document, std::uint32_t &schemaVersion) {
            const auto found = document.find("schemaVersion");
            if (found == document.end()) {
                schemaVersion = 1;
                return true;
            }
            if (!found->is_number_unsigned() || found->get<std::uint64_t>() != 1)
                return Reject("$.schemaVersion", "extension.manifest.unsupported_schema", "Only integer schemaVersion 1 is supported.");
            schemaVersion = 1;
            return true;
        }

        [[nodiscard]] const Json *SelectPackage(const Json &document) {
            const auto nested = document.find("package");
            if (nested == document.end())
                return &document;
            if (!nested->is_object()) {
                static_cast<void>(Reject("$.package", "extension.manifest.invalid_type", "Package field must be an object."));
                return nullptr;
            }
            constexpr std::array packageFields = {std::string_view{"id"},          std::string_view{"version"},
                                                  std::string_view{"kind"},        std::string_view{"displayName"},
                                                  std::string_view{"description"}, std::string_view{"author"}};
            for (const std::string_view field : packageFields) {
                if (document.contains(field)) {
                    static_cast<void>(Reject(ChildPath("$", field), "extension.manifest.ambiguous_field",
                                             "Package fields cannot appear both at the root and in $.package."));
                    return nullptr;
                }
            }
            if (!AllowFields(*nested, "$.package", {"id", "version", "kind", "displayName", "description", "author"}))
                return nullptr;
            return std::to_address(nested);
        }

    private:
        const ExtensionManifestLimits &limits_;
        std::optional<Error> failure_;
    };
}  // namespace Horo::Extensions::ManifestValidation
