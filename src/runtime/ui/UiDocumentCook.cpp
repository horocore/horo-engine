#include "Horo/Runtime/Ui/UiDocument.h"

#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        constexpr std::array<std::uint8_t, 8> CookedMagic{'H', 'O', 'R', 'O', 'U', 'I', 'C', '\0'};

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsSupportedSchema(const UiDocumentSchemaVersion version) noexcept {
            return version.major != 0 && version.major == CurrentUiDocumentSchemaVersion.major &&
                   version.minor <= CurrentUiDocumentSchemaVersion.minor;
        }

        class CookedWriter final {
        public:
            explicit CookedWriter(const std::size_t limit) : limit_(limit) {}

            [[nodiscard]] bool Byte(const std::uint8_t value) {
                if (!CanAppend(1))
                    return false;
                bytes_.push_back(value);
                return true;
            }

            [[nodiscard]] bool U16(const std::uint16_t value) {
                return Byte(static_cast<std::uint8_t>(value & 0xFFU)) && Byte(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
            }

            [[nodiscard]] bool U32(const std::uint32_t value) {
                return Byte(static_cast<std::uint8_t>(value & 0xFFU)) && Byte(static_cast<std::uint8_t>((value >> 8U) & 0xFFU)) &&
                       Byte(static_cast<std::uint8_t>((value >> 16U) & 0xFFU)) && Byte(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
            }

            [[nodiscard]] bool U64(const std::uint64_t value) {
                for (std::uint32_t shift = 0; shift < 64; shift += 8)
                    if (!Byte(static_cast<std::uint8_t>((value >> shift) & 0xFFU)))
                        return false;
                return true;
            }

            [[nodiscard]] bool Bytes(const std::span<const std::uint8_t> bytes) {
                if (!CanAppend(bytes.size()))
                    return false;
                bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
                return true;
            }

            [[nodiscard]] bool Text(const std::string_view value, const std::size_t maximumBytes) {
                if (value.size() > maximumBytes || value.size() > std::numeric_limits<std::uint32_t>::max() ||
                    !U32(static_cast<std::uint32_t>(value.size())))
                    return false;
                return Bytes(std::span{reinterpret_cast<const std::uint8_t *>(value.data()), value.size()});
            }

            [[nodiscard]] std::vector<std::uint8_t> Take() && noexcept {
                return std::move(bytes_);
            }

        private:
            [[nodiscard]] bool CanAppend(const std::size_t count) const noexcept {
                return count <= limit_ && bytes_.size() <= limit_ - count;
            }

            std::size_t limit_;
            std::vector<std::uint8_t> bytes_;
        };

        template <typename Id> [[nodiscard]] bool WriteId(CookedWriter &writer, const Id &id) {
            return writer.Bytes(id.Bytes());
        }

        [[nodiscard]] bool WriteType(CookedWriter &writer, const Assets::AssetTypeId &type, const std::size_t maximumTextBytes) {
            return writer.Text(type.Value(), maximumTextBytes);
        }

        [[nodiscard]] bool WriteReference(CookedWriter &writer, const UiReference &reference, const std::size_t maximumTextBytes) {
            return writer.Byte(static_cast<std::uint8_t>(reference.kind)) && WriteId(writer, reference.element) &&
                   WriteId(writer, reference.canvas) && WriteId(writer, reference.document) && WriteId(writer, reference.asset) &&
                   WriteType(writer, reference.expectedAssetType, maximumTextBytes);
        }

        [[nodiscard]] bool WriteProperty(CookedWriter &writer, const UiTypedProperty &property, const std::size_t maximumTextBytes) {
            if (!writer.Text(property.key, maximumTextBytes))
                return false;
            bool written = true;
            std::visit([&](const auto &value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, bool>) {
                    written = writer.Byte(0) && writer.Byte(value ? 1 : 0);
                } else if constexpr (std::is_same_v<Value, std::int64_t>) {
                    written = writer.Byte(1) && writer.U64(std::bit_cast<std::uint64_t>(value));
                } else if constexpr (std::is_same_v<Value, double>) {
                    written = writer.Byte(2) && writer.U64(std::bit_cast<std::uint64_t>(value));
                } else if constexpr (std::is_same_v<Value, std::string>) {
                    written = writer.Byte(3) && writer.Text(value, maximumTextBytes);
                } else if constexpr (std::is_same_v<Value, UiReference>) {
                    written = writer.Byte(4) && WriteReference(writer, value, maximumTextBytes);
                }
            }, property.value);
            return written;
        }

        [[nodiscard]] Result<std::vector<std::uint8_t>> EncodeDocument(const UiDocument &document, const UiDocumentCookLimits &limits) {
            if (!limits.IsValid() || !IsSupportedSchema(document.SchemaVersion()) || document.Canvases().size() > limits.maximumCanvases ||
                document.Elements().size() > limits.maximumElements || document.Dependencies().size() > limits.maximumDependencies ||
                document.Routes().size() > limits.maximumRoutes)
                return Failure<std::vector<std::uint8_t>>(UiErrors::CapacityExceeded);

            for (const UiDocumentElement &element : document.Elements()) {
                if (element.properties.size() > limits.maximumPropertiesPerElement ||
                    element.references.size() > limits.maximumReferencesPerElement)
                    return Failure<std::vector<std::uint8_t>>(UiErrors::CapacityExceeded);
                for (const UiTypedProperty &property : element.properties) {
                    if (property.key.size() > limits.maximumTextBytes)
                        return Failure<std::vector<std::uint8_t>>(UiErrors::CapacityExceeded);
                    if (const auto *text = std::get_if<std::string>(&property.value);
                        text != nullptr && text->size() > limits.maximumTextBytes)
                        return Failure<std::vector<std::uint8_t>>(UiErrors::CapacityExceeded);
                    if (const auto *reference = std::get_if<UiReference>(&property.value);
                        reference != nullptr && reference->expectedAssetType.Value().size() > limits.maximumTextBytes)
                        return Failure<std::vector<std::uint8_t>>(UiErrors::CapacityExceeded);
                }
            }

            if (document.Canvases().size() > std::numeric_limits<std::uint32_t>::max() ||
                document.Elements().size() > std::numeric_limits<std::uint32_t>::max() ||
                document.Dependencies().size() > std::numeric_limits<std::uint32_t>::max() ||
                document.Routes().size() > std::numeric_limits<std::uint32_t>::max())
                return Failure<std::vector<std::uint8_t>>(UiErrors::CapacityExceeded);

            CookedWriter writer{limits.maximumPayloadBytes};
            if (!writer.Bytes(CookedMagic) || !writer.U32(CurrentCookedUiDocumentFormatVersion) ||
                !writer.U16(document.SchemaVersion().major) || !writer.U16(document.SchemaVersion().minor) ||
                !WriteId(writer, document.Id()) || !writer.U64(document.Revision().Value()) ||
                !writer.U32(static_cast<std::uint32_t>(document.Canvases().size())) ||
                !writer.U32(static_cast<std::uint32_t>(document.Elements().size())) ||
                !writer.U32(static_cast<std::uint32_t>(document.Dependencies().size())) ||
                !writer.U32(static_cast<std::uint32_t>(document.Routes().size())))
                return Failure<std::vector<std::uint8_t>>(UiErrors::CapacityExceeded);

            for (const UiCanvasDescriptor &canvas : document.Canvases()) {
                if (!WriteId(writer, canvas.id) || !WriteId(writer, canvas.rootElement) ||
                    !writer.Byte(static_cast<std::uint8_t>(canvas.renderMode)) || !writer.U32(canvas.referenceResolution.width) ||
                    !writer.U32(canvas.referenceResolution.height) || !writer.Byte(static_cast<std::uint8_t>(canvas.scaleMode)))
                    return Failure<std::vector<std::uint8_t>>(UiErrors::CapacityExceeded);
            }

            for (const UiDocumentElement &element : document.Elements()) {
                if (!WriteId(writer, element.id) || !WriteId(writer, element.parent) ||
                    !WriteType(writer, element.type, limits.maximumTextBytes) ||
                    !writer.U32(static_cast<std::uint32_t>(element.properties.size())) ||
                    !writer.U32(static_cast<std::uint32_t>(element.references.size())))
                    return Failure<std::vector<std::uint8_t>>(UiErrors::CapacityExceeded);
                for (const UiTypedProperty &property : element.properties)
                    if (!WriteProperty(writer, property, limits.maximumTextBytes))
                        return Failure<std::vector<std::uint8_t>>(UiErrors::CapacityExceeded);
                for (const UiReference &reference : element.references)
                    if (!WriteReference(writer, reference, limits.maximumTextBytes))
                        return Failure<std::vector<std::uint8_t>>(UiErrors::CapacityExceeded);
            }

            for (const UiAssetDependency &dependency : document.Dependencies()) {
                if (!WriteId(writer, dependency.asset) || !WriteType(writer, dependency.expectedType, limits.maximumTextBytes) ||
                    !writer.Byte(dependency.required ? 1 : 0))
                    return Failure<std::vector<std::uint8_t>>(UiErrors::CapacityExceeded);
            }

            for (const UiRouteMetadata &route : document.Routes()) {
                if (!WriteId(writer, route.id) || !writer.Byte(static_cast<std::uint8_t>(route.band)) || !writer.U32(route.order) ||
                    !writer.Byte(route.modal ? 1 : 0))
                    return Failure<std::vector<std::uint8_t>>(UiErrors::CapacityExceeded);
            }
            return Result<std::vector<std::uint8_t>>::Success(std::move(writer).Take());
        }

        class CookedReader final {
        public:
            explicit CookedReader(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

            [[nodiscard]] bool Bytes(std::span<std::uint8_t> output) {
                if (!CanRead(output.size()))
                    return false;
                std::memcpy(output.data(), bytes_.data() + offset_, output.size());
                offset_ += output.size();
                return true;
            }

            [[nodiscard]] bool Byte(std::uint8_t &value) {
                if (!CanRead(1))
                    return false;
                value = bytes_[offset_++];
                return true;
            }

            [[nodiscard]] bool U16(std::uint16_t &value) {
                std::uint8_t first{}, second{};
                if (!Byte(first) || !Byte(second))
                    return false;
                value = static_cast<std::uint16_t>(first) | (static_cast<std::uint16_t>(second) << 8U);
                return true;
            }

            [[nodiscard]] bool U32(std::uint32_t &value) {
                std::uint8_t bytes[4]{};
                if (!Bytes(bytes))
                    return false;
                value = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                        (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
                return true;
            }

            [[nodiscard]] bool U64(std::uint64_t &value) {
                std::uint8_t bytes[8]{};
                if (!Bytes(bytes))
                    return false;
                value = 0;
                for (std::uint32_t shift = 0; shift < 64; shift += 8)
                    value |= static_cast<std::uint64_t>(bytes[shift / 8]) << shift;
                return true;
            }

            [[nodiscard]] Result<std::string> Text(const std::size_t maximumBytes) {
                std::uint32_t length{};
                if (!U32(length) || length > maximumBytes || !CanRead(length))
                    return Failure<std::string>(UiErrors::CookedPayloadMalformed);
                std::string value(reinterpret_cast<const char *>(bytes_.data() + offset_), length);
                offset_ += length;
                return Result<std::string>::Success(std::move(value));
            }

            [[nodiscard]] bool AtEnd() const noexcept {
                return offset_ == bytes_.size();
            }

        private:
            [[nodiscard]] bool CanRead(const std::size_t count) const noexcept {
                return count <= bytes_.size() && offset_ <= bytes_.size() - count;
            }

            std::span<const std::uint8_t> bytes_;
            std::size_t offset_{};
        };

        template <typename Id> [[nodiscard]] Result<Id> ReadId(CookedReader &reader, const bool allowZero = true) {
            SerializedUiId bytes{};
            if (!reader.Bytes(bytes))
                return Failure<Id>(UiErrors::CookedPayloadMalformed);
            if (bytes == SerializedUiId{})
                return allowZero ? Result<Id>::Success(Id{}) : Failure<Id>(UiErrors::CookedPayloadMalformed);
            if constexpr (std::is_same_v<Id, Assets::AssetId>) {
                return Result<Id>::Success(Assets::AssetId::FromBytes(bytes));
            } else {
                return Id::Create(bytes);
            }
        }

        [[nodiscard]] Result<Assets::AssetTypeId> ReadType(CookedReader &reader, const UiDocumentCookLimits &limits) {
            const auto text = reader.Text(limits.maximumTextBytes);
            if (text.HasError())
                return Result<Assets::AssetTypeId>::Failure(text.ErrorValue());
            if (text.Value().empty())
                return Result<Assets::AssetTypeId>::Success(Assets::AssetTypeId{});
            return Assets::AssetTypeId::Parse(text.Value());
        }

        [[nodiscard]] Result<UiReference> ReadReference(CookedReader &reader, const UiDocumentCookLimits &limits) {
            std::uint8_t kind{};
            if (!reader.Byte(kind))
                return Failure<UiReference>(UiErrors::CookedPayloadMalformed);
            auto element = ReadId<UiElementId>(reader);
            auto canvas = ReadId<UiCanvasId>(reader);
            auto document = ReadId<UiDocumentId>(reader);
            auto asset = ReadId<Assets::AssetId>(reader);
            auto expectedType = ReadType(reader, limits);
            if (element.HasError() || canvas.HasError() || document.HasError() || asset.HasError() || expectedType.HasError())
                return Failure<UiReference>(UiErrors::CookedPayloadMalformed);
            return Result<UiReference>::Success({static_cast<UiReferenceKind>(kind), element.Value(), canvas.Value(), document.Value(),
                                                 asset.Value(), expectedType.Value()});
        }

        [[nodiscard]] Result<UiPropertyValue> ReadPropertyValue(CookedReader &reader, const UiDocumentCookLimits &limits) {
            std::uint8_t tag{};
            if (!reader.Byte(tag))
                return Failure<UiPropertyValue>(UiErrors::CookedPayloadMalformed);
            switch (tag) {
                case 0: {
                    std::uint8_t value{};
                    if (!reader.Byte(value) || value > 1)
                        return Failure<UiPropertyValue>(UiErrors::CookedPayloadMalformed);
                    return Result<UiPropertyValue>::Success(value != 0);
                }
                case 1: {
                    std::uint64_t value{};
                    if (!reader.U64(value))
                        return Failure<UiPropertyValue>(UiErrors::CookedPayloadMalformed);
                    return Result<UiPropertyValue>::Success(std::bit_cast<std::int64_t>(value));
                }
                case 2: {
                    std::uint64_t value{};
                    if (!reader.U64(value))
                        return Failure<UiPropertyValue>(UiErrors::CookedPayloadMalformed);
                    return Result<UiPropertyValue>::Success(std::bit_cast<double>(value));
                }
                case 3: {
                    const auto value = reader.Text(limits.maximumTextBytes);
                    if (value.HasError())
                        return Result<UiPropertyValue>::Failure(value.ErrorValue());
                    return Result<UiPropertyValue>::Success(std::move(value).Value());
                }
                case 4: {
                    const auto value = ReadReference(reader, limits);
                    if (value.HasError())
                        return Result<UiPropertyValue>::Failure(value.ErrorValue());
                    return Result<UiPropertyValue>::Success(std::move(value).Value());
                }
                default:
                    return Failure<UiPropertyValue>(UiErrors::CookedPayloadMalformed);
            }
        }

        [[nodiscard]] Result<UiDocument> DecodeDocument(const std::span<const std::uint8_t> payload, const UiDocumentCookLimits &limits) {
            if (!limits.IsValid())
                return Failure<UiDocument>(UiErrors::CapacityExceeded);
            if (payload.empty())
                return Failure<UiDocument>(UiErrors::CookedPayloadMalformed);
            if (payload.size() > limits.maximumPayloadBytes)
                return Failure<UiDocument>(UiErrors::CapacityExceeded);
            CookedReader reader{payload};
            std::array<std::uint8_t, CookedMagic.size()> magic{};
            std::uint32_t formatVersion{};
            std::uint16_t schemaMajor{}, schemaMinor{};
            SerializedUiId documentBytes{};
            std::uint64_t revisionValue{};
            std::uint32_t canvasCount{}, elementCount{}, dependencyCount{}, routeCount{};
            if (!reader.Bytes(magic) || magic != CookedMagic)
                return Failure<UiDocument>(UiErrors::CookedPayloadMalformed);
            if (!reader.U32(formatVersion))
                return Failure<UiDocument>(UiErrors::CookedPayloadMalformed);
            if (formatVersion != CurrentCookedUiDocumentFormatVersion)
                return Failure<UiDocument>(UiErrors::CookedFormatUnsupported);
            if (!reader.U16(schemaMajor) || !reader.U16(schemaMinor) || !reader.Bytes(documentBytes) || !reader.U64(revisionValue) ||
                !reader.U32(canvasCount) || !reader.U32(elementCount) || !reader.U32(dependencyCount) || !reader.U32(routeCount))
                return Failure<UiDocument>(UiErrors::CookedPayloadMalformed);
            const UiDocumentSchemaVersion schemaVersion{schemaMajor, schemaMinor};
            if (!IsSupportedSchema(schemaVersion))
                return Failure<UiDocument>(UiErrors::DocumentSchemaUnsupported);
            if (documentBytes == SerializedUiId{} || revisionValue == 0)
                return Failure<UiDocument>(UiErrors::CookedPayloadMalformed);
            if (canvasCount > limits.maximumCanvases || elementCount > limits.maximumElements ||
                dependencyCount > limits.maximumDependencies || routeCount > limits.maximumRoutes)
                return Failure<UiDocument>(UiErrors::CapacityExceeded);
            auto documentId = UiDocumentId::Create(documentBytes);
            auto revision = UiDocumentRevision::Create(revisionValue);
            if (documentId.HasError() || revision.HasError())
                return Failure<UiDocument>(UiErrors::CookedPayloadMalformed);

            UiDocumentBuilder builder{documentId.Value(), revision.Value(), schemaVersion};
            for (std::uint32_t index = 0; index < canvasCount; ++index) {
                auto id = ReadId<UiCanvasId>(reader, false);
                auto root = ReadId<UiElementId>(reader, false);
                std::uint8_t renderMode{}, scaleMode{};
                std::uint32_t width{}, height{};
                if (id.HasError() || root.HasError() || !reader.Byte(renderMode) || !reader.U32(width) || !reader.U32(height) ||
                    !reader.Byte(scaleMode))
                    return Failure<UiDocument>(UiErrors::CookedPayloadMalformed);
                if (builder
                        .AddCanvas({id.Value(),
                                    root.Value(),
                                    static_cast<UiRenderMode>(renderMode),
                                    {width, height},
                                    static_cast<UiScaleMode>(scaleMode)})
                        .HasError())
                    return Failure<UiDocument>(UiErrors::CookedPayloadMalformed);
            }

            for (std::uint32_t index = 0; index < elementCount; ++index) {
                auto id = ReadId<UiElementId>(reader, false);
                auto parent = ReadId<UiElementId>(reader);
                auto type = ReadType(reader, limits);
                std::uint32_t propertyCount{}, referenceCount{};
                if (id.HasError() || parent.HasError() || type.HasError() || !reader.U32(propertyCount) || !reader.U32(referenceCount) ||
                    propertyCount > limits.maximumPropertiesPerElement || referenceCount > limits.maximumReferencesPerElement)
                    return Failure<UiDocument>(UiErrors::CookedPayloadMalformed);
                UiDocumentElement element{id.Value(), parent.Value(), type.Value(), {}, {}};
                element.properties.reserve(propertyCount);
                element.references.reserve(referenceCount);
                for (std::uint32_t propertyIndex = 0; propertyIndex < propertyCount; ++propertyIndex) {
                    auto key = reader.Text(limits.maximumTextBytes);
                    auto value = ReadPropertyValue(reader, limits);
                    if (key.HasError() || value.HasError())
                        return Failure<UiDocument>(UiErrors::CookedPayloadMalformed);
                    element.properties.push_back({std::move(key).Value(), std::move(value).Value()});
                }
                for (std::uint32_t referenceIndex = 0; referenceIndex < referenceCount; ++referenceIndex) {
                    auto reference = ReadReference(reader, limits);
                    if (reference.HasError())
                        return Failure<UiDocument>(UiErrors::CookedPayloadMalformed);
                    element.references.push_back(std::move(reference).Value());
                }
                if (builder.AddElement(std::move(element)).HasError())
                    return Failure<UiDocument>(UiErrors::CookedPayloadMalformed);
            }

            for (std::uint32_t index = 0; index < dependencyCount; ++index) {
                auto asset = ReadId<Assets::AssetId>(reader, false);
                auto expectedType = ReadType(reader, limits);
                std::uint8_t required{};
                if (asset.HasError() || expectedType.HasError() || !reader.Byte(required) || required > 1)
                    return Failure<UiDocument>(UiErrors::CookedPayloadMalformed);
                if (builder.RequireAsset({asset.Value(), expectedType.Value(), required != 0}).HasError())
                    return Failure<UiDocument>(UiErrors::CookedPayloadMalformed);
            }

            for (std::uint32_t index = 0; index < routeCount; ++index) {
                auto id = ReadId<UiRouteId>(reader, false);
                std::uint8_t band{}, modal{};
                std::uint32_t order{};
                if (id.HasError() || !reader.Byte(band) || !reader.U32(order) || !reader.Byte(modal) || modal > 1)
                    return Failure<UiDocument>(UiErrors::CookedPayloadMalformed);
                if (builder.AddRoute({id.Value(), static_cast<UiPresentationBand>(band), order, modal != 0}).HasError())
                    return Failure<UiDocument>(UiErrors::CookedPayloadMalformed);
            }
            if (!reader.AtEnd())
                return Failure<UiDocument>(UiErrors::CookedPayloadMalformed);
            return std::move(builder).Build();
        }
    }  // namespace

    /** @copydoc CookedUiDocument::Cook */
    Result<CookedUiDocument> CookedUiDocument::Cook(const UiDocument &document, const UiDocumentCookLimits &limits) {
        try {
            const auto payload = EncodeDocument(document, limits);
            if (payload.HasError())
                return Result<CookedUiDocument>::Failure(payload.ErrorValue());
            return Create(document, std::move(payload).Value());
        } catch (const std::bad_alloc &) {
            return Failure<CookedUiDocument>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc CookedUiDocument::Decode */
    Result<CookedUiDocument> CookedUiDocument::Decode(const std::span<const std::uint8_t> payload, const UiDocumentCookLimits &limits) {
        try {
            const auto document = DecodeDocument(payload, limits);
            if (document.HasError())
                return Result<CookedUiDocument>::Failure(document.ErrorValue());
            return Create(document.Value(), std::vector<std::uint8_t>{payload.begin(), payload.end()});
        } catch (const std::bad_alloc &) {
            return Failure<CookedUiDocument>(UiErrors::CapacityExceeded);
        }
    }
}  // namespace Horo::Runtime::Ui
