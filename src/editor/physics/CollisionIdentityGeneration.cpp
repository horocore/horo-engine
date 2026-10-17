#include "CollisionIdentityGeneration.h"

#include "Horo/Editor/CollisionProjectSettingsSession.h"

#include <algorithm>
#include <cstddef>
#include <new>
#include <random>
#include <stdexcept>

namespace Horo::Editor {
    namespace {
        /** @brief Rejects raw-bit reuse across every existing tagged project definition. */
        bool Used(const Physics::NormalizedCollisionSchema *schema, const std::array<std::uint8_t, 16> &bytes) {
            if (!schema)
                return false;
            const auto same = [&](const auto &entry) {
                return entry.id.Bytes() == bytes;
            };
            return std::ranges::any_of(schema->Layers(), same) || std::ranges::any_of(schema->Profiles(), same) ||
                   std::ranges::any_of(schema->QueryChannels(), same);
        }
    }  // namespace

    /** @copydoc AllocateCollisionIdentityBytes */
    Result<std::array<std::uint8_t, 16>> AllocateCollisionIdentityBytes(const Physics::NormalizedCollisionSchema *existing) {
        try {
            std::random_device random;
            for (unsigned attempt = 0; attempt < 16; ++attempt) {
                std::array<std::uint8_t, 16> bytes;
                for (auto &byte : bytes)
                    byte = static_cast<std::uint8_t>(random());
                bytes[6] = std::to_integer<std::uint8_t>((std::byte{bytes[6]} & std::byte{0x0f}) | std::byte{0x40});
                bytes[8] = std::to_integer<std::uint8_t>((std::byte{bytes[8]} & std::byte{0x3f}) | std::byte{0x80});
                if (!Used(existing, bytes))
                    return Result<std::array<std::uint8_t, 16>>::Success(bytes);
            }
        } catch (const std::runtime_error &) {
            return Result<std::array<std::uint8_t, 16>>::Failure(
                MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity), "Collision identity entropy is unavailable."));
        } catch (const std::bad_alloc &) {
            return Result<std::array<std::uint8_t, 16>>::Failure(
                MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity), "Collision identity capacity is unavailable."));
        }
        return Result<std::array<std::uint8_t, 16>>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity)));
    }
}  // namespace Horo::Editor
