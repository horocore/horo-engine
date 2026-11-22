#include "Horo/Navigation/NavigationCellTileSelection.h"

#include <algorithm>
#include <bit>
#include <new>

namespace Horo::Navigation {
    namespace {
        constexpr std::uint32_t Magic = 0x31434e48;  // HNC1
        constexpr std::size_t HeaderBytes = 4 + 16 + 32 + 4;
        constexpr std::size_t TileBytes = 16 + 8 + 8 + 4 + 4 + 2;

        template <class T> Result<T> Reject(const ErrorCodeDescriptor &code) {
            return Result<T>::Failure(MakeError(code));
        }

        /** @brief Writes a fixed-size unsigned integer in little-endian form. */
        void Write(std::vector<std::byte> &bytes, std::uint64_t value, std::size_t width) {
            for (std::size_t index = 0; index < width; ++index)
                bytes.push_back(static_cast<std::byte>((value >> (8 * index)) & 0xffU));
        }

        /** @brief Reads one bounded row after the enclosing format verified its full extent. */
        std::uint64_t Read(std::span<const std::byte> &bytes, std::size_t width) noexcept {
            std::uint64_t value{};
            for (std::size_t index = 0; index < width; ++index)
                value |= std::to_integer<std::uint64_t>(bytes[index]) << (8 * index);
            bytes = bytes.subspan(width);
            return value;
        }

        /** @brief Preserves canonical UUID and digest bytes independently of native structure layout. */
        template <std::size_t N> std::array<std::uint8_t, N> ReadBytes(std::span<const std::byte> &bytes) noexcept {
            std::array<std::uint8_t, N> result{};
            std::ranges::transform(bytes.first(N), result.begin(), [](std::byte value) {
                return std::to_integer<std::uint8_t>(value);
            });
            bytes = bytes.subspan(N);
            return result;
        }

        /** @brief Checks ownership and canonical order before emitting or returning any selection. */
        bool Canonical(const NavigationCellTileSelection &selection) noexcept {
            if (!selection.definition.IsValid() || selection.cookedEnvelope == Sha256Digest{} || selection.tiles.empty())
                return false;
            const auto &scope = selection.tiles.front().scope;
            const auto invalid = [&scope](const auto &tile) {
                return !tile.profile.IsValid() || !tile.surface.IsValid() || tile.scope != scope;
            };
            return std::ranges::none_of(selection.tiles, invalid) &&
                   std::ranges::adjacent_find(selection.tiles, std::greater_equal<>{}) == selection.tiles.end();
        }

        /** @brief Appends the complete typed scope/profile/surface/grid address. */
        void WriteTile(std::vector<std::byte> &bytes, const NavigationBakeTileKey &tile) {
            const auto scope = std::as_bytes(std::span{tile.scope.SceneBytes()});
            bytes.insert(bytes.end(), scope.begin(), scope.end());
            Write(bytes, tile.profile.Value(), 8);
            Write(bytes, tile.surface.Value(), 8);
            Write(bytes, std::bit_cast<std::uint32_t>(tile.tile.x), 4);
            Write(bytes, std::bit_cast<std::uint32_t>(tile.tile.z), 4);
            Write(bytes, tile.tile.layer, 2);
        }

        /** @brief Decodes a complete address, preserving the explicit unscoped representation. */
        Result<NavigationBakeTileKey> ReadTile(std::span<const std::byte> &bytes) {
            const auto scene = ReadBytes<16>(bytes);
            const auto profile = NavigationAgentProfileId::Create(Read(bytes, 8));
            const auto surface = SurfaceId::Create(Read(bytes, 8));
            if (profile.HasError() || surface.HasError())
                return Reject<NavigationBakeTileKey>(NavigationErrors::NavMeshArtifactInvalid);
            const NavMeshTileKey key{std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(Read(bytes, 4))),
                                     std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(Read(bytes, 4))),
                                     static_cast<std::uint16_t>(Read(bytes, 2))};
            const auto scope =
                scene == std::array<std::uint8_t, 16>{} ? NavigationBakeScopeId{} : NavigationBakeScopeId::ForScene(scene).Value();
            return Result<NavigationBakeTileKey>::Success({profile.Value(), surface.Value(), key, scope});
        }
    }  // namespace

    /** @copydoc EncodeNavigationCellTileSelection */
    Result<std::vector<std::byte>> EncodeNavigationCellTileSelection(const NavigationCellTileSelection &selection,
                                                                     std::size_t maximumBytes) {
        using Bytes = std::vector<std::byte>;
        if (selection.tiles.size() > NavMeshArtifactLimits::MaximumTiles || maximumBytes < HeaderBytes ||
            selection.tiles.size() > (maximumBytes - HeaderBytes) / TileBytes)
            return Reject<Bytes>(NavigationErrors::CapacityExceeded);
        if (!Canonical(selection))
            return Reject<Bytes>(NavigationErrors::NavMeshArtifactInvalid);
        try {
            Bytes bytes;
            bytes.reserve(HeaderBytes + selection.tiles.size() * TileBytes);
            Write(bytes, Magic, 4);
            const auto definition = std::as_bytes(std::span{selection.definition.Bytes()});
            const auto digest = std::as_bytes(std::span{selection.cookedEnvelope.bytes});
            bytes.insert(bytes.end(), definition.begin(), definition.end());
            bytes.insert(bytes.end(), digest.begin(), digest.end());
            Write(bytes, selection.tiles.size(), 4);
            for (const auto &tile : selection.tiles)
                WriteTile(bytes, tile);
            return Result<Bytes>::Success(std::move(bytes));
        } catch (const std::bad_alloc &) {
            return Reject<Bytes>(NavigationErrors::CapacityExceeded);
        }
    }

    /** @copydoc DecodeNavigationCellTileSelection */
    Result<NavigationCellTileSelection> DecodeNavigationCellTileSelection(std::span<const std::byte> bytes, std::size_t maximumBytes) {
        using Selection = NavigationCellTileSelection;
        if (bytes.size() > maximumBytes)
            return Reject<Selection>(NavigationErrors::CapacityExceeded);
        if (bytes.size() < HeaderBytes || Read(bytes, 4) != Magic)
            return Reject<Selection>(NavigationErrors::NavMeshArtifactInvalid);
        Selection selection;
        selection.definition = Assets::AssetId::FromBytes(ReadBytes<16>(bytes));
        selection.cookedEnvelope.bytes = ReadBytes<32>(bytes);
        const auto count = Read(bytes, 4);
        if (count > NavMeshArtifactLimits::MaximumTiles || count * TileBytes != bytes.size())
            return Reject<Selection>(NavigationErrors::NavMeshArtifactInvalid);
        try {
            selection.tiles.reserve(static_cast<std::size_t>(count));
            for (std::uint64_t index = 0; index < count; ++index) {
                const auto tile = ReadTile(bytes);
                if (tile.HasError())
                    return Result<Selection>::Failure(tile.ErrorValue());
                selection.tiles.push_back(tile.Value());
            }
            if (!Canonical(selection))
                return Reject<Selection>(NavigationErrors::NavMeshArtifactInvalid);
            return Result<Selection>::Success(std::move(selection));
        } catch (const std::bad_alloc &) {
            return Reject<Selection>(NavigationErrors::CapacityExceeded);
        }
    }
}  // namespace Horo::Navigation
