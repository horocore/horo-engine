#pragma once

#include "Horo/Runtime/Scene/RuntimeScene.h"

namespace Horo::Tests {
    struct GameplayStructuralUnitEvents final {
        std::size_t prepared{}, published{};
    };

    /** @brief Explicit Gameplay owner for Scene command unit tests only; real native behavior acceptance uses the installed host. */
    class GameplayStructuralUnitObserver final : public Runtime::SceneStructuralParticipant {
        class Candidate final : public Runtime::SceneStructuralCandidate {
        public:
            explicit Candidate(std::shared_ptr<GameplayStructuralUnitEvents> events) : events_(std::move(events)) {}

            Result<void> ValidatePublication() const override {
                return Result<void>::Success();
            }

            void Publish() noexcept override {
                if (events_)
                    ++events_->published;
            }

            Result<void> AfterPublication() override {
                return Result<void>::Success();
            }

        private:
            std::shared_ptr<GameplayStructuralUnitEvents> events_;
        };

    public:
        explicit GameplayStructuralUnitObserver(std::shared_ptr<GameplayStructuralUnitEvents> events = {}) : events_(std::move(events)) {}

        Runtime::SceneStructuralOwner Owner() const noexcept override {
            return Runtime::SceneStructuralOwner::Gameplay;
        }

        Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(const Runtime::RuntimeSceneView &,
                                                                           std::span<const Runtime::RuntimeEntityView>,
                                                                           std::span<const Runtime::EntityRef>) override {
            if (events_)
                ++events_->prepared;
            return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Success(std::make_unique<Candidate>(events_));
        }

    private:
        std::shared_ptr<GameplayStructuralUnitEvents> events_;
    };
}  // namespace Horo::Tests
