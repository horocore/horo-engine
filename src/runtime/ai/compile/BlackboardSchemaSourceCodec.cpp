#include "DecisionBinary.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <new>

namespace Horo::AI {
    namespace {
        constexpr std::uint64_t SourceMagic = 0x315342424f524f48ULL;  // HOROBBS1
        using Detail::DecisionReader;
        using Detail::DecisionWriter;
        using SourceResult = Result<BlackboardAuthoredSchema>;

        /** @brief Presentation can reorder the complete key set without introducing duplicate or foreign IDs. */
        bool ValidRows(const BlackboardSchema &schema, const std::span<const BlackboardSchemaAuthoringRow> rows) {
            const auto keys = schema.Keys();
            if (rows.size() != keys.size())
                return false;
            std::array<bool, MaximumBlackboardKeys> visited{};
            for (const auto &row : rows) {
                if (row.displayName && !IsValidBlackboardDisplayName(*row.displayName))
                    return false;
                const auto key = std::ranges::lower_bound(keys, row.key, {}, &BlackboardKeyDescriptor::key);
                if (key == keys.end() || key->key != row.key)
                    return false;
                const auto index = static_cast<std::size_t>(key - keys.begin());
                if (visited[index])
                    return false;
                visited[index] = true;
            }
            return true;
        }

        /** @brief Opening a semantic-only asset preserves identity without writing fabricated labels. */
        SourceResult Legacy(const std::span<const std::uint8_t> bytes) {
            auto schema = BlackboardSchemaCodec::Decode(bytes);
            if (schema.HasError())
                return SourceResult::Failure(schema.ErrorValue());
            std::vector<BlackboardSchemaAuthoringRow> rows;
            rows.reserve(schema.Value().Keys().size());
            for (const auto &key : schema.Value().Keys())
                rows.push_back({.key = key.key});
            return SourceResult::Success({std::move(schema).Value(), std::move(rows)});
        }
    }  // namespace

    /** @copydoc IsValidBlackboardDisplayName */
    bool IsValidBlackboardDisplayName(std::string_view displayName) noexcept {
        return !displayName.empty() && displayName.size() <= BlackboardSchemaAuthoringRow::MaximumLabelBytes &&
               IsValidUtf8ScalarSequence(displayName) && std::ranges::none_of(displayName, [](const unsigned char byte) {
            return byte < 0x20 || byte == 0x7f;
        });
    }

    /** @copydoc BlackboardSchemaSourceCodec::Encode */
    Result<std::vector<std::uint8_t>> BlackboardSchemaSourceCodec::Encode(const BlackboardSchema &schema,
                                                                          const std::span<const BlackboardSchemaAuthoringRow> rows) {
        using Encoded = Result<std::vector<std::uint8_t>>;
        if (!ValidRows(schema, rows))
            return Encoded::Failure(MakeError(AIErrors::BlackboardSchemaInvalid));
        try {
            const auto semantic = BlackboardSchemaCodec::Encode(schema);
            if (semantic.HasError())
                return Encoded::Failure(semantic.ErrorValue());
            DecisionWriter writer(MaximumBytes);
            writer.Unsigned(SourceMagic);
            writer.Unsigned(Version, 4);
            if (!writer.Count(semantic.Value().size(), BlackboardSchemaCodec::MaximumBytes))
                return Encoded::Failure(MakeError(AIErrors::BlackboardLimitExceeded));
            writer.Bytes(semantic.Value());
            if (!writer.Count(rows.size(), MaximumBlackboardKeys))
                return Encoded::Failure(MakeError(AIErrors::BlackboardLimitExceeded));
            for (const auto &row : rows) {
                writer.IdentityValue(row.key);
                writer.Unsigned(row.displayName.has_value(), 1);
                if (row.displayName)
                    writer.String(*row.displayName, BlackboardSchemaAuthoringRow::MaximumLabelBytes);
            }
            if (!writer.Valid())
                return Encoded::Failure(MakeError(AIErrors::BlackboardLimitExceeded));
            return Encoded::Success(std::move(writer).Take());
        } catch (const std::bad_alloc &) {
            return Encoded::Failure(MakeError(AIErrors::BlackboardStorageUnavailable));
        }
    }

    /** @copydoc BlackboardSchemaSourceCodec::Decode */
    Result<BlackboardAuthoredSchema> BlackboardSchemaSourceCodec::Decode(const std::span<const std::uint8_t> bytes) {
        if (bytes.size() > MaximumBytes)
            return SourceResult::Failure(MakeError(AIErrors::BlackboardLimitExceeded));
        try {
            DecisionReader reader(bytes);
            if (reader.Unsigned() != SourceMagic)
                return Legacy(bytes);
            if (reader.Unsigned(4) != Version)
                return SourceResult::Failure(MakeError(AIErrors::BlackboardSchemaInvalid));
            auto schema = BlackboardSchemaCodec::Decode(reader.Bytes(reader.Count(BlackboardSchemaCodec::MaximumBytes)));
            if (schema.HasError())
                return SourceResult::Failure(schema.ErrorValue());
            const auto count = reader.Count(MaximumBlackboardKeys);
            std::vector<BlackboardSchemaAuthoringRow> rows;
            rows.reserve(count);
            for (std::size_t index = 0; index < count && reader.Valid(); ++index) {
                BlackboardSchemaAuthoringRow row{.key = reader.IdentityValue<BlackboardKeyId>()};
                if (reader.Boolean())
                    row.displayName = reader.String(BlackboardSchemaAuthoringRow::MaximumLabelBytes);
                rows.push_back(std::move(row));
            }
            if (!reader.Complete() || !ValidRows(schema.Value(), rows))
                return SourceResult::Failure(MakeError(AIErrors::BlackboardSchemaInvalid));
            return SourceResult::Success({std::move(schema).Value(), std::move(rows)});
        } catch (const std::bad_alloc &) {
            return SourceResult::Failure(MakeError(AIErrors::BlackboardStorageUnavailable));
        }
    }
}  // namespace Horo::AI
