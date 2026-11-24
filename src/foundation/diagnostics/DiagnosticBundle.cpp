#include "Horo/Foundation/Diagnostics/DiagnosticBundle.h"

#include "../FoundationErrors.h"
#include "DiagnosticPrivacy.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Foundation/TransparentString.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace Horo::Diagnostics {
    namespace {
        using Json = nlohmann::json;
        constexpr int MaximumDiagnosticJsonDepth = 64;

        struct InvalidDiagnosticJsonStructure final {};

        /** @brief Rejects excessive nesting and ambiguous object members before privacy classification. */
        [[nodiscard]] std::optional<Json> ParseDiagnosticJson(const std::string &text) {
            std::vector<std::set<std::string>> objectKeys;
            try {
                return Json::parse(text, [&objectKeys](const int depth, const Json::parse_event_t event, Json &value) {
                    if (depth > MaximumDiagnosticJsonDepth)
                        throw InvalidDiagnosticJsonStructure{};
                    switch (event) {
                        case Json::parse_event_t::object_start:
                            objectKeys.emplace_back();
                            break;
                        case Json::parse_event_t::object_end:
                            objectKeys.pop_back();
                            break;
                        case Json::parse_event_t::key:
                            if (!objectKeys.back().insert(value.get_ref<const std::string &>()).second)
                                throw InvalidDiagnosticJsonStructure{};
                            break;
                        default:
                            break;
                    }
                    return true;
                }, false, true);
            } catch (const InvalidDiagnosticJsonStructure &) {
                return std::nullopt;
            }
        }

        /** @brief Escapes one JSON string into @p output. */
        void AppendJsonString(std::string &output, const std::string_view value) {
            output += '"';
            for (const unsigned char character : value) {
                switch (character) {
                    case '"':
                        output += R"(\")";
                        break;
                    case '\\':
                        output += R"(\\)";
                        break;
                    case '\n':
                        output += "\\n";
                        break;
                    case '\r':
                        output += "\\r";
                        break;
                    case '\t':
                        output += "\\t";
                        break;
                    default:
                        if (character >= 0x20U)
                            output += static_cast<char>(character);
                }
            }
            output += '"';
        }

        /** @brief Returns whether an archive entry cannot escape the bundle root. */
        [[nodiscard]] bool IsSafeArchivePath(const std::filesystem::path &path) {
            if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
                return false;
            return std::ranges::none_of(path, [](const auto &component) {
                return component == ".." || component == "." || component.empty();
            });
        }

        /** @brief Restricts support bundles to known diagnostic namespaces by default. */
        [[nodiscard]] bool IsAllowlistedArchivePath(const std::filesystem::path &path) {
            if (!IsSafeArchivePath(path))
                return false;
            if (std::distance(path.begin(), path.end()) < 2)
                return false;
            const auto first = *path.begin();
            return first == "logs" || first == "history" || first == "metadata" || first == "crash" || first == "configuration" ||
                   first == "packages";
        }

        [[nodiscard]] bool ContainsAbsolutePath(const std::string_view value) noexcept {
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (const bool boundary = index == 0 || std::isspace(static_cast<unsigned char>(value[index - 1])) ||
                                          value[index - 1] == '=' || value[index - 1] == '"' || value[index - 1] == '\'';
                    boundary && (value[index] == '/' || value[index] == '\\'))
                    return true;
                if (index + 2 < value.size() && std::isalpha(static_cast<unsigned char>(value[index])) && value[index + 1] == ':' &&
                    (value[index + 2] == '/' || value[index + 2] == '\\'))
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsPathBoundary(const std::string_view value, const std::size_t index) noexcept {
            return index == 0 || std::isspace(static_cast<unsigned char>(value[index - 1])) || value[index - 1] == '=' ||
                   value[index - 1] == '"' || value[index - 1] == '\'' || value[index - 1] == '(' || value[index - 1] == '[';
        }

        [[nodiscard]] std::string RedactAbsolutePaths(const std::string_view value) {
            std::string output;
            output.reserve(value.size());
            for (std::size_t index = 0; index < value.size();) {
                const bool posixPath = IsPathBoundary(value, index) && (value[index] == '/' || value[index] == '\\');
                const bool windowsPath = index + 2 < value.size() && std::isalpha(static_cast<unsigned char>(value[index])) &&
                                         value[index + 1] == ':' && (value[index + 2] == '/' || value[index + 2] == '\\');
                if (!posixPath && !windowsPath) {
                    output += value[index++];
                    continue;
                }
                output += "[REDACTED_PATH]";
                index += windowsPath ? 3U : 1U;
                while (index < value.size() && !std::isspace(static_cast<unsigned char>(value[index])) && value[index] != '"' &&
                       value[index] != '\'' && value[index] != ')' && value[index] != ']')
                    ++index;
            }
            return output;
        }

        constexpr std::array DiagnosticNamespaceFields{"category", "subsystem", "domain"};

        /** @brief Identifies a structured log, operation or error that may repeat structured details in its message. */
        [[nodiscard]] bool IsDiagnosticRecord(const Json &value) {
            return std::ranges::any_of(DiagnosticNamespaceFields, [&value](const char *key) {
                const auto field = value.find(key);
                return field != value.end() && field->is_string();
            });
        }

        /** @brief Reports whether the record belongs to a host-owned summary rather than the raw export. */
        [[nodiscard]] bool SummaryOnlyRecord(const Json &value, const std::span<const std::string> namespaces) {
            if (namespaces.empty() || !value.is_object())
                return false;
            for (const auto key : DiagnosticNamespaceFields) {
                const auto field = value.find(key);
                if (field == value.end() || !field->is_string())
                    continue;
                const std::string_view name = field->get_ref<const std::string &>();
                if (std::ranges::any_of(namespaces, [name](const std::string_view root) {
                    return name == root || (name.starts_with(root) && name[root.size()] == '.');
                }))
                    return true;
            }
            return false;
        }

        /** @brief Validates a bounded source namespace without permitting wildcard export rules. */
        [[nodiscard]] bool ValidSummaryNamespace(const std::string_view name) {
            if (name.empty() || name.size() > 64 || name.front() == '.' || name.back() == '.' || name.find("..") != std::string_view::npos)
                return false;
            return std::ranges::all_of(name, [](const char character) {
                return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '.' ||
                       character == '-' || character == '_';
            });
        }

        /** @brief Recognizes nested and flattened error chains used by host and automation diagnostics. */
        [[nodiscard]] bool IsCausalField(const std::string_view key) noexcept {
            return key == "cause" || key == "causes";
        }

        enum class DiagnosticRedaction {
            Kept,
            DescendantWithheld,
            Withheld
        };

        DiagnosticRedaction RedactJson(Json &value, std::span<const std::string> namespaces);

        /** @brief Withholds records that may repeat private descendant values, while preserving unrelated aggregate entries. */
        DiagnosticRedaction RedactObject(Json &value, const std::span<const std::string> namespaces) {
            using enum DiagnosticRedaction;
            const bool record = !namespaces.empty() && IsDiagnosticRecord(value);
            bool privateDescendant = false;
            for (auto &[childKey, child] : value.items()) {
                const auto redaction = RedactJson(child, namespaces);
                if (Detail::IsSensitiveKey(childKey))
                    child = "[REDACTED]";
                if (redaction == Kept)
                    continue;
                if (record || IsCausalField(childKey)) {
                    value = nullptr;
                    return Withheld;
                }
                privateDescendant = true;
            }
            return privateDescendant ? DescendantWithheld : Kept;
        }

        /** @brief Redacts every array entry and makes private descendants visible to the enclosing record. */
        DiagnosticRedaction RedactArray(Json &value, const std::span<const std::string> namespaces) {
            using enum DiagnosticRedaction;
            bool privateDescendant = false;
            for (Json &child : value)
                privateDescendant = RedactJson(child, namespaces) != Kept || privateDescendant;
            return privateDescendant ? DescendantWithheld : Kept;
        }

        /** @brief Redacts credentials and paths, and reports the extent of privacy-based record withdrawal. */
        DiagnosticRedaction RedactJson(Json &value, const std::span<const std::string> namespaces) {
            using enum DiagnosticRedaction;
            if (SummaryOnlyRecord(value, namespaces)) {
                value = nullptr;
                return Withheld;
            }
            if (value.is_object())
                return RedactObject(value, namespaces);
            if (value.is_array())
                return RedactArray(value, namespaces);
            if (value.is_string())
                value = RedactAbsolutePaths(value.get_ref<const std::string &>());
            return Kept;
        }

        [[nodiscard]] std::optional<std::string> RedactJsonlLines(const std::string &text, const std::span<const std::string> namespaces) {
            std::istringstream lines{text};
            std::string line;
            std::string sanitized;
            while (std::getline(lines, line)) {
                if (line.empty())
                    continue;
                auto record = ParseDiagnosticJson(line);
                if (!record)
                    return std::nullopt;
                if (record->is_discarded()) {
                    if (lines.eof() && !text.ends_with('\n'))
                        break;
                    return std::nullopt;
                }
                if (RedactJson(*record, namespaces) == DiagnosticRedaction::Withheld)
                    continue;
                sanitized += record->dump();
                sanitized += '\n';
            }
            return sanitized;
        }

        [[nodiscard]] std::optional<std::vector<std::byte>> RedactDiagnosticText(const std::filesystem::path &archivePath,
                                                                                 const std::vector<std::byte> &bytes,
                                                                                 const std::span<const std::string> namespaces) {
            const std::string text(reinterpret_cast<const char *>(bytes.data()), bytes.size());  // NOSONAR(cpp:S6022)
            std::string sanitized;

            if (archivePath.generic_string().find(".jsonl") != std::string::npos) {
                auto result = RedactJsonlLines(text, namespaces);
                if (!result)
                    return std::nullopt;
                sanitized = std::move(*result);
            } else if (archivePath.extension() == ".json") {
                if (text.empty())
                    return std::vector<std::byte>{};
                auto document = ParseDiagnosticJson(text);
                if (!document || document->is_discarded())
                    return std::nullopt;
                RedactJson(*document, namespaces);
                sanitized = document->dump();
                sanitized += '\n';
            } else {
                return std::nullopt;
            }
            std::vector<std::byte> result(sanitized.size());
            std::ranges::transform(sanitized, result.begin(), [](const char character) {
                return static_cast<std::byte>(character);
            });
            return result;
        }

        /** @brief Reads exactly one already-size-bounded regular file. */
        [[nodiscard]] std::vector<std::byte> ReadFile(const std::filesystem::path &path, const std::uintmax_t size) {
            std::vector<std::byte> bytes(static_cast<std::size_t>(size));
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
                return {};
            stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));  // NOSONAR(cpp:S6022)
            if (static_cast<std::uintmax_t>(stream.gcount()) != size)
                return {};
            return bytes;
        }

        [[nodiscard]] Error MakeBundleError(const ErrorCodeDescriptor &error, const std::string_view detail) {
            return MakeError(error, std::string{detail});
        }

        /** @brief Computes the ZIP CRC-32 checksum for one stored entry. */
        [[nodiscard]] std::uint32_t Crc32(const std::span<const std::byte> bytes) noexcept {
            std::uint32_t crc = 0xffffffffU;
            for (const std::byte byte : bytes) {
                crc ^= std::to_integer<std::uint8_t>(byte);
                for (unsigned int bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
            }
            return ~crc;
        }

        void WriteU16(std::ostream &stream, const std::uint16_t value) {
            const std::array bytes{static_cast<char>(value), static_cast<char>(value >> 8U)};
            stream.write(bytes.data(), bytes.size());
        }

        void WriteU32(std::ostream &stream, const std::uint32_t value) {
            const std::array bytes{static_cast<char>(value), static_cast<char>(value >> 8U), static_cast<char>(value >> 16U),
                                   static_cast<char>(value >> 24U)};
            stream.write(bytes.data(), bytes.size());
        }

        struct ZipEntryView {
            std::string name;
            std::span<const std::byte> bytes;
            std::uint32_t crc{};
            std::uint32_t localOffset{};
        };

        /** @brief Writes a dependency-free ZIP32 archive using the stored method. */
        [[nodiscard]] bool WriteZip(const std::filesystem::path &destination, std::vector<ZipEntryView> &entries) {
            std::ofstream stream(destination, std::ios::binary | std::ios::trunc);
            if (!stream)
                return false;

            for (ZipEntryView &entry : entries) {
                const auto offset = stream.tellp();
                if (offset < 0 || static_cast<std::uint64_t>(offset) > std::numeric_limits<std::uint32_t>::max() ||
                    entry.name.size() > std::numeric_limits<std::uint16_t>::max() ||
                    entry.bytes.size() > std::numeric_limits<std::uint32_t>::max())
                    return false;
                entry.crc = Crc32(entry.bytes);
                entry.localOffset = static_cast<std::uint32_t>(offset);
                WriteU32(stream, 0x04034b50U);
                WriteU16(stream, 20);
                WriteU16(stream, 0);
                WriteU16(stream, 0);
                WriteU16(stream, 0);
                WriteU16(stream, 0);
                WriteU32(stream, entry.crc);
                WriteU32(stream, static_cast<std::uint32_t>(entry.bytes.size()));
                WriteU32(stream, static_cast<std::uint32_t>(entry.bytes.size()));
                WriteU16(stream, static_cast<std::uint16_t>(entry.name.size()));
                WriteU16(stream, 0);
                stream.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
                stream.write(reinterpret_cast<const char *>(entry.bytes.data()),  // NOSONAR(cpp:S6022)
                             static_cast<std::streamsize>(entry.bytes.size()));
            }

            const auto centralOffsetPos = stream.tellp();
            if (centralOffsetPos < 0 || static_cast<std::uint64_t>(centralOffsetPos) > std::numeric_limits<std::uint32_t>::max() ||
                entries.size() > std::numeric_limits<std::uint16_t>::max())
                return false;
            const auto centralOffset = static_cast<std::uint32_t>(centralOffsetPos);
            for (const ZipEntryView &entry : entries) {
                WriteU32(stream, 0x02014b50U);
                WriteU16(stream, 20);
                WriteU16(stream, 20);
                WriteU16(stream, 0);
                WriteU16(stream, 0);
                WriteU16(stream, 0);
                WriteU16(stream, 0);
                WriteU32(stream, entry.crc);
                WriteU32(stream, static_cast<std::uint32_t>(entry.bytes.size()));
                WriteU32(stream, static_cast<std::uint32_t>(entry.bytes.size()));
                WriteU16(stream, static_cast<std::uint16_t>(entry.name.size()));
                WriteU16(stream, 0);
                WriteU16(stream, 0);
                WriteU16(stream, 0);
                WriteU16(stream, 0);
                WriteU32(stream, 0);
                WriteU32(stream, entry.localOffset);
                stream.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
            }
            const auto centralDirectoryEndPos = stream.tellp();
            if (centralDirectoryEndPos < 0 ||
                static_cast<std::uint64_t>(centralDirectoryEndPos) > std::numeric_limits<std::uint32_t>::max())
                return false;
            const auto centralSize = static_cast<std::uint32_t>(centralDirectoryEndPos) - centralOffset;
            WriteU32(stream, 0x06054b50U);
            WriteU16(stream, 0);
            WriteU16(stream, 0);
            WriteU16(stream, static_cast<std::uint16_t>(entries.size()));
            WriteU16(stream, static_cast<std::uint16_t>(entries.size()));
            WriteU32(stream, centralSize);
            WriteU32(stream, centralOffset);
            WriteU16(stream, 0);
            stream.flush();
            return stream.good();
        }

        struct PreparedEntry {
            std::filesystem::path archivePath;
            std::vector<std::byte> bytes;
            std::string digest;
        };

        struct PreparedBundleData {
            std::vector<PreparedEntry> prepared;
            std::vector<std::string> missingOptional;
            std::uintmax_t totalInputBytes = 0;
        };

        [[nodiscard]] Result<PreparedBundleData> CollectAndPrepareEntries(const DiagnosticBundleRequest &request) {
            PreparedBundleData data;
            data.prepared.reserve(request.entries.size());
            TransparentStringSet archivePaths;
            std::uintmax_t totalPreparedBytes = 0;
            std::error_code error;

            for (const DiagnosticBundleEntry &entry : request.entries) {
                error.clear();
                const std::string archivePath = entry.archivePath.generic_string();
                if (!IsAllowlistedArchivePath(entry.archivePath) || !archivePaths.insert(archivePath).second)
                    return Result<PreparedBundleData>::Failure(
                        MakeBundleError(ObservabilityErrors::InvalidBundleRequest,
                                        "Every diagnostic bundle entry must be a regular file with a unique safe relative archive path."));
                const bool exists = std::filesystem::exists(entry.sourcePath, error);
                if ((!exists || error) && entry.optional) {
                    data.missingOptional.push_back(archivePath);
                    continue;
                }
                if (const bool symlink = std::filesystem::is_symlink(entry.sourcePath, error);
                    !exists || error || symlink || !std::filesystem::is_regular_file(entry.sourcePath, error))
                    return Result<PreparedBundleData>::Failure(
                        MakeBundleError(ObservabilityErrors::InvalidBundleRequest,
                                        "Every diagnostic bundle source must be an existing non-symlink regular file."));
                const std::uintmax_t size = std::filesystem::file_size(entry.sourcePath, error);
                if (error || size > request.maxInputBytes - std::min(request.maxInputBytes, data.totalInputBytes))
                    return Result<PreparedBundleData>::Failure(
                        MakeBundleError(ObservabilityErrors::BundleSizeExceeded,
                                        "Diagnostic bundle input exceeded its configured aggregate byte limit."));
                data.totalInputBytes += size;
                std::vector<std::byte> bytes = ReadFile(entry.sourcePath, size);
                if (bytes.size() != size)
                    return Result<PreparedBundleData>::Failure(
                        MakeBundleError(ObservabilityErrors::BundleReadFailed, "Unable to read an allowlisted diagnostic file."));
                if (entry.redactSensitiveText) {
                    auto redacted = RedactDiagnosticText(entry.archivePath, bytes, request.summaryOnlyNamespaces);
                    if (!redacted.has_value())
                        return Result<PreparedBundleData>::Failure(
                            MakeBundleError(ObservabilityErrors::BundleReadFailed, "Invalid or too deeply nested JSON/JSONL."));
                    bytes = std::move(*redacted);
                }
                if (bytes.size() > request.maxInputBytes - std::min(request.maxInputBytes, totalPreparedBytes))
                    return Result<PreparedBundleData>::Failure(
                        MakeBundleError(ObservabilityErrors::BundleSizeExceeded,
                                        "Redacted diagnostic bundle input exceeded its configured aggregate byte limit."));
                totalPreparedBytes += bytes.size();
                const std::string digest = FormatSha256(ComputeSha256(bytes));
                data.prepared.push_back(PreparedEntry{.archivePath = entry.archivePath, .bytes = std::move(bytes), .digest = digest});
            }
            return Result<PreparedBundleData>::Success(std::move(data));
        }

        [[nodiscard]] Result<void> ValidateMetadata(const std::vector<std::pair<std::string, std::string>> &metadata) {
            TransparentStringSet metadataKeys;
            for (const auto &[key, val] : metadata) {
                if (key.empty() || Detail::IsSensitiveKey(key) || ContainsAbsolutePath(val) || !metadataKeys.insert(key).second)
                    return Result<void>::Failure(
                        MakeBundleError(ObservabilityErrors::InvalidBundleRequest, "Diagnostic bundle metadata keys must be unique."));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] std::string BuildManifestJson(const std::vector<std::pair<std::string, std::string>> &metadata,
                                                    const std::vector<std::string> &missingOptional,
                                                    const std::vector<PreparedEntry> &prepared) {
            std::string manifest = R"({"schemaVersion":1,"metadata":{)";
            for (std::size_t index = 0; index < metadata.size(); ++index) {
                if (index != 0)
                    manifest += ',';
                AppendJsonString(manifest, metadata[index].first);
                manifest += ':';
                AppendJsonString(manifest, metadata[index].second);
            }
            manifest += R"(},"missingOptional":[)";
            for (std::size_t index = 0; index < missingOptional.size(); ++index) {
                if (index != 0)
                    manifest += ',';
                AppendJsonString(manifest, missingOptional[index]);
            }
            manifest += R"(],"files":[)";
            for (std::size_t index = 0; index < prepared.size(); ++index) {
                if (index != 0)
                    manifest += ',';
                manifest += R"({"path":)";
                AppendJsonString(manifest, prepared[index].archivePath.generic_string());
                manifest += std::format(R"(,"bytes":{},"sha256":)", prepared[index].bytes.size());
                AppendJsonString(manifest, prepared[index].digest);
                manifest += '}';
            }
            manifest += "]}";
            return manifest;
        }

        /** @brief Rejects spellings that cannot name one new absolute output file. */
        bool ValidDestinationSpelling(const std::filesystem::path &path) {
            const auto name = path.filename();
            return !path.empty() && path.is_absolute() && !name.empty() && name != "." && name != ".." &&
                   path.native().find(std::filesystem::path::value_type{}) == std::filesystem::path::string_type::npos;
        }

        /** @brief Inspects the destination and its existing parent chain without creating directories. */
        Result<void> ValidateDestinationAncestors(const std::filesystem::path &outputPath) {
            std::error_code error;
            for (auto current = outputPath; !current.empty();) {
                const auto status = std::filesystem::symlink_status(current, error);
                if (error && error != std::errc::no_such_file_or_directory)
                    return Result<void>::Failure(
                        MakeBundleError(ObservabilityErrors::InvalidBundleRequest, "Diagnostic bundle destination cannot be inspected."));
                if (std::filesystem::exists(status) && (current == outputPath || !std::filesystem::is_directory(current, error) || error))
                    return Result<void>::Failure(MakeBundleError(ObservabilityErrors::InvalidBundleRequest,
                                                                 "Diagnostic bundle destination is occupied or has an invalid parent."));
                const auto parent = current.parent_path();
                if (parent == current)
                    break;
                current = parent;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateDiagnosticBundleDestination */
    Result<void> ValidateDiagnosticBundleDestination(const std::filesystem::path &outputPath) {
        if (!ValidDestinationSpelling(outputPath))
            return Result<void>::Failure(
                MakeBundleError(ObservabilityErrors::InvalidBundleRequest, "Diagnostic bundle output must name a new absolute file."));
        if (const auto ancestors = ValidateDestinationAncestors(outputPath); ancestors.HasError())
            return ancestors;
        std::error_code error;
        auto temporaryPath = outputPath;
        temporaryPath += ".tmp";
        const auto temporary = std::filesystem::symlink_status(temporaryPath, error);
        if ((error && error != std::errc::no_such_file_or_directory) || std::filesystem::exists(temporary))
            return Result<void>::Failure(MakeBundleError(ObservabilityErrors::InvalidBundleRequest,
                                                         "Diagnostic bundle temporary destination is occupied or unavailable."));
        return Result<void>::Success();
    }

    /** @copydoc GenerateDiagnosticBundle */
    Result<DiagnosticBundleSummary> GenerateDiagnosticBundle(const DiagnosticBundleRequest &request) {
        if (request.outputPath.empty() || request.outputPath.is_relative() || request.maxInputBytes == 0 || request.maxEntries == 0 ||
            request.maxMetadataEntries == 0 || request.entries.size() > request.maxEntries ||
            request.metadata.size() > request.maxMetadataEntries)
            return Result<DiagnosticBundleSummary>::Failure(
                MakeBundleError(ObservabilityErrors::InvalidBundleRequest,
                                "Diagnostic bundle output must be absolute and the byte limit must be non-zero."));

        if (request.summaryOnlyNamespaces.size() > 8 || !std::ranges::all_of(request.summaryOnlyNamespaces, ValidSummaryNamespace))
            return Result<DiagnosticBundleSummary>::Failure(
                MakeBundleError(ObservabilityErrors::InvalidBundleRequest,
                                "Diagnostic summary namespaces must be bounded canonical names."));
        if (const auto destination = ValidateDiagnosticBundleDestination(request.outputPath); destination.HasError())
            return Result<DiagnosticBundleSummary>::Failure(destination.ErrorValue());

        auto entryResult = CollectAndPrepareEntries(request);
        if (entryResult.HasError())
            return Result<DiagnosticBundleSummary>::Failure(entryResult.ErrorValue());

        auto [prepared, missingOptional, totalInputBytes] = std::move(entryResult).Value();
        std::ranges::sort(prepared, {}, &PreparedEntry::archivePath);
        std::ranges::sort(missingOptional);

        std::vector<std::pair<std::string, std::string>> metadata = request.metadata;
        std::ranges::sort(metadata, {}, &std::pair<std::string, std::string>::first);
        if (const auto metaResult = ValidateMetadata(metadata); metaResult.HasError())
            return Result<DiagnosticBundleSummary>::Failure(metaResult.ErrorValue());

        const std::string manifest = BuildManifestJson(metadata, missingOptional, prepared);

        std::error_code error;
        std::filesystem::create_directories(request.outputPath.parent_path(), error);
        if (error)
            return Result<DiagnosticBundleSummary>::Failure(
                MakeBundleError(ObservabilityErrors::BundleWriteFailed, "Unable to create the diagnostic bundle directory."));

        auto temporaryPath = request.outputPath;
        temporaryPath += ".tmp";
        if (std::filesystem::exists(temporaryPath, error) || error)
            return Result<DiagnosticBundleSummary>::Failure(
                MakeBundleError(ObservabilityErrors::BundleWriteFailed, "Diagnostic bundle temporary output already exists."));
        std::vector<std::byte> manifestBytes(manifest.size());
        std::ranges::transform(manifest, manifestBytes.begin(), [](const char character) {
            return static_cast<std::byte>(character);
        });
        std::vector<ZipEntryView> zipEntries;
        zipEntries.reserve(prepared.size() + 1U);
        zipEntries.push_back(ZipEntryView{.name = "manifest.json", .bytes = manifestBytes});
        for (const PreparedEntry &entry : prepared) {
            zipEntries.push_back(ZipEntryView{.name = entry.archivePath.generic_string(), .bytes = entry.bytes});
        }
        if (!WriteZip(temporaryPath, zipEntries)) {
            std::filesystem::remove(temporaryPath, error);
            return Result<DiagnosticBundleSummary>::Failure(
                MakeBundleError(ObservabilityErrors::BundleWriteFailed, "Unable to create the diagnostic ZIP archive."));
        }

        std::filesystem::rename(temporaryPath, request.outputPath, error);
        if (error) {
            std::filesystem::remove(temporaryPath, error);
            return Result<DiagnosticBundleSummary>::Failure(
                MakeBundleError(ObservabilityErrors::BundleWriteFailed, "Unable to commit the diagnostic bundle atomically."));
        }
        return Result<DiagnosticBundleSummary>::Success({.outputPath = request.outputPath,
                                                         .fileCount = prepared.size(),
                                                         .inputBytes = totalInputBytes,
                                                         .missingOptionalCount = missingOptional.size()});
    }
}  // namespace Horo::Diagnostics
