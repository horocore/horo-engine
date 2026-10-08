#include "Horo/Runtime/Scene/SaveContentRequirements.h"

#include "Horo/Runtime/Save/SaveErrors.h"
#include "SaveContentRequirementsInternal.h"

#include <algorithm>
#include <array>
#include <new>
#include <tuple>

namespace Horo::Runtime {
    namespace {
        using ContentIdentity = std::variant<Assets::AssetId, Assets::AssetChunkId, SaveParticipantId>;
        using RequirementKey = std::tuple<SaveParticipantId, std::size_t, ContentIdentity>;

        /** @brief Excludes mutable compatibility details from duplicate schema identities. */
        RequirementKey Key(const SaveContentRequirement &requirement) {
            const auto identity = std::visit([]<typename T>(const T &content) -> ContentIdentity {
                if constexpr (std::is_same_v<T, SaveAssetContentRequirement>)
                    return content.asset;
                else if constexpr (std::is_same_v<T, SaveChunkContentRequirement>)
                    return content.chunk;
                else
                    return content.module;
            }, requirement.content);
            return {requirement.owner, requirement.content.index(), identity};
        }

        /** @brief Keeps this built-in record's complete decoded work bounded independently of host defaults. */
        CanonicalCodecLimits Limits() {
            return {.maximumBytes = 1024U * 1024U,
                    .maximumDecodedBytes = 2U * 1024U * 1024U,
                    .maximumStringBytes = 128,
                    .maximumCollectionElements = MaximumSaveContentRequirements,
                    .maximumFields = 16,
                    .maximumNestingDepth = 4,
                    .maximumReadWorkBytes = 4U * 1024U * 1024U};
        }

        /** @brief Rejects malformed schema declarations before any installed-content lookup. */
        bool ValidContent(const SaveContentRequirement &requirement) {
            if (!requirement.owner.IsValid() || requirement.necessity > SaveContentNecessity::Optional ||
                requirement.content.valueless_by_exception())
                return false;
            return std::visit([]<typename T>(const T &content) {
                if constexpr (std::is_same_v<T, SaveAssetContentRequirement>) {
                    return content.asset.IsValid() && !content.type.Value().empty() &&
                           std::ranges::any_of(content.envelopeDigest.bytes, [](const std::uint8_t byte) {
                        return byte != 0;
                    });
                } else if constexpr (std::is_same_v<T, SaveChunkContentRequirement>) {
                    return !content.chunk.Value().empty() && content.kind <= Assets::AssetChunkKind::DedicatedServer;
                } else
                    return content.module.IsValid() && content.version != 0;
            }, requirement.content);
        }

        /** @brief Encodes one already validated declaration with sticky-failure canonical writes. */
        Result<CanonicalEncodedValue> EncodeRequirement(const SaveContentRequirement &requirement) {
            CanonicalValueWriter writer{Limits()};
            (void)writer.WriteUtf8(requirement.owner.Value());
            (void)writer.WriteUInt8(static_cast<std::uint8_t>(requirement.necessity));
            (void)writer.WriteUInt8(static_cast<std::uint8_t>(requirement.content.index()));
            std::visit([&writer]<typename T>(const T &content) {
                if constexpr (std::is_same_v<T, SaveAssetContentRequirement>) {
                    (void)writer.WriteBytes(std::as_bytes(std::span{content.asset.Bytes()}));
                    (void)writer.WriteUtf8(content.type.Value());
                    (void)writer.WriteBytes(std::as_bytes(std::span{content.envelopeDigest.bytes}));
                } else if constexpr (std::is_same_v<T, SaveChunkContentRequirement>) {
                    (void)writer.WriteUtf8(content.chunk.Value());
                    (void)writer.WriteUInt8(static_cast<std::uint8_t>(content.kind));
                } else {
                    (void)writer.WriteUtf8(content.module.Value());
                    (void)writer.WriteUInt32(content.version);
                }
            }, requirement.content);
            return std::move(writer).Finalize();
        }

        /** @brief Decodes asset identity and envelope evidence without invoking an asset provider. */
        Result<SaveAssetContentRequirement> DecodeAsset(CanonicalValueReader &reader) {
            auto id = reader.ReadBytes(16);
            if (id.HasError())
                return Result<SaveAssetContentRequirement>::Failure(id.ErrorValue());
            auto type = reader.ReadUtf8();
            if (type.HasError())
                return Result<SaveAssetContentRequirement>::Failure(type.ErrorValue());
            auto digest = reader.ReadBytes(32);
            if (digest.HasError())
                return Result<SaveAssetContentRequirement>::Failure(digest.ErrorValue());
            auto parsedType = Assets::AssetTypeId::Parse(type.Value());
            if (parsedType.HasError())
                return Result<SaveAssetContentRequirement>::Failure(parsedType.ErrorValue());
            if (id.Value().size() != 16 || digest.Value().size() != 32)
                return Result<SaveAssetContentRequirement>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
            std::array<std::uint8_t, 16> identity{};
            Sha256Digest hash;
            std::ranges::transform(id.Value(), identity.begin(), [](const std::byte byte) {
                return std::to_integer<std::uint8_t>(byte);
            });
            std::ranges::transform(digest.Value(), hash.bytes.begin(), [](const std::byte byte) {
                return std::to_integer<std::uint8_t>(byte);
            });
            return Result<SaveAssetContentRequirement>::Success(
                {Assets::AssetId::FromBytes(identity), std::move(parsedType).Value(), hash});
        }

        /** @brief Decodes the closed schema variant; unknown kinds never become optional success. */
        Result<decltype(SaveContentRequirement::content)> DecodeContent(CanonicalValueReader &reader, const std::uint8_t kind) {
            using Content = decltype(SaveContentRequirement::content);
            if (kind == 0) {
                auto asset = DecodeAsset(reader);
                return asset.HasError() ? Result<Content>::Failure(asset.ErrorValue()) : Result<Content>::Success(std::move(asset).Value());
            }
            auto identity = reader.ReadUtf8();
            if (identity.HasError())
                return Result<Content>::Failure(identity.ErrorValue());
            if (kind == 1) {
                auto chunk = Assets::AssetChunkId::Parse(identity.Value());
                if (chunk.HasError())
                    return Result<Content>::Failure(chunk.ErrorValue());
                auto role = reader.ReadUInt8();
                if (role.HasError())
                    return Result<Content>::Failure(role.ErrorValue());
                return Result<Content>::Success(
                    SaveChunkContentRequirement{std::move(chunk).Value(), static_cast<Assets::AssetChunkKind>(role.Value())});
            }
            if (kind == 2) {
                auto installation = SaveParticipantId::Parse(identity.Value());
                if (installation.HasError())
                    return Result<Content>::Failure(installation.ErrorValue());
                auto version = reader.ReadUInt32();
                if (version.HasError())
                    return Result<Content>::Failure(version.ErrorValue());
                return Result<Content>::Success(SaveModuleContentRequirement{std::move(installation).Value(), version.Value()});
            }
            return Result<Content>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
        }

        /** @brief Requires complete consumption of each bounded schema element. */
        Result<SaveContentRequirement> DecodeRequirement(const CanonicalDecodedValue &encoded) {
            auto opened = encoded.OpenReader();
            if (opened.HasError())
                return Result<SaveContentRequirement>::Failure(opened.ErrorValue());
            auto reader = std::move(opened).Value();
            auto owner = reader.ReadUtf8();
            if (owner.HasError())
                return Result<SaveContentRequirement>::Failure(owner.ErrorValue());
            auto parsedOwner = SaveParticipantId::Parse(owner.Value());
            if (parsedOwner.HasError())
                return Result<SaveContentRequirement>::Failure(parsedOwner.ErrorValue());
            auto necessity = reader.ReadUInt8();
            if (necessity.HasError())
                return Result<SaveContentRequirement>::Failure(necessity.ErrorValue());
            auto kind = reader.ReadUInt8();
            if (kind.HasError())
                return Result<SaveContentRequirement>::Failure(kind.ErrorValue());
            auto content = DecodeContent(reader, kind.Value());
            if (content.HasError())
                return Result<SaveContentRequirement>::Failure(content.ErrorValue());
            if (const auto complete = reader.RequireFinished(); complete.HasError())
                return Result<SaveContentRequirement>::Failure(complete.ErrorValue());
            return Result<SaveContentRequirement>::Success(
                {std::move(parsedOwner).Value(), static_cast<SaveContentNecessity>(necessity.Value()), std::move(content).Value()});
        }
    }  // namespace

    /** @copydoc SaveContentRequirementsParticipant */
    SaveParticipantId SaveContentRequirementsParticipant() {
        return SaveParticipantId::Parse("horo.content.requirements.v1").Value();
    }

    /** @copydoc SaveContentRequirementsRecord */
    SaveRecordId SaveContentRequirementsRecord() {
        return SaveRecordId::Parse("2d576609-77ae-4fb2-bf4a-9e7b63cbc001").Value();
    }

    namespace SaveContentDetail {
        Result<void> SortRequirements(std::vector<SaveContentRequirement> &requirements) {
            if (requirements.size() > MaximumSaveContentRequirements)
                return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
            try {
                std::ranges::sort(requirements, {}, Key);
                return ValidateSaveContentRequirements(requirements);
            } catch (const std::bad_alloc &) {
                return Result<void>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
            }
        }
    }  // namespace SaveContentDetail

    /** @copydoc ValidateSaveContentRequirements */
    Result<void> ValidateSaveContentRequirements(const std::span<const SaveContentRequirement> requirements) {
        try {
            if (requirements.size() > MaximumSaveContentRequirements)
                return Result<void>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
            for (std::size_t index = 0; index < requirements.size(); ++index)
                if (!ValidContent(requirements[index]) || (index != 0 && Key(requirements[index - 1]) >= Key(requirements[index])))
                    return Result<void>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        }
    }

    /** @copydoc EncodeSaveContentRequirements */
    Result<CanonicalEncodedValue> EncodeSaveContentRequirements(const std::span<const SaveContentRequirement> requirements) {
        try {
            if (const auto valid = ValidateSaveContentRequirements(requirements); valid.HasError())
                return Result<CanonicalEncodedValue>::Failure(valid.ErrorValue());
            std::vector<CanonicalEncodedValue> values;
            values.reserve(requirements.size());
            for (const auto &requirement : requirements) {
                auto encoded = EncodeRequirement(requirement);
                if (encoded.HasError())
                    return Result<CanonicalEncodedValue>::Failure(encoded.ErrorValue());
                values.push_back(std::move(encoded).Value());
            }
            CanonicalValueWriter writer{Limits()};
            (void)writer.WriteUInt32(1);
            (void)writer.WriteSequence(values);
            return std::move(writer).Finalize();
        } catch (const std::bad_alloc &) {
            return Result<CanonicalEncodedValue>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        }
    }

    /** @copydoc DecodeSaveContentRequirements */
    Result<std::vector<SaveContentRequirement>> DecodeSaveContentRequirements(const std::span<const std::byte> bytes) {
        using Requirements = std::vector<SaveContentRequirement>;
        try {
            auto opened = CanonicalValueReader::Create(bytes, Limits());
            if (opened.HasError())
                return Result<Requirements>::Failure(opened.ErrorValue());
            auto reader = std::move(opened).Value();
            auto version = reader.ReadUInt32();
            if (version.HasError())
                return Result<Requirements>::Failure(version.ErrorValue());
            if (version.Value() != 1)
                return Result<Requirements>::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
            auto sequence = reader.ReadSequence();
            if (sequence.HasError())
                return Result<Requirements>::Failure(sequence.ErrorValue());
            if (const auto complete = reader.RequireFinished(); complete.HasError())
                return Result<Requirements>::Failure(complete.ErrorValue());
            Requirements requirements;
            requirements.reserve(sequence.Value().size());
            for (const auto &element : sequence.Value()) {
                auto requirement = DecodeRequirement(element);
                if (requirement.HasError())
                    return Result<Requirements>::Failure(requirement.ErrorValue());
                requirements.push_back(std::move(requirement).Value());
            }
            if (const auto valid = ValidateSaveContentRequirements(requirements); valid.HasError())
                return Result<Requirements>::Failure(valid.ErrorValue());
            return Result<Requirements>::Success(std::move(requirements));
        } catch (const std::bad_alloc &) {
            return Result<Requirements>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        }
    }
}  // namespace Horo::Runtime
