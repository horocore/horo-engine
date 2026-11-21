#include "Horo/AI/EnvironmentQuerySourceCodec.h"

#include "DecisionBinary.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <array>
#include <bit>
#include <new>
#include <type_traits>

namespace Horo::AI {
    namespace {
        constexpr std::array<std::uint8_t, 8> Signature{'H', 'O', 'R', 'O', 'E', 'Q', 'S', '1'};
        constexpr std::array<std::uint8_t, 8> CookedSignature{'H', 'O', 'R', 'O', 'E', 'Q', 'C', '1'};
        constexpr std::uint32_t CookedVersion = 1;
        using Detail::DecisionReader;
        using Detail::DecisionWriter;
        using Limits = EnvironmentQuerySchemaLimits;

        /** @brief Accepts labels representable in the canonical bounded string transport. */
        bool ValidLabel(std::string_view label) {
            return label.find('\0') == std::string_view::npos && IsValidUtf8ScalarSequence(label);
        }

        /** @brief Writes the existing typed version interval in fixed-width order. */
        void WriteVersions(DecisionWriter &writer, QueryVersionRange versions) {
            writer.Unsigned(versions.minimum, 4);
            writer.Unsigned(versions.maximum, 4);
        }

        /** @brief Reads a complete inclusive version interval for later schema admission. */
        QueryVersionRange ReadVersions(DecisionReader &reader) {
            return {static_cast<std::uint32_t>(reader.Unsigned(4)), static_cast<std::uint32_t>(reader.Unsigned(4))};
        }

        /** @brief Writes bounded wire payload bytes without a native object representation. */
        void WriteBytes(DecisionWriter &writer, std::span<const std::byte> bytes, std::size_t ceiling) {
            if (writer.Count(bytes.size(), ceiling))
                writer.Bytes({reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
        }

        /** @brief Retains one complete bounded opaque payload. */
        std::vector<std::byte> ReadBytes(DecisionReader &reader, std::size_t ceiling) {
            const auto bytes = std::as_bytes(reader.Bytes(reader.Count(ceiling)));
            return bytes.empty() ? std::vector<std::byte>{} : std::vector<std::byte>{bytes.begin(), bytes.end()};
        }

        /** @brief Writes the exact declared variant arm and a fixed portable value representation. */
        void WriteProperty(DecisionWriter &writer, const QueryPropertyValue &property) {
            writer.IdentityValue(property.id);
            std::visit([&](const auto &value) {
                using Value = std::remove_cvref_t<decltype(value)>;
                using enum QueryPropertyKind;
                if constexpr (std::is_same_v<Value, bool>) {
                    writer.Enumeration(Boolean);
                    writer.Unsigned(value, 1);
                } else if constexpr (std::is_same_v<Value, std::int64_t>) {
                    writer.Enumeration(Signed64);
                    writer.Unsigned(std::bit_cast<std::uint64_t>(value));
                } else if constexpr (std::is_same_v<Value, std::uint64_t>) {
                    writer.Enumeration(Unsigned64);
                    writer.Unsigned(value);
                } else if constexpr (std::is_same_v<Value, double>) {
                    writer.Enumeration(Float64);
                    writer.Unsigned(std::bit_cast<std::uint64_t>(value));
                } else {
                    writer.Enumeration(CanonicalBytes);
                    WriteBytes(writer, value, Limits::CanonicalBytes);
                }
            }, property.value);
        }

        /** @brief Reads only the five existing property kinds and rejects an unknown wire arm. */
        QueryPropertyData ReadPropertyData(DecisionReader &reader) {
            using enum QueryPropertyKind;
            switch (reader.Enumeration(Count)) {
                case Boolean:
                    return reader.Boolean();
                case Signed64:
                    return std::bit_cast<std::int64_t>(reader.Unsigned());
                case Unsigned64:
                    return reader.Unsigned();
                case Float64:
                    return std::bit_cast<double>(reader.Unsigned());
                case CanonicalBytes:
                    return ReadBytes(reader, Limits::CanonicalBytes);
                case Count:
                    break;
            }
            reader.Reject();
            return false;
        }

        /** @brief Serializes typed stage identities, canonical property order and the unknown-stage envelope. */
        void WriteStage(DecisionWriter &writer, const QueryAssetStage &stage) {
            writer.IdentityValue(stage.id);
            writer.Enumeration(stage.kind);
            writer.Unsigned(stage.executionOrder, 4);
            writer.String(stage.displayName, Limits::DisplayNameBytes);
            writer.IdentityValue(stage.generator, true);
            writer.IdentityValue(stage.test, true);
            WriteVersions(writer, stage.descriptorVersion);
            auto properties = stage.properties;
            std::ranges::sort(properties, {}, &QueryPropertyValue::id);
            if (writer.Count(properties.size(), Limits::PropertiesPerStage))
                for (const auto &property : properties)
                    WriteProperty(writer, property);
            writer.Unsigned(stage.unknownTypeId);
            WriteBytes(writer, stage.opaquePayload, Limits::UnknownStageBytes);
        }

        /** @brief Reads a bounded stage while preserving unknown type and bytes for round-trip. */
        QueryAssetStage ReadStage(DecisionReader &reader) {
            QueryAssetStage stage{.id = reader.IdentityValue<QueryStageId>(),
                                  .kind = reader.Enumeration(QueryStageKind::Count),
                                  .executionOrder = static_cast<std::uint32_t>(reader.Unsigned(4)),
                                  .displayName = reader.String(Limits::DisplayNameBytes),
                                  .generator = reader.IdentityValue<QueryGeneratorId>(true),
                                  .test = reader.IdentityValue<QueryTestId>(true),
                                  .descriptorVersion = ReadVersions(reader)};
            if (!ValidLabel(stage.displayName))
                reader.Reject();
            const auto count = reader.Count(Limits::PropertiesPerStage);
            stage.properties.reserve(count);
            for (std::size_t index = 0; index < count && reader.Valid(); ++index)
                stage.properties.push_back({reader.IdentityValue<QueryPropertyId>(), ReadPropertyData(reader)});
            if (!std::ranges::is_sorted(stage.properties, {}, &QueryPropertyValue::id))
                reader.Reject();
            stage.unknownTypeId = reader.Unsigned();
            stage.opaquePayload = ReadBytes(reader, Limits::UnknownStageBytes);
            return stage;
        }

        /** @brief Emits stable owner and schema identity without copying a native object representation. */
        void WriteOrigin(DecisionWriter &writer, const QueryDescriptorOrigin &origin) {
            writer.Enumeration(origin.kind);
            writer.IdentityValue(origin.provider);
            writer.Unsigned(origin.version, 4);
        }

        /** @brief Records the bytes a compiled stage relies on at provider and context boundaries. */
        void WriteStageAdmission(DecisionWriter &writer, const QueryPlanStage &stage, const QuerySchemaRegistry &registry) {
            writer.IdentityValue(stage.id);
            const auto &origin =
                stage.kind == QueryStageKind::Generator ? registry.Find(stage.generator)->origin : registry.Find(stage.test)->origin;
            WriteOrigin(writer, origin);
            writer.IdentityValue(stage.selectedContext, true);
            if (!writer.Count(stage.contexts.size(), Limits::Contexts))
                return;
            for (const auto &requirement : stage.contexts) {
                const auto *context = registry.Find(requirement.id);
                writer.IdentityValue(requirement.id);
                WriteVersions(writer, requirement.version);
                WriteOrigin(writer, context->origin);
                writer.Unsigned(context->maximumPayloadBytes, 4);
            }
        }

        /** @brief Fingerprints only descriptors and resolved choices that the admitted plan actually consumes. */
        Sha256Digest AdmissionDigest(const EnvironmentQueryPlan &plan, const QuerySchemaRegistry &registry,
                                     std::span<const std::uint8_t> recipe) {
            using namespace std::literals;
            constexpr auto domain = "horo.eqs.admission.v1"sv;
            DecisionWriter writer{EnvironmentQueryCookedCodec::MaximumBytes};
            writer.String(domain);
            const auto *item = registry.Find(plan.ResultSchema().itemType);
            writer.IdentityValue(item->id);
            WriteOrigin(writer, item->origin);
            writer.Enumeration(item->kind);
            writer.Unsigned(item->maximumPayloadBytes, 4);
            if (writer.Count(plan.Stages().size(), Limits::Stages))
                for (const auto &stage : plan.Stages())
                    WriteStageAdmission(writer, stage, registry);
            Sha256Builder digest;
            static_cast<void>(digest.Update(std::as_bytes(writer.Data())));
            static_cast<void>(digest.Update(std::as_bytes(recipe)));
            return digest.Finalize();
        }

        /** @brief Removes presentation labels before encoding the deterministic reconstruction recipe. */
        Result<std::vector<std::uint8_t>> EncodeRecipe(const EnvironmentQueryAsset &asset) {
            QueryAssetSource recipe{.id = asset.Id(),
                                    .result = asset.ResultSchema(),
                                    .stages = {asset.Stages().begin(), asset.Stages().end()}};
            for (auto &stage : recipe.stages)
                stage.displayName.clear();
            const auto canonical = EnvironmentQueryAsset::Capture(recipe);
            if (canonical.HasError())
                return Result<std::vector<std::uint8_t>>::Failure(canonical.ErrorValue());
            return EnvironmentQuerySourceCodec::Encode(canonical.Value());
        }
    }  // namespace

    /** @copydoc EnvironmentQuerySourceCodec::Encode */
    Result<std::vector<std::uint8_t>> EnvironmentQuerySourceCodec::Encode(const EnvironmentQueryAsset &asset) {
        using Encoded = Result<std::vector<std::uint8_t>>;
        if (!ValidLabel(asset.DisplayName()) || std::ranges::any_of(asset.Stages(), [](const auto &stage) {
            return !ValidLabel(stage.displayName);
        }))
            return Encoded::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        try {
            DecisionWriter writer{MaximumBytes};
            writer.Bytes(Signature);
            writer.Unsigned(CurrentEnvironmentQuerySchemaVersion, 4);
            writer.IdentityValue(asset.Id());
            writer.String(asset.DisplayName(), Limits::DisplayNameBytes);
            const auto &result = asset.ResultSchema();
            writer.IdentityValue(result.id);
            writer.Unsigned(result.version, 4);
            writer.IdentityValue(result.itemType);
            WriteVersions(writer, result.itemVersion);
            writer.Unsigned(result.maximumResults, 4);
            std::vector<const QueryAssetStage *> stages;
            stages.reserve(asset.Stages().size());
            for (const auto &stage : asset.Stages())
                stages.push_back(&stage);
            std::ranges::sort(stages, {}, [](const QueryAssetStage *stage) {
                return stage->executionOrder;
            });
            if (writer.Count(stages.size(), Limits::Stages))
                for (const auto *stage : stages)
                    WriteStage(writer, *stage);
            if (!writer.Valid())
                return Encoded::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
            return Encoded::Success(std::move(writer).Take());
        } catch (const std::bad_alloc &) {
            return Encoded::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        }
    }

    /** @copydoc EnvironmentQuerySourceCodec::Decode */
    Result<EnvironmentQueryAsset> EnvironmentQuerySourceCodec::Decode(std::span<const std::uint8_t> bytes) {
        using Decoded = Result<EnvironmentQueryAsset>;
        if (bytes.size() > MaximumBytes)
            return Decoded::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        try {
            DecisionReader reader{bytes};
            if (!std::ranges::equal(reader.Bytes(Signature.size()), Signature))
                return Decoded::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            const auto version = static_cast<std::uint32_t>(reader.Unsigned(4));
            if (!reader.Valid())
                return Decoded::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            if (version != CurrentEnvironmentQuerySchemaVersion)
                return Decoded::Failure(MakeError(AIErrors::EnvironmentQueryVersionIncompatible));
            QueryAssetSource source{.id = reader.IdentityValue<QueryId>(),
                                    .schemaVersion = version,
                                    .displayName = reader.String(Limits::DisplayNameBytes),
                                    .result = {.id = reader.IdentityValue<QueryResultSchemaId>(),
                                               .version = static_cast<std::uint32_t>(reader.Unsigned(4)),
                                               .itemType = reader.IdentityValue<QueryItemTypeId>(),
                                               .itemVersion = ReadVersions(reader),
                                               .maximumResults = static_cast<std::uint32_t>(reader.Unsigned(4))}};
            const auto count = reader.Count(Limits::Stages);
            source.stages.reserve(count);
            for (std::size_t index = 0; index < count && reader.Valid(); ++index)
                source.stages.push_back(ReadStage(reader));
            if (!reader.Complete() || !ValidLabel(source.displayName) ||
                !std::ranges::is_sorted(source.stages, {}, &QueryAssetStage::executionOrder))
                return Decoded::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            return EnvironmentQueryAsset::Capture(source);
        } catch (const std::bad_alloc &) {
            return Decoded::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        }
    }

    /** @copydoc EnvironmentQueryCookedCodec::Encode */
    Result<std::vector<std::uint8_t>> EnvironmentQueryCookedCodec::Encode(const EnvironmentQueryAsset &asset,
                                                                          const QuerySchemaRegistry &registry) {
        using Encoded = Result<std::vector<std::uint8_t>>;
        try {
            const auto plan = EnvironmentQueryPlan::Compile(asset, registry);
            if (plan.HasError())
                return Encoded::Failure(plan.ErrorValue());
            const auto recipe = EncodeRecipe(asset);
            if (recipe.HasError())
                return Encoded::Failure(recipe.ErrorValue());
            const auto digest = AdmissionDigest(plan.Value(), registry, recipe.Value());
            DecisionWriter writer{MaximumBytes};
            writer.Bytes(CookedSignature);
            writer.Unsigned(CookedVersion, 4);
            writer.Bytes(digest.bytes);
            if (writer.Count(recipe.Value().size(), EnvironmentQuerySourceCodec::MaximumBytes))
                writer.Bytes(recipe.Value());
            if (!writer.Valid())
                return Encoded::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
            return Encoded::Success(std::move(writer).Take());
        } catch (const std::bad_alloc &) {
            return Encoded::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        }
    }

    /** @copydoc EnvironmentQueryCookedCodec::Decode */
    Result<EnvironmentQueryPlan> EnvironmentQueryCookedCodec::Decode(std::span<const std::uint8_t> bytes,
                                                                     const QuerySchemaRegistry &registry) {
        using Plan = Result<EnvironmentQueryPlan>;
        if (bytes.size() > MaximumBytes)
            return Plan::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        try {
            DecisionReader reader{bytes};
            if (!std::ranges::equal(reader.Bytes(CookedSignature.size()), CookedSignature))
                return Plan::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            const auto version = reader.Unsigned(4);
            if (reader.Valid() && version != CookedVersion)
                return Plan::Failure(MakeError(AIErrors::EnvironmentQueryVersionIncompatible));
            const auto expected = reader.Bytes(Sha256Digest{}.bytes.size());
            const auto recipe = reader.Bytes(reader.Count(EnvironmentQuerySourceCodec::MaximumBytes));
            if (!reader.Complete())
                return Plan::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            const auto asset = EnvironmentQuerySourceCodec::Decode(recipe);
            if (asset.HasError())
                return Plan::Failure(asset.ErrorValue());
            const auto canonical = EncodeRecipe(asset.Value());
            if (canonical.HasError())
                return Plan::Failure(canonical.ErrorValue());
            if (!std::ranges::equal(canonical.Value(), recipe))
                return Plan::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
            auto plan = EnvironmentQueryPlan::Compile(asset.Value(), registry);
            if (plan.HasError())
                return plan;
            const auto admitted = AdmissionDigest(plan.Value(), registry, recipe);
            if (!std::ranges::equal(expected, admitted.bytes))
                return Plan::Failure(MakeError(AIErrors::EnvironmentQueryVersionIncompatible));
            return plan;
        } catch (const std::bad_alloc &) {
            return Plan::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        }
    }
}  // namespace Horo::AI
