#include "SaveSlotLifecycleInternal.h"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Horo::Runtime::SaveSlotLifecycleDetail {
    namespace {
        using Json = nlohmann::json;
        constexpr std::size_t kDigestBytes = 32;

        /** @brief Propagates typed validation failure through the private catalog decoder only. */
        template <typename T> T Checked(Result<T> value) {
            if (value.HasError())
                throw std::invalid_argument("Invalid lifecycle catalog value");
            return std::move(value).Value();
        }

        /** @brief Encodes bounded catalog metadata; physical filenames never enter this representation. */
        [[nodiscard]] Json EncodeEntry(const SaveSlotCatalogEntry &entry) {
            const auto &p = entry.publication;
            return Json::array(
                {p.slot.ToString(), p.generation.ToString(), static_cast<std::uint8_t>(p.kind), p.savedAtUnixMilliseconds,
                 p.playTimeNanoseconds, p.baseScene.ToString(), p.checkpoint ? Json(p.checkpoint->ToString()) : Json(nullptr),
                 p.thumbnail ? Json(p.thumbnail->ToString()) : Json(nullptr), p.productCompatibility.Value(), p.saveSchema.Value(),
                 p.projectBuildId, FormatSha256(p.canonicalState.value), FormatSha256(p.archiveContent.value),
                 static_cast<std::uint8_t>(p.cloudState), entry.display.displayName, entry.display.summary});
        }

        /** @brief Decodes one strict, fixed-width metadata tuple with independent field validation. */
        [[nodiscard]] SaveSlotCatalogEntry DecodeEntry(const Json &value) {
            if (!value.is_array() || value.size() != 16)
                throw std::invalid_argument("Invalid lifecycle catalog entry");
            const auto text = [&value](const std::size_t index) {
                return value.at(index).get<std::string>();
            };
            const auto number = [&value](const std::size_t index) {
                if (!value.at(index).is_number_unsigned())
                    throw std::invalid_argument("Invalid lifecycle catalog integer");
                return value.at(index).get<std::uint64_t>();
            };
            if (number(2) > static_cast<std::uint8_t>(SaveSlotKind::System) ||
                number(13) > static_cast<std::uint8_t>(SaveSlotCloudState::Conflict) ||
                number(8) > std::numeric_limits<std::uint32_t>::max() || number(9) > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("Invalid lifecycle catalog enum/version");
            SaveSlotCatalogEntry entry{.publication = {.slot = Checked(SaveGameSlotId::Parse(text(0))),
                                                       .generation = Checked(SlotGenerationId::Parse(text(1))),
                                                       .kind = static_cast<SaveSlotKind>(number(2)),
                                                       .savedAtUnixMilliseconds = number(3),
                                                       .playTimeNanoseconds = number(4),
                                                       .baseScene = Checked(SaveBaseSceneId::Parse(text(5))),
                                                       .productCompatibility = Checked(
                                                           ProductSaveCompatibilityVersion::Create(static_cast<std::uint32_t>(number(8)))),
                                                       .saveSchema =
                                                           Checked(SaveSchemaVersion::Create(static_cast<std::uint32_t>(number(9)))),
                                                       .projectBuildId = text(10),
                                                       .canonicalState = {Checked(ParseSha256(text(11)))},
                                                       .archiveContent = {Checked(ParseSha256(text(12)))},
                                                       .cloudState = static_cast<SaveSlotCloudState>(number(13))},
                                       .display = {text(14), text(15)}};
            if (!value.at(6).is_null())
                entry.publication.checkpoint = Checked(SaveCheckpointId::Parse(text(6)));
            if (!value.at(7).is_null())
                entry.publication.thumbnail = Checked(SaveThumbnailId::Parse(text(7)));
            if (ValidateSaveSlotPublicationMetadata(entry.publication).HasError() ||
                ValidateSaveSlotDisplayMetadata(entry.display).HasError())
                throw std::invalid_argument("Invalid lifecycle catalog metadata");
            return entry;
        }

        /** @brief Binds durable evidence to the exact typed namespace without publishing private paths. */
        [[nodiscard]] std::string ScopeKey(const SaveNamespaceId &name) {
            const auto key = EncodeSaveNamespaceKey(name).Value();
            return FormatSha256(ComputeSha256(key.Bytes()));
        }

        /** @brief Produces one canonical shallow catalog; order and duplicate rejection are checked on decode. */
        [[nodiscard]] Json CatalogJson(const Catalog &catalog, const SaveSlotLifecyclePolicy &policy) {
            Json records = Json::array();
            Json retired = Json::array();
            for (const auto &record : catalog.records)
                records.push_back(Json::array({EncodeEntry(record.entry), record.deleted}));
            for (const auto &record : catalog.retired)
                retired.push_back(Json::array({EncodeEntry(record.entry), record.recycle}));
            return Json::array({1U, ScopeKey(policy.destination.name), catalog.revision, std::move(records), std::move(retired)});
        }

        /** @brief Enforces finite counts and unique selected/retired generation ownership. */
        [[nodiscard]] bool ValidCatalog(const Catalog &catalog, const SaveSlotLifecyclePolicy &policy) {
            if (catalog.revision == 0 || catalog.records.size() > policy.maximumSlots || catalog.retired.size() > policy.maximumSlots)
                return false;
            std::vector<SlotGenerationId> generations;
            generations.reserve(catalog.records.size() + catalog.retired.size());
            SaveGameSlotId previous;
            for (const auto &record : catalog.records) {
                if ((previous.IsValid() && previous >= record.entry.publication.slot) ||
                    ValidateSaveSlotPublicationMetadata(record.entry.publication).HasError() ||
                    ValidateSaveSlotDisplayMetadata(record.entry.display).HasError())
                    return false;
                previous = record.entry.publication.slot;
                generations.push_back(record.entry.publication.generation);
            }
            for (const auto &retired : catalog.retired) {
                if (ValidateSaveSlotPublicationMetadata(retired.entry.publication).HasError() ||
                    ValidateSaveSlotDisplayMetadata(retired.entry.display).HasError())
                    return false;
                generations.push_back(retired.entry.publication.generation);
            }
            std::ranges::sort(generations);
            return std::ranges::adjacent_find(generations) == generations.end();
        }

        /** @brief Prepends a digest so truncation or damaged evidence cannot turn into an empty namespace. */
        [[nodiscard]] std::vector<std::byte> Seal(const std::span<const std::byte> body) {
            const auto digest = ComputeSha256(body);
            std::vector<std::byte> bytes;
            bytes.reserve(kDigestBytes + body.size());
            for (const auto byte : digest.bytes)
                bytes.push_back(static_cast<std::byte>(byte));
            bytes.insert(bytes.end(), body.begin(), body.end());
            return bytes;
        }

        /** @brief Verifies bounded complete evidence before any metadata parsing. */
        [[nodiscard]] bool Sealed(const std::span<const std::byte> bytes) {
            if (bytes.size() <= kDigestBytes)
                return false;
            const auto digest = ComputeSha256(bytes.subspan(kDigestBytes));
            for (std::size_t index = 0; index < kDigestBytes; ++index) {
                if (bytes[index] != static_cast<std::byte>(digest.bytes[index]))
                    return false;
            }
            return true;
        }
    }  // namespace

    /** @copydoc EncodeCatalog */
    Result<std::vector<std::byte>> EncodeCatalog(const Catalog &catalog, const SaveSlotLifecyclePolicy &policy) {
        if (!ValidCatalog(catalog, policy))
            return Result<std::vector<std::byte>>::Failure(MakeError(SaveErrors::SlotCommitInvalid));
        const auto text = CatalogJson(catalog, policy).dump();
        if (text.size() > policy.maximumCatalogBytes - kDigestBytes)
            return Result<std::vector<std::byte>>::Failure(MakeError(SaveErrors::StorageQuotaExceeded));
        return Result<std::vector<std::byte>>::Success(Seal(std::as_bytes(std::span{text.data(), text.size()})));
    }

    /** @copydoc DecodeCatalog */
    Result<Catalog> DecodeCatalog(const std::span<const std::byte> bytes, const SaveSlotLifecyclePolicy &policy) {
        if (bytes.size() > policy.maximumCatalogBytes || !Sealed(bytes))
            return Result<Catalog>::Failure(MakeError(SaveErrors::SlotCommitInvalid));
        try {
            const auto body = bytes.subspan(kDigestBytes);
            const std::string text(reinterpret_cast<const char *>(body.data()), body.size());
            auto value = Json::parse(text, [](const int depth, Json::parse_event_t, Json &) {
                if (depth > 6)
                    throw std::invalid_argument("Lifecycle catalog nesting bound exceeded");
                return true;
            });
            if (!value.is_array() || value.size() != 5 || !value.at(0).is_number_unsigned() || value.at(0) != 1U ||
                !value.at(1).is_string() || value.at(1) != ScopeKey(policy.destination.name) || !value.at(2).is_number_unsigned() ||
                !value.at(3).is_array() || !value.at(4).is_array() || value.at(3).size() > policy.maximumSlots ||
                value.at(4).size() > policy.maximumSlots || value.dump() != text)
                return Result<Catalog>::Failure(MakeError(SaveErrors::SlotCommitInvalid));
            Catalog catalog{.revision = value.at(2).get<std::uint64_t>()};
            const auto decode = [](const Json &record) {
                if (!record.is_array() || record.size() != 2 || !record.at(1).is_boolean())
                    throw std::invalid_argument("Invalid lifecycle record");
                return Record{DecodeEntry(record.at(0)), record.at(1).get<bool>()};
            };
            for (const auto &item : value.at(3))
                catalog.records.push_back(decode(item));
            for (const auto &item : value.at(4)) {
                auto record = decode(item);
                catalog.retired.push_back({std::move(record.entry), record.deleted});
            }
            if (!ValidCatalog(catalog, policy))
                return Result<Catalog>::Failure(MakeError(SaveErrors::SlotCommitInvalid));
            return Result<Catalog>::Success(std::move(catalog));
        } catch (const Json::exception &) {
            return Result<Catalog>::Failure(MakeError(SaveErrors::SlotCommitInvalid));
        } catch (const std::invalid_argument &) {
            return Result<Catalog>::Failure(MakeError(SaveErrors::SlotCommitInvalid));
        }
    }

    /** @copydoc EncodeJournal */
    std::vector<std::byte> EncodeJournal(const Journal journal, const SaveNamespaceId &name) {
        const auto key = EncodeSaveNamespaceKey(name).Value();
        std::vector<std::byte> body(key.Bytes().begin(), key.Bytes().end());
        for (const auto byte : journal.generation.Bytes())
            body.push_back(static_cast<std::byte>(byte));
        for (const auto byte : journal.bytesHash.bytes)
            body.push_back(static_cast<std::byte>(byte));
        return Seal(body);
    }

    /** @copydoc DecodeJournal */
    Result<Journal> DecodeJournal(const std::span<const std::byte> bytes, const SaveNamespaceId &name) {
        const auto key = EncodeSaveNamespaceKey(name).Value();
        if (bytes.size() != kDigestBytes + CanonicalSaveNamespaceKeyBytes + 16 + kDigestBytes || !Sealed(bytes) ||
            !std::ranges::equal(bytes.subspan(kDigestBytes, CanonicalSaveNamespaceKeyBytes), key.Bytes()))
            return Result<Journal>::Failure(MakeError(SaveErrors::SlotCommitInvalid));
        SaveIdentityDetail::Bytes generation{};
        for (std::size_t index = 0; index < generation.size(); ++index)
            generation[index] = std::to_integer<std::uint8_t>(bytes[kDigestBytes + CanonicalSaveNamespaceKeyBytes + index]);
        auto identity = SlotGenerationId::FromBytes(generation);
        if (identity.HasError())
            return Result<Journal>::Failure(identity.ErrorValue());
        Sha256Digest hash;
        for (std::size_t index = 0; index < hash.bytes.size(); ++index)
            hash.bytes[index] = std::to_integer<std::uint8_t>(bytes[kDigestBytes + CanonicalSaveNamespaceKeyBytes + 16 + index]);
        return Result<Journal>::Success({identity.Value(), hash});
    }
}  // namespace Horo::Runtime::SaveSlotLifecycleDetail
