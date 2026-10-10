#include "SaveSlotRetentionCloudCatalog.h"

#include <stdexcept>

namespace Horo::Runtime::SaveSlotLifecycleDetail {
    namespace {
        using Json = nlohmann::json;

        /** @brief Emits opaque byte arrays without interpreting provider keys as filenames or text. */
        [[nodiscard]] Json EncodeBytes(const std::vector<std::byte> &bytes) {
            Json value = Json::array();
            for (const auto byte : bytes)
                value.push_back(std::to_integer<std::uint8_t>(byte));
            return value;
        }

        /** @brief Bounds provider evidence before allocation and rejects signed or oversized bytes. */
        [[nodiscard]] std::vector<std::byte> DecodeBytes(const Json &value) {
            if (!value.is_array() || value.empty() || value.size() > 512)
                throw std::invalid_argument("Invalid cloud byte evidence");
            std::vector<std::byte> bytes;
            bytes.reserve(value.size());
            for (const auto &byte : value) {
                if (!byte.is_number_unsigned() || byte.get<std::uint64_t>() > 255)
                    throw std::invalid_argument("Invalid cloud byte");
                bytes.push_back(static_cast<std::byte>(byte.get<std::uint8_t>()));
            }
            return bytes;
        }

        /** @brief Converts fallible identity parsing to the private strict decoder's typed failure boundary. */
        template <typename T> T Checked(Result<T> value) {
            if (value.HasError())
                throw std::invalid_argument("Invalid cloud identity");
            return std::move(value).Value();
        }

        /** @brief Decodes only a complete finite object/CAS tuple. */
        [[nodiscard]] std::optional<SaveCloudObjectRef> DecodeObject(const Json &value) {
            if (value.is_null())
                return {};
            if (!value.is_array() || value.size() != 2)
                throw std::invalid_argument("Invalid cloud object");
            SaveCloudObjectRef object{DecodeBytes(value.at(0)), {}};
            if (!value.at(1).is_null())
                object.revision = DecodeBytes(value.at(1));
            return object;
        }
    }  // namespace

    /** @copydoc EncodeRetentionCloud */
    nlohmann::json EncodeRetentionCloud(const std::optional<SaveSlotRetentionCloudDeletion> &cloud) {
        if (!cloud)
            return nullptr;
        const auto &record = cloud->generation;
        Json object = nullptr;
        if (record.object)
            object = Json::array(
                {EncodeBytes(record.object->key), record.object->revision ? EncodeBytes(*record.object->revision) : Json(nullptr)});
        return Json::array({cloud->scope.provider.ToString(), cloud->scope.account.ToString(), record.slot.ToString(),
                            record.generation.ToString(), FormatSha256(record.archive.value), static_cast<std::uint8_t>(record.state),
                            std::move(object), record.lastConfirmed ? Json(record.lastConfirmed->ToString()) : Json(nullptr)});
    }

    /** @copydoc DecodeRetentionCloud */
    std::optional<SaveSlotRetentionCloudDeletion> DecodeRetentionCloud(const nlohmann::json &value, const SaveNamespaceId &name) {
        if (value.is_null())
            return {};
        if (!value.is_array() || value.size() != 8 || !value.at(5).is_number_unsigned() ||
            value.at(5).get<std::uint64_t>() > static_cast<std::uint8_t>(SaveCloudGenerationState::Failed))
            throw std::invalid_argument("Invalid cloud retention tuple");
        SaveSlotRetentionCloudDeletion cloud{.scope = {name, Checked(SaveCloudProviderId::Parse(value.at(0).get<std::string>())),
                                                       Checked(SaveCloudAccountId::Parse(value.at(1).get<std::string>()))},
                                             .generation = {.slot = Checked(SaveGameSlotId::Parse(value.at(2).get<std::string>())),
                                                            .generation = Checked(SlotGenerationId::Parse(value.at(3).get<std::string>())),
                                                            .archive = {Checked(ParseSha256(value.at(4).get<std::string>()))},
                                                            .state = static_cast<SaveCloudGenerationState>(value.at(5).get<std::uint8_t>()),
                                                            .object = DecodeObject(value.at(6))}};
        if (!value.at(7).is_null())
            cloud.generation.lastConfirmed = Checked(SaveCloudMutationId::Parse(value.at(7).get<std::string>()));
        return cloud;
    }

    /** @copydoc ValidRetentionCloud */
    bool ValidRetentionCloud(const Retired &record, const SaveNamespaceId &name) {
        if (!record.cloud)
            return !record.tombstone;
        if (record.cloud->scope.localNamespace != name)
            return false;
        SaveSlotIndex index{.revision = 1, .entries = {record.entry}};
        SaveCloudRevisionMetadata metadata{.revision = 1,
                                           .indexRevision = 1,
                                           .scope = record.cloud->scope,
                                           .records = {record.cloud->generation}};
        return ValidateSaveCloudRevisionMetadata(index, record.cloud->scope, metadata).HasValue();
    }
}  // namespace Horo::Runtime::SaveSlotLifecycleDetail
