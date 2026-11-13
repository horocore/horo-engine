#include "FoliageSourceWire.h"
#include "Horo/Terrain/FoliageAuthoringSource.h"

namespace Horo::Terrain {
    namespace {
        constexpr std::uint32_t TypeMagic = 0x31544648;  // HFT1.

        /** @brief Encodes typed fixed-size fields; only one schema traversal owns their order. */
        struct TypeWriter final {
            FoliageWire::Writer wire;

            template <typename T> void Integer(T &value) {
                wire.Integer(value);
            }

            template <typename T> void Enum(T &value) {
                wire.Integer(static_cast<std::uint8_t>(value));
            }

            template <typename T> void Identity(T &value) {
                wire.Bytes(value.Bytes());
            }

            void Boolean(const bool value) {
                wire.Integer(static_cast<std::uint8_t>(value));
            }

            void Revision(const FoliageDefinitionRevision &value) {
                wire.Integer(value.Value());
            }

            void Impostor(std::optional<FoliageImpostorAssetId> &value) {
                wire.Integer(static_cast<std::uint8_t>(value.has_value()));
                const SerializedTerrainIdentity bytes = value ? value->Bytes() : SerializedTerrainIdentity{};
                wire.Bytes(bytes);
            }
        };

        /** @brief Tracks truncated/invalid scalar input without exposing a partially decoded source. */
        struct TypeReader final {
            FoliageWire::Reader wire;
            bool valid{true};

            template <typename T> void Integer(T &value) {
                valid = wire.Integer(value) && valid;
            }

            template <typename T> void Enum(T &value) {
                std::uint8_t byte{};
                Integer(byte);
                value = static_cast<T>(byte);
            }

            template <typename T> void Identity(T &value) {
                SerializedTerrainIdentity bytes{};
                if (!wire.Bytes(bytes)) {
                    valid = false;
                    return;
                }
                auto id = T::Create(bytes);
                if (id.HasError()) {
                    valid = false;
                    return;
                }
                value = id.Value();
            }

            void Identity(Assets::AssetId &value) {
                SerializedTerrainIdentity bytes{};
                valid = wire.Bytes(bytes) && valid;
                value = Assets::AssetId::FromBytes(bytes);
                valid = value.IsValid() && valid;
            }

            void Boolean(bool &value) {
                std::uint8_t byte{};
                Integer(byte);
                valid = byte <= 1 && valid;
                value = byte == 1;
            }

            void Revision(FoliageDefinitionRevision &value) {
                std::uint64_t number{};
                Integer(number);
                auto revision = FoliageDefinitionRevision::Create(number);
                if (revision.HasError()) {
                    valid = false;
                    return;
                }
                value = revision.Value();
            }

            void Impostor(std::optional<FoliageImpostorAssetId> &value) {
                std::uint8_t present{};
                Integer(present);
                SerializedTerrainIdentity bytes{};
                valid = wire.Bytes(bytes) && valid;
                if (!present) {
                    valid = bytes == SerializedTerrainIdentity{} && valid;
                    return;
                }
                auto id = FoliageImpostorAssetId::Create(bytes);
                if (present != 1 || id.HasError()) {
                    valid = false;
                    return;
                }
                value = id.Value();
            }
        };

        /** @brief Shares the exact placement-rule wire field order between encode and decode. */
        template <typename Archive> void PlacementFields(Archive &io, FoliagePlacementDefinition &value) {
            io.Integer(value.algorithmVersion);
            io.Enum(value.algorithm);
            io.Enum(value.alignment);
            io.Integer(value.seed);
            io.Integer(value.densityPerSquareKilometer);
            io.Integer(value.minimumAltitudeMillimeters);
            io.Integer(value.maximumAltitudeMillimeters);
            io.Integer(value.minimumSlopeMilliDegrees);
            io.Integer(value.maximumSlopeMilliDegrees);
            io.Integer(value.minimumSeparationMillimeters);
            io.Integer(value.coordinateQuantumMillimeters);
        }

        /** @brief Shares the exact visibility/wind wire fields, without deriving policy. */
        template <typename Archive> void PresentationFields(Archive &io, FoliageTypeDefinitionData &value) {
            io.Enum(value.culling.recipe);
            for (auto &distance : value.culling.meshLodDistanceMillimeters)
                io.Integer(distance);
            io.Integer(value.culling.impostorStartDistanceMillimeters);
            io.Integer(value.culling.cullDistanceMillimeters);
            io.Integer(value.culling.crossFadeDistanceMillimeters);
            auto &wind = value.wind;
            io.Enum(wind.model);
            io.Integer(wind.primaryStrengthPermille);
            io.Integer(wind.secondaryStrengthPermille);
            io.Integer(wind.primaryFrequencyMilliHertz);
            io.Integer(wind.secondaryFrequencyMilliHertz);
            io.Integer(wind.gustProbabilityPerMillion);
            io.Integer(wind.gustStrengthPermille);
            io.Integer(wind.branchFlexibilityPermille);
            io.Integer(wind.leafFlutterPermille);
        }

        /** @brief Shares fixed authoring definition fields; validation remains the existing definition authority. */
        template <typename Archive> void DefinitionFields(Archive &io, FoliageTypeDefinitionData &value) {
            io.Integer(value.contractVersion);
            io.Identity(value.type);
            io.Revision(value.revision);
            // Unused mesh slots are transported exactly too; the definition validator requires canonical zeroes.
            for (auto &mesh : value.assets.meshLods) {
                SerializedTerrainIdentity bytes = mesh.Bytes();
                for (auto &byte : bytes)
                    io.Integer(byte);
                if (bytes != SerializedTerrainIdentity{})
                    mesh = FoliageMeshAssetId::Create(bytes).Value();
            }
            io.Integer(value.assets.meshLodCount);
            io.Identity(value.assets.material);
            io.Impostor(value.assets.impostor);
            PlacementFields(io, value.placement);
            PresentationFields(io, value);
            auto &collision = value.collision;
            io.Enum(collision.shape);
            io.Integer(collision.radiusMillimeters);
            io.Integer(collision.heightMillimeters);
            io.Boolean(collision.blocksProjectiles);
            io.Boolean(collision.blocksNavigation);
            io.Integer(value.minimumScalePermille);
            io.Integer(value.maximumScalePermille);
            io.Integer(value.maximumInstances);
        }

        /** @brief Reject a missing asset owner and defer all definition policy to the canonical constructor. */
        Result<void> ValidateType(const FoliageTypeSource &source, const TerrainConfigurationSnapshot &configuration,
                                  FoliageDefinitionCapabilitySet capabilities, const CancellationToken &cancel) {
            if (cancel.IsCancellationRequested())
                return Result<void>::Failure(MakeError(TerrainSourceErrors::Cancelled));
            if (!source.asset.IsValid())
                return Result<void>::Failure(MakeError(TerrainSourceErrors::InvalidBytes));
            if (auto definition = FoliageTypeDefinition::Create(source.definition, configuration, capabilities); definition.HasError())
                return Result<void>::Failure(definition.ErrorValue());
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc EncodeFoliageTypeSource */
    Result<std::vector<std::byte>> EncodeFoliageTypeSource(const FoliageTypeSource &source,
                                                           const TerrainConfigurationSnapshot &configuration,
                                                           FoliageDefinitionCapabilitySet capabilities, const CancellationToken &cancel) {
        if (auto valid = ValidateType(source, configuration, capabilities, cancel); valid.HasError())
            return Result<std::vector<std::byte>>::Failure(valid.ErrorValue());
        auto copy = source;
        TypeWriter writer;
        writer.wire.bytes.reserve(512);
        writer.wire.Integer(TypeMagic);
        writer.Identity(copy.asset);
        DefinitionFields(writer, copy.definition);
        return Result<std::vector<std::byte>>::Success(std::move(writer.wire.bytes));
    }

    /** @copydoc DecodeFoliageTypeSource */
    Result<FoliageTypeSource> DecodeFoliageTypeSource(std::span<const std::byte> bytes, Assets::AssetId expectedAsset,
                                                      const TerrainConfigurationSnapshot &configuration,
                                                      FoliageDefinitionCapabilitySet capabilities, const CancellationToken &cancel) {
        if (bytes.size() > 1024)
            return Result<FoliageTypeSource>::Failure(MakeError(TerrainSourceErrors::LimitExceeded));
        TypeReader reader{FoliageWire::Reader{bytes}};
        std::uint32_t magic{};
        reader.Integer(magic);
        FoliageTypeSource source;
        reader.Identity(source.asset);
        DefinitionFields(reader, source.definition);
        if (!reader.valid || !reader.wire.Complete() || magic != TypeMagic || source.asset != expectedAsset)
            return Result<FoliageTypeSource>::Failure(MakeError(TerrainSourceErrors::InvalidBytes));
        if (auto valid = ValidateType(source, configuration, capabilities, cancel); valid.HasError())
            return Result<FoliageTypeSource>::Failure(valid.ErrorValue());
        return Result<FoliageTypeSource>::Success(std::move(source));
    }
}  // namespace Horo::Terrain
