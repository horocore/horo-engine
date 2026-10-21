#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiStyleAsset.h"
#include "JsonUtils.h"
#include "UiDocumentSerializationInternal.h"
#include "UiStyleInternal.h"
#include "UiStyleValueCodec.h"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Horo::Runtime::Ui {
    namespace {
        using Json = nlohmann::json;
        using Out = nlohmann::ordered_json;

        /** @brief Reject missing and unknown fields before allocating collections. */
        void Fields(const Json &value, std::initializer_list<std::string_view> keys) {
            if (!Foundation::HasAllowedFields(value, keys) || value.size() != keys.size())
                throw std::invalid_argument("style fields");
        }

        /** @brief Decode a closed unsigned number without narrowing. */
        template <typename T> T Unsigned(const Json &value) {
            if (!value.is_number_integer() || (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() < 0))
                throw std::invalid_argument("style unsigned number");
            const auto number = value.get<std::uint64_t>();
            if (number > std::numeric_limits<T>::max())
                throw std::invalid_argument("style unsigned range");
            return static_cast<T>(number);
        }

        /** @brief Read an actual JSON boolean. */
        bool Boolean(const Json &value) {
            if (!value.is_boolean())
                throw std::invalid_argument("style boolean");
            return value.get<bool>();
        }

        /** @brief Convert an exact canonical AssetId to the domain's full-width identity. */
        template <typename T> T Identity(const Json &value) {
            auto decoded = SerializationInternal::DecodeUiId<T>(value);
            if (decoded.HasError())
                throw std::invalid_argument("style identity");
            return decoded.Value();
        }

        /** @brief The root asset field uses Assets canonical UUID text; only exact bytes cross into the style namespace. */
        RuntimeStyleAssetId RootIdentity(const Json &value) {
            if (!value.is_string())
                throw std::invalid_argument("style root identity");
            const auto asset = Assets::AssetId::Parse(value.get_ref<const std::string &>());
            if (asset.HasError() || !asset.Value().IsValid() || asset.Value().ToString() != value.get_ref<const std::string &>())
                throw std::invalid_argument("style root identity");
            return RuntimeStyleAssetId::Create(asset.Value().Bytes()).Value();
        }

        /** @brief Preserve all 128 identity bits in canonical text. */
        template <typename T> std::string Id(const T id) {
            return SerializationInternal::EncodeUiId(id.Bytes());
        }

        /** @brief Check finite collection bounds before extraction. */
        void List(const Json &value, const std::size_t maximum) {
            if (!value.is_array() || value.size() > maximum)
                throw std::invalid_argument("style collection capacity");
        }

        /** @brief Encode canonical assignment semantics, including sealing. */
        Out Assignment(const UiStyleAssignment &value) {
            return Out{{"property", Id(value.property)}, {"source", StyleCodec::EncodeSource(value.value)}, {"sealed", value.sealed}};
        }

        /** @brief Decode an assignment using the single shared literal/token codec. */
        UiStyleAssignment Assignment(const Json &value) {
            Fields(value, {"property", "source", "sealed"});
            auto source = StyleCodec::DecodeSource(value.at("source"));
            if (!source.IsValid())
                throw std::invalid_argument("style source");
            return {Identity<UiStylePropertyId>(value.at("property")), std::move(source), Boolean(value.at("sealed"))};
        }

        /** @brief Encode bounded assignment order exactly. */
        Out Assignments(std::span<const UiStyleAssignment> values) {
            Out out = Out::array();
            for (std::size_t index = 0; index < values.size(); ++index) {
                const auto prefix = values.first(index);
                if (std::ranges::find(prefix, values[index].property, &UiStyleAssignment::property) != prefix.end())
                    throw std::invalid_argument("duplicate assignment");
                out.push_back(Assignment(values[index]));
            }
            return out;
        }

        /** @brief Decode bounded assignment order; semantic duplicates fail in registry admission. */
        std::vector<UiStyleAssignment> Assignments(const Json &values) {
            List(values, MaximumUiStyleProperties);
            std::vector<UiStyleAssignment> out;
            for (const auto &value : values) {
                auto assignment = Assignment(value);
                if (std::ranges::find(out, assignment.property, &UiStyleAssignment::property) != out.end())
                    throw std::invalid_argument("duplicate assignment");
                out.push_back(std::move(assignment));
            }
            return out;
        }

        /** @brief Encode state blocks without live interaction state. */
        Out States(std::span<const UiStyleStateOverride> values) {
            Out out = Out::array();
            for (const auto &value : values)
                out.push_back(Out{{"required", value.required.bits},
                                  {"forbidden", value.forbidden.bits},
                                  {"layer", static_cast<std::uint8_t>(value.layer)},
                                  {"assignments", Assignments(value.assignments)}});
            return out;
        }

        /** @brief Decode bounded closed state selectors; the registry owns selector semantics. */
        std::vector<UiStyleStateOverride> States(const Json &values) {
            List(values, MaximumUiStyleStateBlocks);
            std::vector<UiStyleStateOverride> out;
            for (const auto &value : values) {
                Fields(value, {"required", "forbidden", "layer", "assignments"});
                if ((Unsigned<std::uint16_t>(value.at("required")) & ~StyleInternal::KnownVisualStateBits) != 0 ||
                    (Unsigned<std::uint16_t>(value.at("forbidden")) & ~StyleInternal::KnownVisualStateBits) != 0 ||
                    !StyleInternal::IsKnownLayer(static_cast<UiStateLayer>(Unsigned<std::uint8_t>(value.at("layer")))))
                    throw std::invalid_argument("style selector");
                out.push_back({UiVisualStateMask{Unsigned<std::uint16_t>(value.at("required"))},
                               UiVisualStateMask{Unsigned<std::uint16_t>(value.at("forbidden"))},
                               static_cast<UiStateLayer>(Unsigned<std::uint8_t>(value.at("layer"))), Assignments(value.at("assignments"))});
            }
            return out;
        }

        /** @brief Encode an optional qualified class base. */
        Out ClassBase(const std::optional<UiStyleClassReference> &base) {
            return base ? Out::array({Id(base->asset), Id(base->id)}) : Out(nullptr);
        }

        /** @brief Decode one optional qualified class base. */
        std::optional<UiStyleClassReference> ClassBase(const Json &value) {
            if (value.is_null())
                return std::nullopt;
            if (!value.is_array() || value.size() != 2)
                throw std::invalid_argument("class base");
            return UiStyleClassReference{Identity<RuntimeStyleAssetId>(value[0]), Identity<UiStyleClassId>(value[1])};
        }

        /** @brief Encode one inert namespace; no schema or service registration data exists in it. */
        Out Style(const UiStyleAssetDefinition &style) {
            Out tokens = Out::array(), classes = Out::array();
            for (const auto &token : style.tokens)
                tokens.push_back(Out{{"id", Id(token.id)},
                                     {"category", static_cast<std::uint8_t>(token.category)},
                                     {"source", StyleCodec::EncodeSource(token.value)},
                                     {"sealed", token.sealed}});
            for (const auto &value : style.classes)
                classes.push_back(Out{{"id", Id(value.id)},
                                      {"base", ClassBase(value.base)},
                                      {"sealed", value.sealed},
                                      {"assignments", Assignments(value.assignments)},
                                      {"states", States(value.states)}});
            return Out{{"base", style.base ? Out(Id(*style.base)) : Out(nullptr)},
                       {"tokens", std::move(tokens)},
                       {"assignments", Assignments(style.assignments)},
                       {"states", States(style.states)},
                       {"classes", std::move(classes)}};
        }

        /** @brief Decode only bounded inert namespace declarations. */
        UiStyleAssetDefinition Style(const Json &value, RuntimeStyleAssetId id) {
            Fields(value, {"base", "tokens", "assignments", "states", "classes"});
            UiStyleAssetDefinition style{.id = id};
            if (!value.at("base").is_null())
                style.base = Identity<RuntimeStyleAssetId>(value.at("base"));
            List(value.at("tokens"), MaximumUiStyleTokens);
            List(value.at("classes"), MaximumUiStyleClasses);
            for (const auto &token : value.at("tokens")) {
                Fields(token, {"id", "category", "source", "sealed"});
                if (!StyleInternal::IsKnownCategory(static_cast<UiStyleValueCategory>(Unsigned<std::uint8_t>(token.at("category")))) ||
                    !StyleCodec::DecodeSource(token.at("source")).IsValid())
                    throw std::invalid_argument("style token");
                const auto id = Identity<UiStyleTokenId>(token.at("id"));
                if (std::ranges::find(style.tokens, id, &UiStyleTokenDefinition::id) != style.tokens.end())
                    throw std::invalid_argument("duplicate token");
                style.tokens.push_back({id, static_cast<UiStyleValueCategory>(Unsigned<std::uint8_t>(token.at("category"))),
                                        StyleCodec::DecodeSource(token.at("source")), Boolean(token.at("sealed"))});
            }
            style.assignments = Assignments(value.at("assignments"));
            style.states = States(value.at("states"));
            for (const auto &entry : value.at("classes")) {
                Fields(entry, {"id", "base", "sealed", "assignments", "states"});
                const auto id = Identity<UiStyleClassId>(entry.at("id"));
                if (std::ranges::find(style.classes, id, &UiStyleClassDefinition::id) != style.classes.end())
                    throw std::invalid_argument("duplicate class");
                style.classes.push_back({id, ClassBase(entry.at("base")), Boolean(entry.at("sealed")), Assignments(entry.at("assignments")),
                                         States(entry.at("states"))});
            }
            return style;
        }

        /** @brief Encode selected-root resource default values, never schema metadata. */
        Out Defaults(const UiCoreStyleDefaults &value) {
            return Out{{"typography", value.typography ? StyleCodec::EncodeLiteral(*value.typography) : Out(nullptr)},
                       {"image", value.image ? StyleCodec::EncodeLiteral(*value.image) : Out(nullptr)}};
        }

        /** @brief Reject a default with a different literal category. */
        UiCoreStyleDefaults Defaults(const Json &value) {
            Fields(value, {"typography", "image"});
            UiCoreStyleDefaults result;
            if (!value.at("typography").is_null())
                result.typography = std::get<UiStyleTypography>(StyleCodec::DecodeLiteral(value.at("typography")));
            if (!value.at("image").is_null())
                result.image = std::get<UiStyleImage>(StyleCodec::DecodeLiteral(value.at("image")));
            if ((result.typography && !result.typography->IsValid()) || (result.image && !result.image->IsValid()))
                throw std::invalid_argument("style resource defaults");
            return result;
        }

        /** @brief Bound every nested collection before any source encoding allocation. */
        bool Bounded(const UiStyleAssetDocument &document) {
            const auto &style = document.style;
            if (document.dependencies.size() > MaximumUiStyleTokens || style.tokens.size() > MaximumUiStyleTokens ||
                style.assignments.size() > MaximumUiStyleProperties || style.classes.size() > MaximumUiStyleClasses)
                return false;
            std::size_t declarations = style.tokens.size() + style.assignments.size() + style.classes.size();
            std::size_t stateCount{};
            if (declarations > MaximumUiStyleAssetDeclarations)
                return false;
            const auto states = [&](const auto &values) {
                if (values.size() > MaximumUiStyleStateBlocks - stateCount)
                    return false;
                stateCount += values.size();
                for (const auto &state : values) {
                    if (state.assignments.size() > MaximumUiStyleProperties ||
                        state.assignments.size() > MaximumUiStyleAssetDeclarations - declarations)
                        return false;
                    declarations += state.assignments.size();
                }
                return true;
            };
            if (!states(style.states))
                return false;
            for (const auto &value : style.classes) {
                if (value.assignments.size() > MaximumUiStyleProperties ||
                    value.assignments.size() > MaximumUiStyleAssetDeclarations - declarations)
                    return false;
                declarations += value.assignments.size();
                if (!states(value.states))
                    return false;
            }
            return true;
        }

        /** @brief Encode source authority with a stable explicit version and dependency order. */
        Out Document(const UiStyleAssetDocument &document) {
            Out dependencies = Out::array();
            for (const auto &dependency : document.dependencies)
                dependencies.push_back(SerializationInternal::EncodeDependency(dependency));
            return Out{{"format", "horo.ui_style"},
                       {"schemaVersion", 1},
                       {"asset", Assets::AssetId::FromBytes(document.style.id.Bytes()).ToString()},
                       {"revision", document.revision.Value()},
                       {"defaults", Defaults(document.defaults)},
                       {"style", Style(document.style)},
                       {"dependencies", std::move(dependencies)}};
        }

        /** @brief Decode owned source fields; complete cross-namespace semantics are admitted as one candidate. */
        UiStyleAssetDocument DocumentBody(const Json &value) {
            const auto revision = UiStyleContentRevision::Create(Unsigned<std::uint64_t>(value.at("revision")));
            if (revision.HasError())
                throw std::invalid_argument("style revision");
            UiStyleAssetDocument result{revision.Value(),
                                        Style(value.at("style"), RootIdentity(value.at("asset"))),
                                        Defaults(value.at("defaults")),
                                        {}};
            List(value.at("dependencies"), MaximumUiStyleTokens);
            for (const auto &entry : value.at("dependencies")) {
                auto dependency = SerializationInternal::DecodeDependency(entry, {});
                if (dependency.HasError())
                    throw std::invalid_argument("style dependency");
                if (std::ranges::find(result.dependencies, dependency.Value().asset, &UiAssetDependency::asset) !=
                    result.dependencies.end())
                    throw std::invalid_argument("duplicate style dependency");
                result.dependencies.push_back(std::move(dependency).Value());
            }
            if (!Bounded(result))
                throw std::invalid_argument("style declaration capacity");
            return result;
        }

        /** @brief Source and cooked framing share only typed semantic decoding, never source-format admission. */
        UiStyleAssetDocument Document(const Json &value) {
            Fields(value, {"format", "schemaVersion", "asset", "revision", "defaults", "style", "dependencies"});
            if (value.at("format") != "horo.ui_style" || Unsigned<std::uint32_t>(value.at("schemaVersion")) != 1)
                throw std::invalid_argument("style version");
            return DocumentBody(value);
        }

        constexpr std::array<std::uint8_t, 12> CookedHeader{'H', 'U', 'I', 'S', 'T', 'Y', 'L', 'E', 1, 0, 0, 0};

    }  // namespace

    /** @copydoc SerializeUiStyleAsset */
    Result<std::string> SerializeUiStyleAsset(const UiStyleAssetDocument &document) {
        if (!Bounded(document))
            return Result<std::string>::Failure(MakeError(UiErrors::CapacityExceeded));
        try {
            auto canonical = document;
            std::ranges::sort(canonical.style.tokens, {}, &UiStyleTokenDefinition::id);
            std::ranges::sort(canonical.style.classes, {}, &UiStyleClassDefinition::id);
            std::ranges::sort(canonical.dependencies, {}, &UiAssetDependency::asset);
            const auto encoded = Document(canonical).dump();
            auto decoded = DeserializeUiStyleAsset(encoded);
            if (decoded.HasError())
                return Result<std::string>::Failure(decoded.ErrorValue());
            return Result<std::string>::Success(encoded);
        } catch (const std::exception &) {
            return Result<std::string>::Failure(MakeError(UiErrors::StyleInvalid));
        }
    }

    /** @copydoc DeserializeUiStyleAsset */
    Result<UiStyleAssetDocument> DeserializeUiStyleAsset(const std::string_view source) {
        auto parsed = SerializationInternal::ParseJson(source, {});
        if (parsed.HasError())
            return Result<UiStyleAssetDocument>::Failure(parsed.ErrorValue());
        try {
            return Result<UiStyleAssetDocument>::Success(Document(parsed.Value()));
        } catch (const std::exception &) {
            return Result<UiStyleAssetDocument>::Failure(MakeError(UiErrors::StyleInvalid));
        }
    }

    /** @copydoc CookUiStyleAsset */
    Result<std::vector<std::uint8_t>> CookUiStyleAsset(const UiStyleAssetDocument &document, const UiStyleSchema &schema) {
        auto source = SerializeUiStyleAsset(document);
        if (source.HasError())
            return Result<std::vector<std::uint8_t>>::Failure(source.ErrorValue());
        auto body = Out::parse(source.Value());
        body.erase("format");
        body.erase("schemaVersion");
        const auto encoded = body.dump();
        if (encoded.size() > MaximumUiStyleAssetBytes - CookedHeader.size() - schema.fingerprint.bytes.size())
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(UiErrors::CapacityExceeded));
        std::vector<std::uint8_t> output{CookedHeader.begin(), CookedHeader.end()};
        output.insert(output.end(), schema.fingerprint.bytes.begin(), schema.fingerprint.bytes.end());
        output.insert(output.end(), encoded.begin(), encoded.end());
        return Result<std::vector<std::uint8_t>>::Success(std::move(output));
    }

    /** @copydoc DecodeCookedUiStyleAsset */
    Result<UiStyleAssetDocument> DecodeCookedUiStyleAsset(const std::span<const std::uint8_t> bytes, const Assets::AssetId expected,
                                                          const UiStyleSchema &schema) {
        constexpr std::size_t prefix = CookedHeader.size() + Sha256Digest{}.bytes.size();
        if (bytes.size() <= prefix || bytes.size() > MaximumUiStyleAssetBytes ||
            !std::ranges::equal(bytes.first(CookedHeader.size()), CookedHeader))
            return Result<UiStyleAssetDocument>::Failure(MakeError(UiErrors::StyleInvalid));
        if (!std::ranges::equal(bytes.subspan(CookedHeader.size(), schema.fingerprint.bytes.size()), schema.fingerprint.bytes))
            return Result<UiStyleAssetDocument>::Failure(MakeError(UiErrors::StyleSourceStale));
        const std::string_view source{reinterpret_cast<const char *>(bytes.data() + prefix), bytes.size() - prefix};
        auto parsed = SerializationInternal::ParseJson(source, {});
        if (parsed.HasError())
            return Result<UiStyleAssetDocument>::Failure(parsed.ErrorValue());
        try {
            const auto &value = parsed.Value();
            Fields(value, {"asset", "revision", "defaults", "style", "dependencies"});
            auto document = DocumentBody(value);
            if (document.style.id.Bytes() != expected.Bytes())
                return Result<UiStyleAssetDocument>::Failure(MakeError(UiErrors::StyleIdentityConflict));
            const auto canonical = CookUiStyleAsset(document, schema);
            if (canonical.HasError())
                return Result<UiStyleAssetDocument>::Failure(canonical.ErrorValue());
            if (!std::ranges::equal(canonical.Value(), bytes))
                return Result<UiStyleAssetDocument>::Failure(MakeError(UiErrors::StyleInvalid));
            return Result<UiStyleAssetDocument>::Success(std::move(document));
        } catch (const std::exception &) {
            return Result<UiStyleAssetDocument>::Failure(MakeError(UiErrors::StyleInvalid));
        }
    }
}  // namespace Horo::Runtime::Ui
