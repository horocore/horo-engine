#include "NetworkReplicationInventoryCodec.h"

#include <algorithm>
#include <limits>
#include <new>
#include <nlohmann/json.hpp>

namespace Horo::Network::Detail {
    namespace {
        using Json = nlohmann::json;

        /** @brief Rejects unknown/missing keys before any indexed access. */
        bool Shape(const Json &value, const std::initializer_list<std::string_view> fields) {
            return value.is_object() && value.size() == fields.size() && std::ranges::all_of(fields, [&](const auto field) {
                return value.contains(field);
            });
        }

        /** @brief Reads a bounded exact unsigned integer, never a float or signed wrap. */
        bool Number(const Json &value, std::uint64_t &output, const std::uint64_t maximum) {
            if (value.is_number_unsigned())
                output = value.get<std::uint64_t>();
            else if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
                output = static_cast<std::uint64_t>(value.get<std::int64_t>());
            else
                return false;
            return output <= maximum;
        }

        /** @brief Preserves each strong identity's exact width and nonzero invariant. */
        template <typename Id> bool Identity(const Json &value, Id &output) {
            using Integer = decltype(output.Value());
            std::uint64_t number{};
            if (!Number(value, number, std::numeric_limits<Integer>::max()))
                return false;
            const auto parsed = Id::Create(static_cast<Integer>(number));
            if (parsed.HasError())
                return false;
            output = parsed.Value();
            return true;
        }

        /** @brief Converts a closed byte-sized policy enum; semantic sentinels are rejected by validation. */
        template <typename Enum> bool Policy(const Json &value, Enum &output) {
            std::uint64_t number{};
            if (!Number(value, number, static_cast<std::uint8_t>(Enum::Count) - 1U))
                return false;
            output = static_cast<Enum>(number);
            return true;
        }

        /** @brief Reads exact semantic version components. */
        bool Version(const Json &value, ReplicationSchemaVersion &output) {
            if (!Shape(value, {"major", "minor"}))
                return false;
            std::uint64_t major{};
            std::uint64_t minor{};
            if (!Number(value.at("major"), major, std::numeric_limits<std::uint16_t>::max()) ||
                !Number(value.at("minor"), minor, std::numeric_limits<std::uint16_t>::max()))
                return false;
            output = {static_cast<std::uint16_t>(major), static_cast<std::uint16_t>(minor)};
            return true;
        }

        /** @brief Canonical wire-version representation, unrelated to Horo project/release versions. */
        Json Version(const ReplicationSchemaVersion value) {
            return {{"major", value.major}, {"minor", value.minor}};
        }

        /** @brief Emits opaque canonical defaults as lowercase hex, never native object bytes. */
        std::string Hex(const std::span<const std::byte> bytes) {
            constexpr std::string_view digits = "0123456789abcdef";
            std::string text;
            text.reserve(bytes.size() * 2);
            for (const auto byte : bytes) {
                text.push_back(digits[std::to_integer<std::size_t>(byte >> 4U)]);
                text.push_back(digits[std::to_integer<std::size_t>(byte & std::byte{15})]);
            }
            return text;
        }

        /** @brief Decodes only bounded lowercase canonical hex; null is distinct from an empty default. */
        bool Default(const Json &value, std::optional<ReplicationFieldDefault> &output) {
            if (value.is_null())
                return true;
            if (!value.is_string())
                return false;
            const auto &text = value.get_ref<const std::string &>();
            if (text.size() % 2 != 0 || text.size() / 2 > NetworkReplicationInventory::DescriptorLimits.maximumDefaultBytesPerField)
                return false;
            constexpr std::string_view digits = "0123456789abcdef";
            ReplicationFieldDefault parsed;
            parsed.canonicalBytes.reserve(text.size() / 2);
            for (std::size_t index = 0; index < text.size(); index += 2) {
                const auto high = digits.find(text[index]);
                const auto low = digits.find(text[index + 1]);
                if (high == std::string_view::npos || low == std::string_view::npos)
                    return false;
                parsed.canonicalBytes.push_back(static_cast<std::byte>((high << 4U) | low));
            }
            output = std::move(parsed);
            return true;
        }

        /** @brief Reads wire identities and closed policies without resolving executable bindings. */
        bool FieldSemantics(const Json &value, ReplicationFieldDescriptor &output) {
            return Identity(value.at("id"), output.id) && Identity(value.at("valueType"), output.valueType) &&
                   Identity(value.at("codec"), output.codec) && Version(value.at("introducedVersion"), output.introducedVersion) &&
                   Policy(value.at("condition"), output.condition) && Policy(value.at("requirement"), output.requirement) &&
                   Policy(value.at("writePolicy"), output.writePolicy);
        }

        /** @brief Reads bounded payload/default metadata and an optional inert condition identity. */
        bool FieldPayload(const Json &value, ReplicationFieldDescriptor &output) {
            std::uint64_t bytes{};
            std::uint64_t elements{};
            if (!Number(value.at("limits").at("maximumEncodedBytes"), bytes, std::numeric_limits<std::uint32_t>::max()) ||
                !Number(value.at("limits").at("maximumElementCount"), elements, std::numeric_limits<std::uint32_t>::max()) ||
                !Default(value.at("canonicalDefault"), output.canonicalDefault))
                return false;
            output.limits = {static_cast<std::uint32_t>(bytes), static_cast<std::uint32_t>(elements)};
            if (!value.at("customCondition").is_null()) {
                ReplicationConditionId condition;
                if (!Identity(value.at("customCondition"), condition))
                    return false;
                output.customCondition = condition;
            }
            return true;
        }

        /** @brief Rejects unknown field keys before decoding complete inert semantics. */
        bool Field(const Json &value, ReplicationFieldDescriptor &output) {
            return Shape(value, {"id", "valueType", "codec", "introducedVersion", "condition", "requirement", "writePolicy", "limits",
                                 "canonicalDefault", "customCondition"}) &&
                   Shape(value.at("limits"), {"maximumEncodedBytes", "maximumElementCount"}) && FieldSemantics(value, output) &&
                   FieldPayload(value, output);
        }

        /** @brief Writes every stable field semantic with explicit nullable optional identities/defaults. */
        Json Field(const ReplicationFieldDescriptor &value) {
            return {{"id", value.id.Value()},
                    {"valueType", value.valueType.Value()},
                    {"codec", value.codec.Value()},
                    {"introducedVersion", Version(value.introducedVersion)},
                    {"condition", static_cast<std::uint8_t>(value.condition)},
                    {"requirement", static_cast<std::uint8_t>(value.requirement)},
                    {"writePolicy", static_cast<std::uint8_t>(value.writePolicy)},
                    {"limits",
                     {{"maximumEncodedBytes", value.limits.maximumEncodedBytes},
                      {"maximumElementCount", value.limits.maximumElementCount}}},
                    {"canonicalDefault", value.canonicalDefault ? Json(Hex(value.canonicalDefault->canonicalBytes)) : Json(nullptr)},
                    {"customCondition", value.customCondition ? Json(value.customCondition->Value()) : Json(nullptr)}};
        }

        /** @brief Checks the complete array/owner envelope before allocating schema storage. */
        bool SchemaEnvelope(const Json &value) {
            if (!Shape(value, {"id", "owner", "version", "compatibility", "fields", "tombstones"}) ||
                !Shape(value.at("compatibility"), {"minimum", "maximum"}) || !value.at("owner").is_string() ||
                !value.at("fields").is_array() || !value.at("tombstones").is_array())
                return false;
            const auto &owner = value.at("owner").get_ref<const std::string &>();
            const auto &fields = value.at("fields");
            if (const auto &tombstones = value.at("tombstones");
                owner.size() > NetworkReplicationInventory::DescriptorLimits.maximumOwnerIdentityBytes ||
                fields.size() + tombstones.size() > NetworkReplicationInventory::DescriptorLimits.maximumFieldsPerSchema)
                return false;
            return true;
        }

        /** @brief Reads exact schema identity and compatibility interval before copying fields. */
        bool SchemaIdentity(const Json &value, ReplicationSchemaDescriptor &output) {
            return Identity(value.at("id"), output.id) && Version(value.at("version"), output.version) &&
                   Version(value.at("compatibility").at("minimum"), output.compatibility.minimum) &&
                   Version(value.at("compatibility").at("maximum"), output.compatibility.maximum);
        }

        /** @brief Copies only bounded inert fields and retired identities from a verified envelope. */
        bool SchemaContents(const Json &value, ReplicationSchemaDescriptor &output) {
            output.owner.value = value.at("owner").get_ref<const std::string &>();
            for (const auto &field : value.at("fields")) {
                ReplicationFieldDescriptor parsed;
                if (!Field(field, parsed))
                    return false;
                output.fields.push_back(std::move(parsed));
            }
            for (const auto &tombstone : value.at("tombstones")) {
                FieldId id;
                if (!Identity(tombstone, id))
                    return false;
                output.tombstonedFields.push_back(id);
            }
            return true;
        }

        /** @brief Reads one complete schema without selecting a codec or native adapter. */
        bool Schema(const Json &value, ReplicationSchemaDescriptor &output) {
            return SchemaEnvelope(value) && SchemaIdentity(value, output) && SchemaContents(value, output);
        }

        /** @brief Writes an already canonical inert schema, including retired wire identities. */
        Json Schema(const ReplicationSchemaDescriptor &value) {
            Json fields = Json::array();
            for (const auto &field : value.fields)
                fields.push_back(Field(field));
            Json tombstones = Json::array();
            for (const auto id : value.tombstonedFields)
                tombstones.push_back(id.Value());
            return {{"id", value.id.Value()},
                    {"owner", value.owner.value},
                    {"version", Version(value.version)},
                    {"compatibility",
                     {{"minimum", Version(value.compatibility.minimum)}, {"maximum", Version(value.compatibility.maximum)}}},
                    {"fields", std::move(fields)},
                    {"tombstones", std::move(tombstones)}};
        }

        /** @brief Validates the aggregate default budget across every schema before copying the inventory. */
        Result<void> ValidateDefaultBudget(const NetworkReplicationInventory &input) {
            std::size_t defaultBytes{};
            for (const auto &declaration : input.declarations) {
                for (const auto &field : declaration.schema.fields) {
                    const auto bytes = field.canonicalDefault ? field.canonicalDefault->canonicalBytes.size() : 0;
                    if (bytes > NetworkReplicationInventory::DescriptorLimits.maximumTotalDefaultBytes - defaultBytes)
                        return Result<void>::Failure(MakeError(NetworkErrors::NetworkProjectSettingsCapacityExceeded));
                    defaultBytes += bytes;
                }
            }
            return Result<void>::Success();
        }

        /** @brief Validates completeness, declaration shape, and aggregate budgets before any owning copy. */
        Result<void> ValidateInventory(const NetworkReplicationInventory &input) {
            if (input.completeness >= NetworkReplicationInventoryCompleteness::Count ||
                (input.completeness == NetworkReplicationInventoryCompleteness::Unknown && !input.declarations.empty()))
                return Result<void>::Failure(MakeError(NetworkErrors::NetworkProjectSettingsInvalid));
            if (input.declarations.size() > NetworkReplicationInventory::DescriptorLimits.maximumSchemas)
                return Result<void>::Failure(MakeError(NetworkErrors::NetworkProjectSettingsCapacityExceeded));
            for (const auto &declaration : input.declarations) {
                if (declaration.requirement >= ReplicationDeclarationRequirement::Count)
                    return Result<void>::Failure(MakeError(NetworkErrors::ReplicationDescriptorInvalid));
                if (const auto valid =
                        ValidateReplicationSchemaDescriptor(declaration.schema, NetworkReplicationInventory::DescriptorLimits);
                    valid.HasError())
                    return Result<void>::Failure(valid.ErrorValue());
            }
            return ValidateDefaultBudget(input);
        }
    }  // namespace

    /** @copydoc CanonicalizeReplicationInventory */
    Result<NetworkReplicationInventory> CanonicalizeReplicationInventory(const NetworkReplicationInventory &input) {
        if (const auto valid = ValidateInventory(input); valid.HasError())
            return Result<NetworkReplicationInventory>::Failure(valid.ErrorValue());
        try {
            auto result = input;
            std::ranges::sort(result.declarations, {}, [](const auto &declaration) {
                return declaration.schema.id;
            });
            if (std::ranges::adjacent_find(result.declarations, {}, [](const auto &declaration) {
                return declaration.schema.id;
            }) != result.declarations.end())
                return Result<NetworkReplicationInventory>::Failure(MakeError(NetworkErrors::ReplicationDescriptorConflict));
            for (auto &declaration : result.declarations) {
                std::ranges::sort(declaration.schema.fields, {}, &ReplicationFieldDescriptor::id);
                std::ranges::sort(declaration.schema.tombstonedFields);
            }
            if (EncodeReplicationInventory(result).size() > NetworkReplicationInventory::MaximumDocumentBytes)
                return Result<NetworkReplicationInventory>::Failure(MakeError(NetworkErrors::NetworkProjectSettingsCapacityExceeded));
            return Result<NetworkReplicationInventory>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Result<NetworkReplicationInventory>::Failure(MakeError(NetworkErrors::NetworkProjectSettingsCapacityExceeded));
        }
    }

    /** @copydoc WriteReplicationInventory */
    Json WriteReplicationInventory(const NetworkReplicationInventory &inventory) {
        Json declarations = Json::array();
        for (const auto &declaration : inventory.declarations)
            declarations.push_back(
                {{"requirement", static_cast<std::uint8_t>(declaration.requirement)}, {"schema", Schema(declaration.schema)}});
        return {{"completeness", static_cast<std::uint8_t>(inventory.completeness)}, {"declarations", std::move(declarations)}};
    }

    /** @copydoc ReadReplicationInventory */
    bool ReadReplicationInventory(const Json &value, NetworkReplicationInventory &output) {
        if (!Shape(value, {"completeness", "declarations"}) || !Policy(value.at("completeness"), output.completeness) ||
            !value.at("declarations").is_array() ||
            value.at("declarations").size() > NetworkReplicationInventory::DescriptorLimits.maximumSchemas)
            return false;
        for (const auto &declaration : value.at("declarations")) {
            ReplicationDeclaration parsed;
            if (!Shape(declaration, {"requirement", "schema"}) || !Policy(declaration.at("requirement"), parsed.requirement) ||
                !Schema(declaration.at("schema"), parsed.schema))
                return false;
            output.declarations.push_back(std::move(parsed));
        }
        return true;
    }

    /** @copydoc EncodeReplicationInventory */
    std::string EncodeReplicationInventory(const NetworkReplicationInventory &inventory) {
        return WriteReplicationInventory(inventory).dump();
    }
}  // namespace Horo::Network::Detail
