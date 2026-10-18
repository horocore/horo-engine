#include "CollisionIdentityGeneration.h"

#include "Horo/Editor/CollisionProjectSettingsSession.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <new>
#include <optional>
#include <random>

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

        /** @brief Contains standard-library entropy failures without allocating an error while unwinding.
         * @return Fresh random bytes, or no value when the native entropy implementation fails.
         */
        std::optional<std::array<std::uint8_t, 16>> TryGenerateIdentityBytes() noexcept {
            try {
                std::random_device random;
                std::array<std::uint8_t, 16> bytes;
                for (auto &byte : bytes)
                    byte = static_cast<std::uint8_t>(random());
                return bytes;
            } catch (const std::exception &) {
                return std::nullopt;
            }
        }
    }  // namespace

    /** @copydoc AllocateCollisionIdentityBytes */
    Result<std::array<std::uint8_t, 16>> AllocateCollisionIdentityBytes(const Physics::NormalizedCollisionSchema *existing) {
        try {
            for (unsigned attempt = 0; attempt < 16; ++attempt) {
                const auto generated = TryGenerateIdentityBytes();
                if (!generated)
                    return Result<std::array<std::uint8_t, 16>>::Failure(
                        MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity),
                                  "Collision identity entropy is unavailable."));
                auto bytes = *generated;
                bytes[6] = std::to_integer<std::uint8_t>((std::byte{bytes[6]} & std::byte{0x0f}) | std::byte{0x40});
                bytes[8] = std::to_integer<std::uint8_t>((std::byte{bytes[8]} & std::byte{0x3f}) | std::byte{0x80});
                if (!Used(existing, bytes))
                    return Result<std::array<std::uint8_t, 16>>::Success(bytes);
            }
        } catch (const std::bad_alloc &) {
            return Result<std::array<std::uint8_t, 16>>::Failure(
                MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity), "Collision identity capacity is unavailable."));
        }
        return Result<std::array<std::uint8_t, 16>>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity)));
    }
}  // namespace Horo::Editor
