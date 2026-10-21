#include "UiPresentationProfileCodec.h"

#include "JsonUtils.h"
#include "UiDocumentSerializationInternal.h"
#include "UiElementPresentationCodec.h"

#include <limits>
#include <new>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Horo::Runtime::Ui::ProfileCodec {
    namespace {
        using Json = nlohmann::json;
        using Out = nlohmann::ordered_json;

        /** @brief Rejects noncanonical object keys. */
        void Fields(const Json &value, std::initializer_list<std::string_view> fields) {
            if (!Horo::Foundation::HasAllowedFields(value, fields))
                throw std::invalid_argument("profile fields");
        }

        /** @brief Reads exact nonnegative integral values without float truncation. */
        template <class T> T Unsigned(const Json &value) {
            if (!value.is_number_unsigned() && (!value.is_number_integer() || value.get<std::int64_t>() < 0))
                throw std::invalid_argument("profile integer");
            const auto raw = value.get<std::uint64_t>();
            if (raw > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
                throw std::invalid_argument("profile overflow");
            return static_cast<T>(raw);
        }

        /** @brief Reads a stable nonzero durable identity. */
        template <class T> T Id(const Json &value) {
            auto parsed = SerializationInternal::DecodeUiId<T>(value);
            if (parsed.HasError())
                throw std::invalid_argument("profile identity");
            return parsed.Value();
        }

        /** @brief Encodes one optional extent interval. */
        Out Extent(const UiPresentationExtentRange &value) {
            return Out::array({value.minimum ? Out(*value.minimum) : Out(nullptr), value.maximum ? Out(*value.maximum) : Out(nullptr)});
        }

        /** @brief Decodes exactly two optional positive fixed-point extent bounds. */
        UiPresentationExtentRange Extent(const Json &value) {
            if (!value.is_array() || value.size() != 2)
                throw std::invalid_argument("profile extent");
            UiPresentationExtentRange result;
            if (!value[0].is_null())
                result.minimum = Unsigned<std::int32_t>(value[0]);
            if (!value[1].is_null())
                result.maximum = Unsigned<std::int32_t>(value[1]);
            return result;
        }

        /** @brief Encodes an optional reduced aspect bound. */
        Out Ratio(const std::optional<UiPresentationAspectRatio> &ratio) {
            return ratio ? Out::array({ratio->numerator, ratio->denominator}) : Out(nullptr);
        }

        /** @brief Decodes a reduced aspect pair without floating conversion. */
        std::optional<UiPresentationAspectRatio> Ratio(const Json &value) {
            if (value.is_null())
                return std::nullopt;
            if (!value.is_array() || value.size() != 2)
                throw std::invalid_argument("profile aspect");
            return UiPresentationAspectRatio{Unsigned<std::uint32_t>(value[0]), Unsigned<std::uint32_t>(value[1])};
        }

        /** @brief Decodes one complete row after the outer count/byte checks, preserving typed layout errors. */
        Result<UiPresentationProfile> DecodeRow(const Json &row) {
            UiPresentationProfile profile{Id<UiPresentationProfileId>(row.at("id")), Unsigned<std::uint32_t>(row.at("priority"))};
            const auto &s = row.at("selector");
            Fields(s, {"width", "height", "aspect", "form", "role", "required"});
            profile.selector.width = Extent(s.at("width"));
            profile.selector.height = Extent(s.at("height"));
            const auto &aspect = s.at("aspect");
            if (!aspect.is_array() || aspect.size() != 2)
                throw std::invalid_argument("profile aspect bounds");
            profile.selector.aspect = {Ratio(aspect[0]), Ratio(aspect[1])};
            if (!s.at("form").is_null())
                profile.selector.formFactor = static_cast<UiPresentationFormFactor>(Unsigned<std::uint8_t>(s.at("form")));
            if (!s.at("role").is_null())
                profile.selector.viewRole = static_cast<UiPresentationViewRole>(Unsigned<std::uint8_t>(s.at("role")));
            profile.selector.required = static_cast<UiPresentationCapabilities>(Unsigned<std::uint8_t>(s.at("required")));
            for (const auto &item : row.at("overrides")) {
                Fields(item, {"element", "layout"});
                auto presentation = PresentationCodec::Encode(UiElementPresentation{});
                presentation["layout"] = item.at("layout");
                auto parsed = PresentationCodec::Decode(Json(presentation));
                if (parsed.HasError())
                    return Result<UiPresentationProfile>::Failure(parsed.ErrorValue());
                profile.overrides.push_back({Id<UiElementId>(item.at("element")), parsed.Value().layout});
            }
            return Result<UiPresentationProfile>::Success(std::move(profile));
        }
    }  // namespace

    /** @copydoc Encode */
    Out Encode(const std::span<const UiDocumentPresentationProfiles> tables) {
        Out encoded = Out::array();
        for (const auto &table : tables) {
            Out rows = Out::array();
            for (const auto &row : table.rows) {
                const auto &s = row.selector;
                Out overrides = Out::array();
                for (const auto &item : row.overrides) {
                    UiElementPresentation presentation;
                    presentation.layout = item.layout;
                    overrides.push_back(Out{{"element", SerializationInternal::EncodeUiId(item.element)},
                                            {"layout", PresentationCodec::Encode(presentation).at("layout")}});
                }
                rows.push_back(Out{{"id", SerializationInternal::EncodeUiId(row.id)},
                                   {"priority", row.priority},
                                   {"selector", Out{{"width", Extent(s.width)},
                                                    {"height", Extent(s.height)},
                                                    {"aspect", Out::array({Ratio(s.aspect.minimum), Ratio(s.aspect.maximum)})},
                                                    {"form", s.formFactor ? Out(static_cast<std::uint8_t>(*s.formFactor)) : Out(nullptr)},
                                                    {"role", s.viewRole ? Out(static_cast<std::uint8_t>(*s.viewRole)) : Out(nullptr)},
                                                    {"required", static_cast<std::uint8_t>(s.required)}}},
                                   {"overrides", std::move(overrides)}});
            }
            encoded.push_back(Out{{"canvas", SerializationInternal::EncodeUiId(table.canvas)},
                                  {"revision", table.revision.Value()},
                                  {"rows", std::move(rows)}});
        }
        return encoded;
    }

    /** @copydoc Decode */
    Result<std::vector<UiDocumentPresentationProfiles>> Decode(const Json &value) {
        try {
            if (!value.is_array() || value.size() > MaximumUiDocumentCanvases)
                throw std::invalid_argument("profile tables");
            std::size_t totalRows = 0, totalOverrides = 0;
            for (const auto &table : value) {
                Fields(table, {"canvas", "revision", "rows"});
                if (!table.at("rows").is_array())
                    throw std::invalid_argument("profile rows");
                totalRows += table.at("rows").size();
                if (totalRows > MaximumUiPresentationProfiles)
                    throw std::invalid_argument("profile rows capacity");
                for (const auto &row : table.at("rows")) {
                    Fields(row, {"id", "priority", "selector", "overrides"});
                    if (!row.at("overrides").is_array())
                        throw std::invalid_argument("profile overrides");
                    totalOverrides += row.at("overrides").size();
                    if (totalOverrides > MaximumUiPresentationOverrides)
                        throw std::invalid_argument("profile override capacity");
                }
            }
            std::vector<UiDocumentPresentationProfiles> decoded;
            for (const auto &table : value) {
                UiDocumentPresentationProfiles result;
                result.canvas = Id<UiCanvasId>(table.at("canvas"));
                auto revision = UiPresentationProfileRevision::Create(Unsigned<std::uint64_t>(table.at("revision")));
                if (revision.HasError())
                    throw std::invalid_argument("profile revision");
                result.revision = revision.Value();
                for (const auto &row : table.at("rows")) {
                    auto profile = DecodeRow(row);
                    if (profile.HasError())
                        return Result<std::vector<UiDocumentPresentationProfiles>>::Failure(profile.ErrorValue());
                    result.rows.push_back(std::move(profile).Value());
                }
                if (auto valid = ValidateUiPresentationProfiles(result.rows); valid.HasError())
                    return Result<std::vector<UiDocumentPresentationProfiles>>::Failure(valid.ErrorValue());
                decoded.push_back(std::move(result));
            }
            return Result<std::vector<UiDocumentPresentationProfiles>>::Success(std::move(decoded));
        } catch (const std::bad_alloc &) {
            return Result<std::vector<UiDocumentPresentationProfiles>>::Failure(MakeError(UiErrors::CapacityExceeded));
        } catch (const Json::exception &) {
            return Result<std::vector<UiDocumentPresentationProfiles>>::Failure(MakeError(UiErrors::PresentationProfileMalformed));
        } catch (const std::invalid_argument &) {
            return Result<std::vector<UiDocumentPresentationProfiles>>::Failure(MakeError(UiErrors::PresentationProfileMalformed));
        }
    }
}  // namespace Horo::Runtime::Ui::ProfileCodec
