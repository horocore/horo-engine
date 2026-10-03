#include "Horo/Cinematic/EventTrack.h"

#include "Horo/Cinematic/EventTrackErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::Cinematic {
    namespace {
        constexpr std::size_t MaximumCookedEventKeys = 2'048;
        constexpr std::size_t MaximumCookedEventPayloadBytes = 64U * 1024U;

        [[nodiscard]] bool KeyLess(const CookedEventKey &left, const CookedEventKey &right) noexcept {
            if (left.track != right.track)
                return left.track < right.track;
            return left.key < right.key;
        }
    }  // namespace

    /** @copydoc CookedEventPlan::Create */
    Result<std::shared_ptr<const CookedEventPlan>> CookedEventPlan::Create(std::vector<CookedEventKey> keys) {
        if (keys.size() > MaximumCookedEventKeys)
            return Result<std::shared_ptr<const CookedEventPlan>>::Failure(MakeError(EventTrackErrors::CookCapacityExceeded));
        std::size_t retainedPayloadBytes{};
        for (const CookedEventKey &key : keys) {
            if (!key.track.IsValid() || !key.key.IsValid() || !key.binding.IsValid() || !key.schema.IsValid() ||
                key.allowedContexts == std::byte{} || (key.allowedContexts & ~std::byte{15}) != std::byte{} || key.payload.empty())
                return Result<std::shared_ptr<const CookedEventPlan>>::Failure(MakeError(EventTrackErrors::CookInvalid));
            if (key.payload.size() > MaximumCookedEventPayloadBytes)
                return Result<std::shared_ptr<const CookedEventPlan>>::Failure(MakeError(EventTrackErrors::CookCapacityExceeded));
            retainedPayloadBytes += key.payload.capacity();
            if (retainedPayloadBytes > 32U * 1024U * 1024U)
                return Result<std::shared_ptr<const CookedEventPlan>>::Failure(MakeError(EventTrackErrors::CookCapacityExceeded));
        }
        std::ranges::sort(keys, KeyLess);
        if (std::ranges::adjacent_find(keys, [](const auto &left, const auto &right) {
            return left.track == right.track && left.key == right.key;
        }) != keys.end())
            return Result<std::shared_ptr<const CookedEventPlan>>::Failure(MakeError(EventTrackErrors::CookInvalid));
        for (std::size_t left = 0; left < keys.size(); ++left)
            for (std::size_t right = left + 1; right < keys.size(); ++right)
                if (keys[left].binding == keys[right].binding && keys[left].schema != keys[right].schema)
                    return Result<std::shared_ptr<const CookedEventPlan>>::Failure(MakeError(EventTrackErrors::SchemaMismatch));
        return Result<std::shared_ptr<const CookedEventPlan>>::Success(
            std::make_shared<const CookedEventPlan>(ConstructionKey{}, std::move(keys)));
    }

    /** @copydoc CookedEventPlan::Find */
    const CookedEventKey *CookedEventPlan::Find(const TrackId track, const KeyframeId key) const noexcept {
        // A payload-bearing search sentinel allocates an empty vector's debug proxy on MSVC.
        const auto found = std::ranges::lower_bound(keys_, std::pair{track, key}, std::ranges::less{}, [](const CookedEventKey &entry) {
            return std::pair{entry.track, entry.key};
        });
        return found != keys_.end() && found->track == track && found->key == key ? std::to_address(found) : nullptr;
    }

    /** @copydoc CookedEventPlan::Keys */
    std::span<const CookedEventKey> CookedEventPlan::Keys() const noexcept {
        return keys_;
    }

    CookedEventPlan::CookedEventPlan(ConstructionKey, std::vector<CookedEventKey> keys) noexcept : keys_(std::move(keys)) {}
}  // namespace Horo::Cinematic
