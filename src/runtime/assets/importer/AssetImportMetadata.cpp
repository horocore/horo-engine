/**
 * @copydoc AssetImportMetadata.h
 */

#include "Horo/Assets/AssetImportMetadata.h"

#include "../AssetErrors.h"
#include "Horo/Foundation/Sha256.h"

#include <chrono>
#include <format>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>

namespace Horo::Assets {
    namespace {
        constexpr std::uintmax_t kMaximumMetadataBytes = 1024U * 1024U;
        constexpr std::uintmax_t kMaximumSourceBytes = 1024ULL * 1024ULL * 1024ULL;

        [[nodiscard]] std::optional<ImportSettingValue> ParseSettingValue(const ImportSettingDescriptor &descriptor,
                                                                          const std::string &serialized) {
            using enum ImportSettingKind;
            try {
                switch (descriptor.kind) {
                    case ImportSettingKind::Boolean:
                        if (serialized == "true")
                            return ImportSettingValue{true};
                        if (serialized == "false")
                            return ImportSettingValue{false};
                        return std::nullopt;
                    case ImportSettingKind::Integer:
                        return ImportSettingValue{static_cast<std::int64_t>(std::stoll(serialized))};
                    case ImportSettingKind::Float:
                        return ImportSettingValue{std::stod(serialized)};
                    case ImportSettingKind::Text:
                        return ImportSettingValue{serialized};
                    case ImportSettingKind::Choice: {
                        const auto choiceIndex = static_cast<std::size_t>(std::stoull(serialized));
                        if (choiceIndex >= descriptor.choices.size())
                            return std::nullopt;
                        return descriptor.choices[choiceIndex].value;
                    }
                }
            } catch (const std::invalid_argument &) {
                return std::nullopt;
            } catch (const std::out_of_range &) {
                return std::nullopt;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<AssetImportReason> ParseReason(const std::string_view value) {
            using enum AssetImportReason;
            if (value == "initial_import")
                return InitialImport;
            if (value == "manual_reimport")
                return ManualReimport;
            if (value == "source_changed")
                return SourceChanged;
            if (value == "importer_changed")
                return ImporterChanged;
            if (value == "module_changed")
                return ModuleChanged;
            return std::nullopt;
        }

        [[nodiscard]] Error MetadataError(std::string message) {
            return MakeError(AssetErrors::SidecarMalformed, std::move(message));
        }

        void PopulateSettings(const nlohmann::json &json, AssetImportMetadata &metadata) {
            if (const auto settings = json.find("importSettings"); settings != json.end() && settings->is_object()) {
                for (auto member = settings->begin(); member != settings->end(); ++member) {
                    if (member.value().is_string())
                        metadata.importSettings.try_emplace(member.key(), member.value().get<std::string>());
                }
            }
        }

        [[nodiscard]] Result<void> PopulateDependencies(const nlohmann::json &json, AssetImportMetadata &metadata) {
            if (const auto dependencies = json.find("dependencies"); dependencies != json.end() && dependencies->is_array()) {
                for (const auto &value : *dependencies) {
                    if (!value.is_string())
                        return Result<void>::Failure(MetadataError("Asset import dependency identity is invalid."));
                    auto dependency = AssetId::Parse(value.get_ref<const std::string &>());
                    if (dependency.HasError())
                        return Result<void>::Failure(MetadataError("Asset import dependency identity is invalid."));
                    metadata.dependencies.push_back(dependency.Value());
                }
            }
            return Result<void>::Success();
        }

        void PopulateReasons(const nlohmann::json &json, AssetImportMetadata &metadata) {
            if (const auto reasons = json.find("lastImportReasons"); reasons != json.end() && reasons->is_array()) {
                for (const auto &value : *reasons) {
                    if (!value.is_string())
                        continue;
                    if (const auto parsed = ParseReason(value.get_ref<const std::string &>()))
                        metadata.lastImportReasons.push_back(*parsed);
                }
            }
        }

        [[nodiscard]] Result<void> PopulateMetadataJsonFields(const nlohmann::json &json, AssetImportMetadata &metadata) {
            if (const auto sourceByteSize = json.find("sourceByteSize");
                sourceByteSize != json.end() && sourceByteSize->is_number_unsigned()) {
                metadata.sourceByteSize = sourceByteSize->get<std::uintmax_t>();
            }
            if (const auto sourceLastWriteTime = json.find("sourceLastWriteTime");
                sourceLastWriteTime != json.end() && sourceLastWriteTime->is_number_integer()) {
                metadata.sourceLastWriteTime = sourceLastWriteTime->get<std::int64_t>();
            }

            PopulateSettings(json, metadata);
            if (auto depResult = PopulateDependencies(json, metadata); depResult.HasError())
                return depResult;
            PopulateReasons(json, metadata);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc AssetImportReasonName */
    std::string_view AssetImportReasonName(const AssetImportReason reason) noexcept {
        using enum AssetImportReason;
        switch (reason) {
            case InitialImport:
                return "initial_import";
            case ManualReimport:
                return "manual_reimport";
            case SourceChanged:
                return "source_changed";
            case ImporterChanged:
                return "importer_changed";
            case ModuleChanged:
                return "module_changed";
        }
        return "manual_reimport";
    }

    /** @copydoc ReadAssetImportMetadata */
    Result<AssetImportMetadata> ReadAssetImportMetadata(const std::filesystem::path &absoluteMetadataPath) {
        if (!absoluteMetadataPath.is_absolute())
            return Result<AssetImportMetadata>::Failure(MetadataError("Asset import metadata path must be absolute."));

        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(absoluteMetadataPath, error);
        if (error || size == 0 || size > kMaximumMetadataBytes)
            return Result<AssetImportMetadata>::Failure(MetadataError("Asset import metadata is missing, empty, or too large."));

        std::ifstream input(absoluteMetadataPath, std::ios::binary);
        if (!input)
            return Result<AssetImportMetadata>::Failure(MetadataError("Asset import metadata could not be opened."));
        std::string contents(static_cast<std::size_t>(size), '\0');
        input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (input.gcount() != static_cast<std::streamsize>(contents.size()))
            return Result<AssetImportMetadata>::Failure(MetadataError("Asset import metadata could not be read completely."));

        return DecodeAssetImportMetadata(contents);
    }

    namespace {
        bool MetadataSchemaSupported(const nlohmann::json &json) {
            const auto schema = json.find("schemaVersion");
            return schema != json.end() && schema->is_number_unsigned() &&
                   schema->get<std::uint32_t>() == AssetImportMetadata::kSchemaVersion;
        }
    }  // namespace

    /** @copydoc DecodeAssetImportMetadata */
    Result<AssetImportMetadata> DecodeAssetImportMetadata(std::string_view contents) {
        if (contents.empty() || contents.size() > kMaximumMetadataBytes)
            return Result<AssetImportMetadata>::Failure(MetadataError("Asset import metadata is missing, empty, or too large."));
        const nlohmann::json json = nlohmann::json::parse(contents, nullptr, false, true);
        if (!json.is_object())
            return Result<AssetImportMetadata>::Failure(MetadataError("Asset import metadata schema is missing or unsupported."));

        const auto stringValue = [&json](const std::string_view key) {
            const auto found = json.find(key);
            return found != json.end() && found->is_string() ? found->get_ref<const std::string &>() : std::string{};
        };
        if (!MetadataSchemaSupported(json)) {
            return Result<AssetImportMetadata>::Failure(MetadataError("Asset import metadata schema is missing or unsupported."));
        }

        const auto id = AssetId::Parse(stringValue("assetId"));
        const auto type = AssetTypeId::Parse(stringValue("assetType"));
        if (id.HasError() || type.HasError())
            return Result<AssetImportMetadata>::Failure(MetadataError("Asset import metadata identity is invalid."));

        AssetImportMetadata metadata{
            .assetId = id.Value(),
            .assetType = type.Value(),
            .importerContributionId = stringValue("importerContributionId"),
            .importerVersion = stringValue("importerVersion"),
            .importerPackageId = stringValue("importerPackageId"),
            .importerModuleId = stringValue("importerModuleId"),
            .importerModuleVersion = stringValue("importerModuleVersion"),
            .absoluteSourcePath = stringValue("absoluteSourcePath"),
            .sourceExtension = stringValue("sourceExtension"),
            .sourceHash = stringValue("sourceHash"),
            .importedAtUtc = stringValue("importedAtUtc"),
        };

        if (auto fieldsResult = PopulateMetadataJsonFields(json, metadata); fieldsResult.HasError())
            return Result<AssetImportMetadata>::Failure(fieldsResult.ErrorValue());

        return Result<AssetImportMetadata>::Success(std::move(metadata));
    }

    /** @copydoc SerializeAssetImportMetadata */
    Result<std::string> SerializeAssetImportMetadata(const AssetImportMetadata &metadata) {
        if (!metadata.assetId.IsValid() || metadata.assetType.Value().empty() || !metadata.absoluteSourcePath.is_absolute() ||
            metadata.importerContributionId.empty() || metadata.importerVersion.empty() || metadata.importerModuleId.empty() ||
            metadata.importerModuleVersion.empty()) {
            return Result<std::string>::Failure(MetadataError("Asset import provenance is incomplete."));
        }

        nlohmann::json settings = nlohmann::json::object();
        for (const auto &[key, value] : metadata.importSettings)
            settings[key] = value;
        nlohmann::json dependencies = nlohmann::json::array();
        for (const AssetId dependency : metadata.dependencies)
            dependencies.push_back(dependency.ToString());
        nlohmann::json reasons = nlohmann::json::array();
        for (const AssetImportReason reason : metadata.lastImportReasons)
            reasons.push_back(AssetImportReasonName(reason));

        return Result<std::string>::Success(nlohmann::json{
                                                {"schemaVersion", AssetImportMetadata::kSchemaVersion},
                                                {"assetId", metadata.assetId.ToString()},
                                                {"assetType", metadata.assetType.Value()},
                                                {"importerContributionId", metadata.importerContributionId},
                                                {"importerVersion", metadata.importerVersion},
                                                {"importerPackageId", metadata.importerPackageId},
                                                {"importerModuleId", metadata.importerModuleId},
                                                {"importerModuleVersion", metadata.importerModuleVersion},
                                                {"absoluteSourcePath", metadata.absoluteSourcePath.lexically_normal().string()},
                                                {"sourceExtension", metadata.sourceExtension},
                                                {"sourceHash", metadata.sourceHash},
                                                {"sourceByteSize", metadata.sourceByteSize},
                                                {"sourceLastWriteTime", metadata.sourceLastWriteTime},
                                                {"importSettings", std::move(settings)},
                                                {"dependencies", std::move(dependencies)},
                                                {"lastImportReasons", std::move(reasons)},
                                                {"importedAtUtc", metadata.importedAtUtc},
                                            }
                                                .dump(2) +
                                            '\n');
    }

    /** @copydoc ResolveImportSettings */
    Result<std::vector<ImportSettingValue>> ResolveImportSettings(const AssetImporterContribution &contribution,
                                                                  const TransparentStringMap<std::string> &serializedSettings) {
        std::vector<ImportSettingValue> settings;
        settings.reserve(contribution.settings.size());
        for (const auto &descriptor : contribution.settings) {
            const auto found = serializedSettings.find("settings." + descriptor.id);
            if (found == serializedSettings.end()) {
                settings.push_back(descriptor.defaultValue);
                continue;
            }
            const auto parsed = ParseSettingValue(descriptor, found->second);
            if (!parsed.has_value())
                return Result<std::vector<ImportSettingValue>>::Failure(
                    MetadataError("Stored importer setting '" + descriptor.id + "' is invalid."));
            settings.push_back(*parsed);
        }
        return Result<std::vector<ImportSettingValue>>::Success(std::move(settings));
    }

    /** @copydoc CurrentImportTimestampUtc */
    std::string CurrentImportTimestampUtc() {
        const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
        return std::format("{:%FT%T%Ez}", now);
    }

    /** @copydoc ReadAssetImportSource */
    Result<std::vector<std::uint8_t>> ReadAssetImportSource(const std::filesystem::path &absoluteSourcePath) {
        if (!absoluteSourcePath.is_absolute())
            return Result<std::vector<std::uint8_t>>::Failure(
                MakeError(CookErrors::SourceReadFailed, "Import source path must be absolute."));

        std::error_code error;
        const auto status = std::filesystem::symlink_status(absoluteSourcePath, error);
        const std::uintmax_t size = std::filesystem::file_size(absoluteSourcePath, error);
        if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status) || size > kMaximumSourceBytes ||
            size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
            return Result<std::vector<std::uint8_t>>::Failure(
                MakeError(CookErrors::SourceReadFailed, "Import source is missing, unsafe, or too large."));
        }

        std::ifstream input(absoluteSourcePath, std::ios::binary);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (!input || (size != 0 && !input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)))) {
            return Result<std::vector<std::uint8_t>>::Failure(
                MakeError(CookErrors::SourceReadFailed, "Import source could not be read completely."));
        }
        return Result<std::vector<std::uint8_t>>::Success(std::move(bytes));
    }

    /** @copydoc HashAssetImportSource */
    std::string HashAssetImportSource(const std::span<const std::uint8_t> bytes) {
        return FormatSha256(ComputeSha256(std::span<const std::byte>{reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()}));
    }
}  // namespace Horo::Assets
