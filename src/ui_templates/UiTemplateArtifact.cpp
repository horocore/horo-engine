#include "Horo/UiTemplates/UiTemplateArtifact.h"

#include "Horo/UiTemplates/UiTemplateErrors.h"
#include "UiTemplateCodec.h"

#include <set>

namespace Horo::UiTemplates {
    namespace {
        using namespace Runtime::Ui;
        using namespace Detail::Codec;
        constexpr std::size_t HeaderBytes = 160;
        constexpr std::array<std::uint8_t, 4> Magic{'H', 'U', 'T', 'A'};

        /** @brief Reads protocol scalars only after the complete fixed header has been admitted. */
        std::uint64_t ReadInteger(const std::span<const std::uint8_t> bytes, const std::size_t offset, const std::size_t width) {
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < width; ++i)
                value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8);
            return value;
        }

        /** @brief Emits explicit little-endian scalars, independent of native object layout. */
        void WriteInteger(std::vector<std::uint8_t> &bytes, const std::uint64_t value, const std::size_t width) {
            for (std::size_t i = 0; i < width; ++i)
                bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
        }

        /** @brief Reads one fixed commitment without accepting truncated or variable-length digests. */
        Sha256Digest ReadDigest(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
            Sha256Digest result;
            std::copy_n(bytes.begin() + offset, result.bytes.size(), result.bytes.begin());
            return result;
        }

        /** @brief Reconstructs ordinary typed authority solely to reuse the closed rich decoder; no template source is needed. */
        UiDocument Ordinary(const CookedUiDocument &cooked) {
            UiDocumentBuilder builder{cooked.Id(), cooked.SourceRevision(), cooked.SchemaVersion()};
            for (const auto &canvas : cooked.Canvases())
                Take(builder.AddCanvas(canvas));
            for (const auto &element : cooked.Elements())
                Take(builder.AddElement(element));
            for (const auto &asset : cooked.Dependencies())
                Take(builder.RequireAsset(asset));
            for (const auto &route : cooked.Routes())
                Take(builder.AddRoute(route));
            return Take(std::move(builder).Build());
        }

        /** @brief Rejects rich target escapes and duplicate exact property/binding identities in the complete runtime projection. */
        void ValidateTargets(const UiTemplateAuthoredDocument &rich) {
            std::set<std::pair<UiElementId, UiStylePropertyId>> properties;
            const auto admit = [&](const UiElementId element, const UiStylePropertyId property) {
                if (!property.IsValid() ||
                    std::ranges::find(rich.ordinary.Elements(), element, &UiDocumentElement::id) == rich.ordinary.Elements().end() ||
                    !properties.emplace(element, property).second)
                    Reject();
            };
            for (const auto &style : rich.styles)
                admit(style.element, style.assignment.property);
            properties.clear();
            for (const auto &text : rich.localizedTexts)
                admit(text.element, text.property);
            std::set<UiBindingId> bindings;
            for (const auto &binding : rich.bindings) {
                if (std::ranges::find(rich.ordinary.Elements(), binding.target.element, &UiDocumentElement::id) ==
                        rich.ordinary.Elements().end() ||
                    !bindings.insert(binding.id).second)
                    Reject();
            }
        }

        /** @brief Checks exact wire version and reserved flags after complete fixed-header admission. */
        bool SupportedHeader(const std::span<const std::uint8_t> bytes) {
            return std::equal(Magic.begin(), Magic.end(), bytes.begin()) && ReadInteger(bytes, 4, 4) == 1 &&
                   ReadInteger(bytes, 8, 4) == 1 && ReadInteger(bytes, 12, 4) == 0;
        }

        /** @brief Admits lengths with checked subtraction before decoder allocations or variable span construction. */
        std::pair<std::size_t, std::size_t> Lengths(const std::span<const std::uint8_t> bytes, const UiTemplateLimits &limits) {
            if (!limits.IsValid() || bytes.size() < HeaderBytes || bytes.size() - HeaderBytes > limits.maximumPayloadBytes)
                Reject();
            if (!SupportedHeader(bytes))
                Reject();
            const auto ordinary = ReadInteger(bytes, 16, 8), rich = ReadInteger(bytes, 24, 8);
            const auto available = bytes.size() - HeaderBytes;
            if (ordinary == 0 || ordinary > available || rich != available - ordinary)
                Reject();
            return {static_cast<std::size_t>(ordinary), static_cast<std::size_t>(rich)};
        }
    }  // namespace

    /** @copydoc EncodeTemplateArtifact */
    Result<std::vector<std::uint8_t>> EncodeTemplateArtifact(const UiTemplateCookedDocument &document, const UiTemplateLimits &limits) {
        using Output = Result<std::vector<std::uint8_t>>;
        return CodecResult<std::vector<std::uint8_t>>([&]() -> Output {
            if (!limits.IsValid() || document.ordinary.Payload().size() > limits.maximumPayloadBytes ||
                document.richPayload.size() > limits.maximumPayloadBytes - document.ordinary.Payload().size() ||
                WriteRich(document.styles, document.localizedTexts, document.bindings).dump() != document.richPayload ||
                CookIdentity(document.sourceIdentity, document.schemaIdentity, document.dependencyManifest, document.ordinary,
                             document.richPayload) != document.cacheIdentity)
                Reject();
            std::vector<std::uint8_t> bytes;
            bytes.reserve(HeaderBytes + document.ordinary.Payload().size() + document.richPayload.size());
            bytes.insert(bytes.end(), Magic.begin(), Magic.end());
            WriteInteger(bytes, 1, 4);
            WriteInteger(bytes, 1, 4);
            WriteInteger(bytes, 0, 4);
            WriteInteger(bytes, document.ordinary.Payload().size(), 8);
            WriteInteger(bytes, document.richPayload.size(), 8);
            for (const auto &digest :
                 {document.sourceIdentity, document.schemaIdentity, document.dependencyManifest, document.cacheIdentity})
                bytes.insert(bytes.end(), digest.bytes.begin(), digest.bytes.end());
            bytes.insert(bytes.end(), document.ordinary.Payload().begin(), document.ordinary.Payload().end());
            bytes.insert(bytes.end(), document.richPayload.begin(), document.richPayload.end());
            return Output::Success(std::move(bytes));
        });
    }

    /** @copydoc DecodeTemplateArtifact */
    Result<UiTemplateCookedDocument> DecodeTemplateArtifact(const std::span<const std::uint8_t> bytes, const Sha256Digest &verifiedManifest,
                                                            const UiTemplateLimits &limits) {
        using Output = Result<UiTemplateCookedDocument>;
        return CodecResult<UiTemplateCookedDocument>([&]() -> Output {
            const auto [ordinaryBytes, richBytes] = Lengths(bytes, limits);
            const auto source = ReadDigest(bytes, 32), schemas = ReadDigest(bytes, 64), manifest = ReadDigest(bytes, 96);
            if (manifest != verifiedManifest || std::ranges::all_of(manifest.bytes, [](const auto byte) {
                return byte == 0;
            }))
                Reject();
            UiDocumentCookLimits policy;
            policy.maximumElements = limits.maximumElements;
            policy.maximumPayloadBytes = limits.maximumPayloadBytes;
            auto ordinary = Take(CookedUiDocument::Decode(bytes.subspan(HeaderBytes, ordinaryBytes), policy));
            const std::string_view payload{reinterpret_cast<const char *>(bytes.data() + HeaderBytes + ordinaryBytes), richBytes};
            const auto cache = CookIdentity(source, schemas, manifest, ordinary, payload);
            if (cache != ReadDigest(bytes, 128) || !IsValidUtf8ScalarSequence(payload))
                Reject();
            UiTemplateAuthoredDocument rich{Ordinary(ordinary)};
            ReadRich(ParseBoundedTemplateJson(payload), rich, limits);
            ValidateTargets(rich);
            if (WriteRich(rich).dump() != payload)
                Reject();
            return Output::Success({std::move(ordinary), std::move(rich.styles), std::move(rich.localizedTexts), std::move(rich.bindings),
                                    std::string{payload}, cache, manifest, source, schemas});
        });
    }
}  // namespace Horo::UiTemplates
