#include "CollisionIdentityGeneration.h"

#include "Horo/Editor/CollisionProjectSettingsSession.h"

#include <algorithm>
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
    }  // namespace

    /** @copydoc AllocateCollisionIdentityBytes */
    Result<std::array<std::uint8_t, 16>> AllocateCollisionIdentityBytes(const Physics::NormalizedCollisionSchema *existing) {
        try {
            std::random_device random;
            for (unsigned attempt = 0; attempt < 16; ++attempt) {
                std::array<std::uint8_t, 16> bytes;
                for (auto &byte : bytes)
                    byte = static_cast<std::uint8_t>(random());
                bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
                bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
                if (!Used(existing, bytes))
                    return Result<std::array<std::uint8_t, 16>>::Success(bytes);
            }
        } catch (const std::exception &) {
            // Entropy failure is a rejected explicit authoring action, never a deterministic fallback identity.
        }
        return Result<std::array<std::uint8_t, 16>>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity)));
    }
}  // namespace Horo::Editor
