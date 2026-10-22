#include "Horo/Editor/PartitionedWorldWorkspace.h"
#include "Horo/Foundation/Utf8.h"

#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <utility>

namespace Horo::Editor {
    namespace {
        using Json = nlohmann::json;
        namespace WS = WorldStreaming;

        /** @brief Reads an exact integral representation without JSON floating-point coercion or narrowing. */
        template <typename T> T Integer(const Json &value) {
            if (value.is_number_unsigned()) {
                const auto number = value.get<std::uint64_t>();
                if (std::in_range<T>(number))
                    return static_cast<T>(number);
            } else if (value.is_number_integer()) {
                const auto number = value.get<std::int64_t>();
                if (std::in_range<T>(number))
                    return static_cast<T>(number);
            }
            throw std::invalid_argument("integer");
        }

        /** @brief Requires a finite exact schema object, including rejection of unknown fields. */
        void Fields(const Json &value, const std::initializer_list<std::string_view> fields) {
            if (!value.is_object() || value.size() != fields.size())
                throw std::invalid_argument("object");
            for (const auto field : fields)
                if (!value.contains(field))
                    throw std::invalid_argument("field");
        }

        /** @brief Reads exactly three signed millimeter coordinates. */
        Math::WorldCoordinate64 Coordinate(const Json &value) {
            if (!value.is_array() || value.size() != 3)
                throw std::invalid_argument("coordinate");
            return Math::WorldCoordinate64::FromMillimeters(Integer<std::int64_t>(value.at(0)), Integer<std::int64_t>(value.at(1)),
                                                            Integer<std::int64_t>(value.at(2)));
        }

        /** @brief Reads orderedness-neutral exact source bounds; workspace admission owns semantic validation. */
        WS::WorldPartitionBounds Bounds(const Json &value) {
            Fields(value, {"minimum", "maximum"});
            return {.minimum = Coordinate(value.at("minimum")), .maximum = Coordinate(value.at("maximum"))};
        }

        /** @brief Reads a canonical nonzero asset identity. */
        Assets::AssetId Asset(const Json &value) {
            const auto asset = Assets::AssetId::Parse(value.get<std::string>());
            if (asset.HasError() || !asset.Value().IsValid())
                throw std::invalid_argument("asset");
            return asset.Value();
        }

        /** @brief Reads one stable declared layer identity. */
        WS::StreamingLayerId Layer(const Json &value) {
            const auto id = WS::StreamingLayerId::Create(Integer<std::uint16_t>(value));
            if (id.HasError())
                throw std::invalid_argument("layer");
            return id.Value();
        }

        /** @brief Reads a complete exact grid policy without lossy numeric conversions. */
        Result<WS::WorldCellQuantizationPolicy> Grid(const Json &grid) {
            Fields(grid, {"origin", "cellMillimeters", "bounds", "lodLevels"});
            const auto &range = grid.at("bounds");
            if (!range.is_array() || range.size() != 6)
                throw std::invalid_argument("grid bounds");
            return WS::WorldCellQuantizationPolicy::Create(Coordinate(grid.at("origin")), Integer<std::int64_t>(grid.at("cellMillimeters")),
                                                           {Integer<std::int32_t>(range.at(0)), Integer<std::int32_t>(range.at(1)),
                                                            Integer<std::int32_t>(range.at(2)), Integer<std::int32_t>(range.at(3)),
                                                            Integer<std::int32_t>(range.at(4)), Integer<std::int32_t>(range.at(5))},
                                                           Integer<std::uint8_t>(grid.at("lodLevels")));
        }

        /** @brief Decodes one complete named layer; workspace admission owns semantic policy validation. */
        WS::WorldLayerDescriptor DecodeLayer(const Json &layer) {
            Fields(layer, {"id", "name", "ownership", "flags", "priority"});
            if (!layer.at("priority").is_number())
                throw std::invalid_argument("priority");
            return {.id = Layer(layer.at("id")),
                    .name = layer.at("name").get<std::string>(),
                    .ownership = static_cast<WS::WorldLayerOwnership>(Integer<std::uint8_t>(layer.at("ownership"))),
                    .flags = static_cast<WS::WorldLayerFlags>(Integer<std::uint32_t>(layer.at("flags"))),
                    .priorityMultiplier = layer.at("priority").get<float>()};
        }

        /** @brief Decodes one exact page head and its revision-correlated source-object metadata. */
        PartitionedWorldPage DecodePage(const Json &page, const WS::WorldPartitionId world) {
            const auto revision = WS::WorldAuthoringRevision::Create(Integer<std::uint64_t>(page.at("revision")));
            if (revision.HasError())
                throw std::invalid_argument("revision");
            PartitionedWorldPage decoded{
                .head = {.partition = world, .sourceAsset = Asset(page.at("asset")), .revision = revision.Value()}};
            for (const auto &object : page.at("objects")) {
                Fields(object, {"id", "source", "bounds", "placement", "layer"});
                decoded.objects.push_back(
                    {.descriptor = {.address = {.page = decoded.head.sourceAsset, .object = Integer<std::uint64_t>(object.at("id"))},
                                    .revision = revision.Value(),
                                    .sourceAsset = Asset(object.at("source")),
                                    .bounds = Bounds(object.at("bounds")),
                                    .placement =
                                        static_cast<WS::WorldSpatialObjectPlacementClass>(Integer<std::uint8_t>(object.at("placement")))},
                     .layer = Layer(object.at("layer"))});
            }
            return decoded;
        }

        /** @brief Checks aggregate source-object capacity before constructing any page's object vector. */
        Result<void> DecodePages(const Json &pages, PartitionedWorldDocument &document, const std::stop_token cancellation) {
            std::size_t objectCount{};
            for (const auto &page : pages) {
                if (cancellation.stop_requested())
                    return Result<void>::Failure(MakeError(WorldDocumentErrors::Cancelled));
                Fields(page, {"asset", "revision", "objects"});
                const auto &objects = page.at("objects");
                if (!objects.is_array())
                    throw std::invalid_argument("objects");
                if (objects.size() > 65536 - objectCount)
                    return Result<void>::Failure(MakeError(WorldDocumentErrors::Capacity));
                objectCount += objects.size();
                document.pages.push_back(DecodePage(page, document.settings.partition));
            }
            return Result<void>::Success();
        }

        /** @brief Decodes complete source metadata through the existing semantic admission authority. */
        Result<PartitionedWorldDocument> Decode(const Json &root, const std::stop_token cancellation) {
            Fields(root, {"version", "world", "bounds", "grid", "layers", "pages"});
            if (Integer<std::uint16_t>(root.at("version")) != 1)
                return Result<PartitionedWorldDocument>::Failure(MakeError(WorldDocumentErrors::Unsupported));
            const auto owner = Asset(root.at("world"));
            const auto policy = Grid(root.at("grid"));
            if (policy.HasError())
                return Result<PartitionedWorldDocument>::Failure(policy.ErrorValue());
            PartitionedWorldDocument document{.settings = {.partition = WS::WorldPartitionId::Create(owner.Bytes()).Value(),
                                                           .bounds = Bounds(root.at("bounds")),
                                                           .grid = policy.Value()}};
            const auto &layers = root.at("layers");
            const auto &pages = root.at("pages");
            if (!layers.is_array() || !pages.is_array())
                throw std::invalid_argument("arrays");
            if (layers.size() > 256 || pages.size() > 4096)
                return Result<PartitionedWorldDocument>::Failure(MakeError(WorldDocumentErrors::Capacity));
            for (const auto &layer : layers)
                document.layers.push_back(DecodeLayer(layer));
            if (const auto decoded = DecodePages(pages, document, cancellation); decoded.HasError())
                return Result<PartitionedWorldDocument>::Failure(decoded.ErrorValue());
            // The existing authority is the only semantic validator; this temporary admission never reaches application state.
            PartitionedWorldWorkspace validator{owner};
            const auto admitted = validator.Open(document, cancellation);
            if (admitted.HasError())
                return Result<PartitionedWorldDocument>::Failure(admitted.ErrorValue());
            return Result<PartitionedWorldDocument>::Success(std::move(document));
        }
    }  // namespace

    /** @copydoc DecodePartitionedWorldDocument */
    Result<PartitionedWorldDocument> DecodePartitionedWorldDocument(const std::string_view source, const std::stop_token cancellation) {
        if (cancellation.stop_requested())
            return Result<PartitionedWorldDocument>::Failure(MakeError(WorldDocumentErrors::Cancelled));
        if (source.size() > 8 * 1024 * 1024)
            return Result<PartitionedWorldDocument>::Failure(MakeError(WorldDocumentErrors::Capacity));
        if (!IsValidUtf8ScalarSequence(source) || source.find('\0') != std::string_view::npos)
            return Result<PartitionedWorldDocument>::Failure(MakeError(WorldDocumentErrors::Invalid));
        try {
            std::vector<std::set<std::string>> objectKeys;
            const auto callback = [&](const int depth, const Json::parse_event_t event, Json &parsed) {
                if (depth > 32)
                    throw std::length_error("depth");
                if (event == Json::parse_event_t::object_start)
                    objectKeys.emplace_back();
                if (event == Json::parse_event_t::key && !objectKeys.back().insert(parsed.get<std::string>()).second)
                    throw std::invalid_argument("duplicate key");
                if (event == Json::parse_event_t::object_end)
                    objectKeys.pop_back();
                return true;
            };
            return Decode(Json::parse(source, callback), cancellation);
        } catch (const Json::exception &) {
            return Result<PartitionedWorldDocument>::Failure(MakeError(WorldDocumentErrors::Invalid));
        } catch (const std::invalid_argument &) {
            return Result<PartitionedWorldDocument>::Failure(MakeError(WorldDocumentErrors::Invalid));
        } catch (const std::length_error &) {
            return Result<PartitionedWorldDocument>::Failure(MakeError(WorldDocumentErrors::Capacity));
        }
    }
}  // namespace Horo::Editor
