#include "Horo/Editor/FractureAssetDocument.h"

#include <bit>
#include <cmath>
#include <new>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        constexpr std::uint32_t SourceMagic = 0x53524648;  // HFRS in the canonical little-endian stream.

        /** @brief Padding-free portable source writer; callers have validated all cardinalities. */
        class Writer final {
        public:
            template <typename T> bool Value(const T value) {
                if constexpr (std::is_enum_v<T>)
                    return Value(static_cast<std::uint8_t>(value));
                else if constexpr (std::is_same_v<T, bool>)
                    return Value(static_cast<std::uint8_t>(value));
                else if constexpr (std::is_floating_point_v<T>) {
                    using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
                    return Value(std::bit_cast<Bits>(value == T{} ? T{} : value));
                } else {
                    static_assert(std::is_unsigned_v<T>);
                    for (std::size_t index = 0; index < sizeof(T); ++index)
                        bytes.push_back(static_cast<std::byte>(value >> (index * 8)));
                    return true;
                }
            }

            template <typename Tag> bool Value(const Destruction::DestructionStableIdentity<Tag> value) {
                return Value(value.Value());
            }

            bool Value(const Assets::AssetId &value) {
                return Octets(value.Bytes());
            }

            bool Value(const Destruction::FractureAssetId &value) {
                return Value(value.Asset());
            }

            bool Value(const Sha256Digest &value) {
                return Octets(value.bytes);
            }

            template <typename T, typename Transfer>
            bool Elements(const std::vector<T> &items, std::uint32_t, std::size_t, Transfer transfer) {
                Value(static_cast<std::uint32_t>(items.size()));
                for (const auto &item : items) {
                    if (!transfer(*this, item))
                        return false;
                }
                return true;
            }

            std::vector<std::byte> bytes;

        private:
            template <std::size_t N> bool Octets(const std::array<std::uint8_t, N> &value) {
                for (const auto byte : value)
                    Value(byte);
                return true;
            }
        };

        /** @brief Bounded reader admitting collection counts and minimum wire bytes before allocation. */
        class Reader final {
        public:
            explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

            template <typename T> bool Value(T &value) {
                if constexpr (std::is_enum_v<T>) {
                    std::uint8_t raw{};
                    if (!Value(raw))
                        return false;
                    value = static_cast<T>(raw);
                    return true;
                } else if constexpr (std::is_same_v<T, bool>) {
                    std::uint8_t raw{};
                    if (!Value(raw) || raw > 1)
                        return false;
                    value = raw != 0;
                    return true;
                } else if constexpr (std::is_floating_point_v<T>) {
                    using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
                    Bits raw{};
                    if (!Value(raw))
                        return false;
                    value = std::bit_cast<T>(raw);
                    return std::isfinite(value) && !(value == T{} && std::signbit(value));
                } else {
                    static_assert(std::is_unsigned_v<T>);
                    if (Remaining() < sizeof(T))
                        return false;
                    value = 0;
                    for (std::size_t index = 0; index < sizeof(T); ++index)
                        value |= static_cast<T>(std::to_integer<std::uint8_t>(bytes_[position_++])) << (index * 8);
                    return true;
                }
            }

            template <typename Tag> bool Value(Destruction::DestructionStableIdentity<Tag> &value) {
                std::uint64_t raw{};
                if (!Value(raw))
                    return false;
                // Parent IDs may be absent; full source validation rejects zero in required dimensions.
                value = raw == 0 ? Destruction::DestructionStableIdentity<Tag>{}
                                 : Destruction::DestructionStableIdentity<Tag>::Create(raw).Value();
                return true;
            }

            bool Value(Assets::AssetId &value) {
                std::array<std::uint8_t, 16> raw{};
                if (!Octets(raw))
                    return false;
                value = Assets::AssetId::FromBytes(raw);
                return value.IsValid();
            }

            bool Value(Destruction::FractureAssetId &value) {
                Assets::AssetId raw;
                if (!Value(raw))
                    return false;
                value = Destruction::FractureAssetId::Create(raw).Value();
                return true;
            }

            bool Value(Sha256Digest &value) {
                return Octets(value.bytes);
            }

            template <typename T, typename Transfer>
            bool Elements(std::vector<T> &items, std::uint32_t maximum, std::size_t minimumWireBytes, Transfer transfer) {
                std::uint32_t count{};
                if (!Value(count) || count > maximum || count > Remaining() / minimumWireBytes)
                    return false;
                items.resize(count);
                for (auto &item : items) {
                    if (!transfer(*this, item))
                        return false;
                }
                return true;
            }

            [[nodiscard]] std::size_t Remaining() const noexcept {
                return bytes_.size() - position_;
            }

        private:
            template <std::size_t N> bool Octets(std::array<std::uint8_t, N> &value) {
                for (auto &byte : value) {
                    if (!Value(byte))
                        return false;
                }
                return true;
            }

            std::span<const std::byte> bytes_;
            std::size_t position_{};
        };

        /** @brief Transfers exact source settings; no cache locations or generator results are encoded. */
        template <typename Archive, typename Settings> bool SettingsFields(Archive &archive, Settings &settings) {
            if (!archive.Value(settings.sourceMesh) || !archive.Value(settings.sourceRevision) || !archive.Value(settings.sourceDigest) ||
                !archive.Value(settings.algorithm) || !archive.Value(settings.recipe) || !archive.Value(settings.recipeRevision) ||
                !archive.Value(settings.seed) || !archive.Value(settings.algorithmVersion) || !archive.Value(settings.toolchainDigest) ||
                !archive.Value(settings.tier) || !archive.Value(settings.requiredFeatures.bits) ||
                !archive.Value(settings.interiorMaterialSlot) || !archive.Value(settings.exteriorUvScale) ||
                !archive.Value(settings.interiorUvScale))
                return false;
            return archive.Elements(settings.sites, Destruction::DestructionHardLimits::ChunksPerDestructible, 32,
                                    [](auto &target, auto &site) {
                return target.Value(site.chunk) && target.Value(site.position[0]) && target.Value(site.position[1]) &&
                       target.Value(site.position[2]);
            });
        }

        /** @brief Transfers authored damage policy through the same field order in both directions. */
        template <typename Archive, typename Damage> bool DamageFields(Archive &archive, Damage &damage) {
            return archive.Value(damage.health.maximumHealth) && archive.Value(damage.health.damagedHealthThreshold) &&
                   archive.Value(damage.health.fractureHealthThreshold) && archive.Value(damage.behavior.trigger) &&
                   archive.Value(damage.behavior.support) && archive.Value(damage.behavior.repair) &&
                   archive.Value(damage.behavior.minimumDamageIntervalTicks) && archive.Value(damage.cleanup.chunkRetention) &&
                   archive.Value(damage.cleanup.debris) && archive.Value(damage.cleanup.debrisLifetimeSeconds);
        }

        /** @brief Shares the single exact schema across encoding and decoding without native memory serialization. */
        template <typename Archive, typename Source> bool SourceFields(Archive &archive, Source &source) {
            if (!archive.Value(source.asset) || !SettingsFields(archive, source.settings) ||
                !archive.Elements(source.chunks, Destruction::DestructionHardLimits::ChunksPerDestructible, 22,
                                  [](auto &target, auto &chunk) {
                return target.Value(chunk.id) && target.Value(chunk.parent) && target.Value(chunk.materialSlot) &&
                       target.Value(chunk.anchor) && target.Value(chunk.required);
            }) ||
                !archive.Elements(source.contacts, MaximumFractureContacts, 24,
                                  [](auto &target, auto &contact) {
                return target.Value(contact.low) && target.Value(contact.high) && target.Value(contact.weight);
            }) ||
                !archive.Elements(source.materials, MaximumFractureMaterials, 52, [](auto &target, auto &material) {
                return target.Value(material.slot) && target.Value(material.asset) && target.Value(material.digest);
            }))
                return false;
            return DamageFields(archive, source.damage);
        }
    }  // namespace

    /** @copydoc EncodeFractureAssetSource */
    Result<std::vector<std::byte>> EncodeFractureAssetSource(const FractureAssetSource &source) {
        const auto valid = ValidateFractureAssetSource(source);
        if (valid.HasError())
            return Result<std::vector<std::byte>>::Failure(valid.ErrorValue());
        try {
            Writer writer;
            writer.Value(SourceMagic);
            writer.Value(FractureSourceSchemaVersion);
            if (!SourceFields(writer, source) || writer.bytes.size() > MaximumFractureSourceBytes)
                return Result<std::vector<std::byte>>::Failure(MakeError(FractureDocumentErrors::LimitExceeded));
            return Result<std::vector<std::byte>>::Success(std::move(writer.bytes));
        } catch (const std::bad_alloc &) {
            return Result<std::vector<std::byte>>::Failure(MakeError(FractureDocumentErrors::LimitExceeded));
        }
    }

    /** @copydoc DecodeFractureAssetSource */
    Result<FractureAssetSource> DecodeFractureAssetSource(const std::span<const std::byte> bytes) {
        if (bytes.size() > MaximumFractureSourceBytes)
            return Result<FractureAssetSource>::Failure(MakeError(FractureDocumentErrors::LimitExceeded));
        try {
            Reader reader{bytes};
            std::uint32_t magic{}, version{};
            if (!reader.Value(magic) || magic != SourceMagic || !reader.Value(version))
                return Result<FractureAssetSource>::Failure(MakeError(FractureDocumentErrors::InvalidSource));
            if (version != FractureSourceSchemaVersion)
                return Result<FractureAssetSource>::Failure(MakeError(FractureDocumentErrors::UnsupportedVersion));
            FractureAssetSource source;
            if (!SourceFields(reader, source) || reader.Remaining() != 0)
                return Result<FractureAssetSource>::Failure(MakeError(FractureDocumentErrors::InvalidSource));
            const auto valid = ValidateFractureAssetSource(source);
            if (valid.HasError())
                return Result<FractureAssetSource>::Failure(valid.ErrorValue());
            return Result<FractureAssetSource>::Success(std::move(source));
        } catch (const std::bad_alloc &) {
            return Result<FractureAssetSource>::Failure(MakeError(FractureDocumentErrors::LimitExceeded));
        }
    }
}  // namespace Horo::Editor
