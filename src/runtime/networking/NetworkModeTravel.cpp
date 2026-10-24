#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/NetworkModeComposition.h"
#include "NetworkWorldBindingController.h"

#include <algorithm>
#include <utility>

namespace Horo::Network {
    namespace {
        Result<void> Invalid(const char *message) {
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeInvalid, message));
        }

        Result<void> Unavailable(const char *message) {
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable, message));
        }

        Result<void> Stale(const char *message) {
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeStale, message));
        }
    }  // namespace

    Result<void> NetworkModeComposition::AppendTravelEntry(std::array<Entry, 5> &prepared, std::size_t &count,
                                                           const NetworkModeServiceRequest &request) {
        auto created = factories_.services[static_cast<std::size_t>(request.service)](request);
        if (created.HasError())
            return Result<void>::Failure(created.ErrorValue());
        if (created.Value() == nullptr)
            return Unavailable("A travel factory returned no world service.");
        prepared[count++] = {.request = request, .service = std::move(created).Value()};
        return Result<void>::Success();
    }

    void NetworkModeComposition::DiscardTravelEntries(std::array<Entry, 5> &prepared, std::size_t count) noexcept {
        while (count != 0) {
            auto &entry = prepared[--count];
            if (entry.service != nullptr)
                entry.service->Shutdown();
        }
    }

    Result<std::size_t> NetworkModeComposition::PrepareTravelEntries(const std::span<const NetworkModeWorld> replacements,
                                                                     std::array<Entry, 5> &prepared) {
        std::size_t count = 0;
        for (std::size_t index = 0; index < plan_.worldCount; ++index) {
            for (const auto service : {NetworkModeServiceKind::Scene, NetworkModeServiceKind::Physics}) {
                auto added = AppendTravelEntry(prepared, count, Request(plan_, service, &replacements[index]));
                if (added.HasError()) {
                    DiscardTravelEntries(prepared, count);
                    return Result<std::size_t>::Failure(added.ErrorValue());
                }
            }
        }
        if (plan_.presentation.localPlayer) {
            auto added = AppendTravelEntry(prepared, count,
                                           Request(plan_, NetworkModeServiceKind::LocalPlayer, &replacements[plan_.worldCount - 1]));
            if (added.HasError()) {
                DiscardTravelEntries(prepared, count);
                return Result<std::size_t>::Failure(added.ErrorValue());
            }
        }
        for (std::size_t index = 0; index < count; ++index) {
            auto result = prepared[index].service->Prepare();
            if (result.HasError()) {
                DiscardTravelEntries(prepared, count);
                return Result<std::size_t>::Failure(result.ErrorValue());
            }
        }
        for (std::size_t index = 0; index < count; ++index) {
            auto result = prepared[index].service->Activate();
            if (result.HasError()) {
                DiscardTravelEntries(prepared, count);
                return Result<std::size_t>::Failure(result.ErrorValue());
            }
        }
        return Result<std::size_t>::Success(count);
    }

    void NetworkModeComposition::CommitTravelEntries(const std::span<const NetworkModeWorld> replacements, std::array<Entry, 5> &prepared,
                                                     const std::size_t count) noexcept {
        // Owner-thread safe-point publication has no failing step after preparation.
        // Hide the old role views before swapping either member of a listen pair.
        started_ = false;
        std::array<std::unique_ptr<INetworkModeService>, 5> oldServices{};
        for (std::size_t replacement = 0; replacement < count; ++replacement) {
            for (std::size_t current = 0; current < entryCount_; ++current) {
                if (entries_[current].request.service == prepared[replacement].request.service &&
                    entries_[current].request.world == prepared[replacement].request.world) {
                    oldServices[replacement] = std::move(entries_[current].service);
                    entries_[current] = std::move(prepared[replacement]);
                    break;
                }
            }
        }
        std::ranges::copy(replacements, plan_.worlds.begin());
        for (std::size_t index = count; index != 0; --index)
            oldServices[index - 1]->Shutdown();
        started_ = true;
    }

    /** @copydoc NetworkModeComposition::Travel */
    Result<void> NetworkModeComposition::Travel(const std::span<const NetworkModeWorld> replacements, const Runtime::RuntimePhase phase,
                                                const std::optional<NetworkOperationGeneration> admittedClientSession) {
        if (!started_ || stopped_)
            return Unavailable("Mode worlds are not published.");
        if (phase != Runtime::RuntimePhase::CommitDeferredLifecycleChanges || !WorldShapeMatches(plan_.mode, replacements))
            return Invalid("Travel must replace the entire mode world set at the canonical lifecycle safe point.");
        for (std::size_t index = 0; index < plan_.worldCount; ++index) {
            if (!replacements[index].scene.IsValid() || replacements[index].scene.value <= plan_.worlds[index].scene.value ||

                (index != 0 && replacements[0].scene == replacements[index].scene))
                return Invalid("Travel must supply fresh distinct local Scene generations.");
        }
        if (const auto clientIndex = WorldIndex(NetworkModeWorldKind::Client);
            clientIndex != plan_.worldCount && (!admittedClientSession.has_value() || sessions_[clientIndex] != admittedClientSession))
            return Stale("Client travel requires the current admitted session generation.");
        if (travelPending_ || travelFailure_)
            return Unavailable("A travel candidate or unconsumed travel failure already exists.");
        std::array<Entry, 5> prepared{};
        auto ready = PrepareTravelEntries(replacements, prepared);
        if (ready.HasError())
            return Result<void>::Failure(ready.ErrorValue());
        if (binding_) {
            if (auto staged = binding_->PrepareTravel(replacements); staged.HasError()) {
                DiscardTravelEntries(prepared, ready.Value());
                return staged;
            }
            travelEntries_ = std::move(prepared);
            travelEntryCount_ = ready.Value();
            std::ranges::copy(replacements, travelWorlds_.begin());
            travelPending_ = true;
        } else {
            CommitTravelEntries(replacements, prepared, ready.Value());
        }
        return Result<void>::Success();
    }

    Result<void> NetworkModeComposition::AdvanceTravel(const Runtime::RuntimePhase phase) {
        if (auto pending = binding_->TravelStatus(); pending.HasError())
            return pending;
        if (!binding_->TravelReady())
            return Result<void>::Success();
        if (auto committed = binding_->Commit(phase); committed.HasError())
            return committed;
        // Every service and both detached world lifecycles are ready. The remaining
        // swap cannot fail and no external callback observes half a listen aggregate.
        CommitTravelEntries({travelWorlds_.data(), plan_.worldCount}, travelEntries_, travelEntryCount_);
        travelEntryCount_ = 0;
        travelPending_ = false;
        return Result<void>::Success();
    }

    /** @copydoc NetworkModeComposition::TakeTravelFailure */
    std::optional<Error> NetworkModeComposition::TakeTravelFailure() {
        return std::exchange(travelFailure_, std::nullopt);
    }

}  // namespace Horo::Network
