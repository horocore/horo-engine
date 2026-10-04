#pragma once

/**
 * @file Behavior.h
 * @brief Public lifecycle and capability boundary implemented by native and scripted gameplay behaviors.
 */

#include "Horo/Gameplay/BehaviorTypes.h"
#include "Horo/Gameplay/GameplayErrors.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Gameplay {
    class GameplayPhysicsContext;
    inline constexpr std::size_t MaximumBehaviorReloadStateBytes = 1024U * 1024U;
    inline constexpr std::size_t MaximumBehaviorReloadSnapshotBytes = 16U * 1024U * 1024U;

    /** @brief Generation-checked entity identity exposed without leaking runtime storage. */
    struct GameplayEntityRef {
        std::uint64_t scene{};
        std::uint32_t index{};
        std::uint32_t generation{};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return scene != 0 && generation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const GameplayEntityRef &) const noexcept = default;
    };

    /** @brief Stable semantic gameplay action identity. */
    class GameplayActionId final {
    public:
        GameplayActionId() = default;

        explicit GameplayActionId(std::string value) : value_(std::move(value)) {}

        [[nodiscard]] bool IsValid() const noexcept {
            return !value_.empty();
        }

        [[nodiscard]] const std::string &Value() const noexcept {
            return value_;
        }

        [[nodiscard]] auto operator<=>(const GameplayActionId &) const noexcept = default;

    private:
        std::string value_;
    };

    /** @brief Immutable tick-assigned semantic input value. */
    struct GameplayInputAction {
        GameplayActionId action;
        float x{};
        float y{};
        bool down{};
        bool pressed{};
        bool released{};
    };

    /** @brief Stable type identity for one schema-versioned custom gameplay event. */
    class GameplayEventTypeId final {
    public:
        GameplayEventTypeId() = default;

        explicit GameplayEventTypeId(std::string value) : value_(std::move(value)) {}

        [[nodiscard]] bool IsValid() const noexcept {
            return !value_.empty();
        }

        [[nodiscard]] const std::string &Value() const noexcept {
            return value_;
        }

        [[nodiscard]] auto operator<=>(const GameplayEventTypeId &) const noexcept = default;

    private:
        std::string value_;
    };

    /** @brief Host-owned bounded payload delivered at a deterministic fixed-tick event phase. */
    struct GameplayEvent {
        GameplayEventTypeId type;
        std::uint32_t schemaVersion{1};
        std::optional<GameplayEntityRef> target;
        std::vector<BehaviorField> fields;
    };

    struct FixedDeltaTime {
        double seconds{};
    };

    struct FrameDeltaTime {
        double seconds{};
    };

    namespace Detail {
        /** @brief Host-side capability adapter backing one short-lived callback context. */
        class IBehaviorContextBackend {
        public:
            virtual ~IBehaviorContextBackend() = default;
            [[nodiscard]] virtual GameplayEntityRef Entity() const noexcept = 0;
            [[nodiscard]] virtual BehaviorInstanceId InstanceId() const noexcept = 0;
            [[nodiscard]] virtual std::span<const BehaviorField> Fields() const noexcept = 0;
            [[nodiscard]] virtual std::span<const GameplayInputAction> InputActions() const noexcept = 0;
            [[nodiscard]] virtual Result<Math::Transform> LocalTransform() const = 0;
            [[nodiscard]] virtual Result<void> SetLocalTransform(const Math::Transform &transform) = 0;
            [[nodiscard]] virtual Result<void> Publish(GameplayEvent event) = 0;

            /** @brief Returns the explicitly composed module/scene Physics holder, or no grant. */
            [[nodiscard]] virtual std::shared_ptr<const GameplayPhysicsContext> PhysicsContext() const noexcept {
                return {};
            }
        };
    }  // namespace Detail

    /** @brief Narrow scene-scoped capabilities available only during one behavior callback. */
    class BehaviorContext final {
    public:
        explicit BehaviorContext(Detail::IBehaviorContextBackend &backend) noexcept : backend_(&backend) {}

        [[nodiscard]] GameplayEntityRef Entity() const noexcept {
            return backend_->Entity();
        }

        [[nodiscard]] BehaviorInstanceId Instance() const noexcept {
            return backend_->InstanceId();
        }

        [[nodiscard]] std::span<const BehaviorField> Fields() const noexcept {
            return backend_->Fields();
        }

        [[nodiscard]] std::span<const GameplayInputAction> InputActions() const noexcept {
            return backend_->InputActions();
        }

        [[nodiscard]] Result<Math::Transform> LocalTransform() const {
            return backend_->LocalTransform();
        }

        [[nodiscard]] Result<void> SetLocalTransform(const Math::Transform &transform) {
            return backend_->SetLocalTransform(transform);
        }

        [[nodiscard]] Result<void> Publish(GameplayEvent event) {
            return backend_->Publish(std::move(event));
        }

        /** @brief Returns the same host Physics binding for native and script callbacks; no world discovery. */
        [[nodiscard]] std::shared_ptr<const GameplayPhysicsContext> PhysicsContext() const noexcept {
            return backend_->PhysicsContext();
        }

    private:
        Detail::IBehaviorContextBackend *backend_{};
    };

    /** @brief Scene-scoped gameplay behavior lifecycle implemented by native code or a script adapter. */
    class IBehaviorInstance {
    public:
        virtual ~IBehaviorInstance() = default;

        virtual void OnCreate(BehaviorContext &) {
            // Optional hook: instances implement only the lifecycle phases they use.
        }

        virtual void OnEnable(BehaviorContext &) {
            // Optional hook: instances implement only the lifecycle phases they use.
        }

        virtual void OnStart(BehaviorContext &) {
            // Optional hook: instances implement only the lifecycle phases they use.
        }

        virtual void OnInputAction(BehaviorContext &, const GameplayInputAction &) {
            // Optional hook: instances implement only the lifecycle phases they use.
        }

        virtual void OnEvent(BehaviorContext &, const GameplayEvent &) {
            // Optional hook: instances implement only the lifecycle phases they use.
        }

        virtual void OnFixedUpdate(BehaviorContext &, FixedDeltaTime) {
            // Optional hook: instances implement only the lifecycle phases they use.
        }

        virtual void OnPresentationUpdate(BehaviorContext &, FrameDeltaTime) {
            // Optional hook: instances implement only the lifecycle phases they use.
        }

        virtual void OnDisable(BehaviorContext &) {
            // Optional hook: instances implement only the lifecycle phases they use.
        }

        virtual void OnDestroy(BehaviorContext &) {
            // Optional hook: instances implement only the lifecycle phases they use.
        }

        /**
         * @brief Captures bounded runtime-only state before a native module reload.
         * @return Opaque state owned by this behavior type, or a typed failure that aborts reload.
         */
        [[nodiscard]] virtual Result<std::vector<std::byte>> CaptureReloadState() const {
            return Result<std::vector<std::byte>>::Success({});
        }

        /**
         * @brief Restores state captured from the previous compatible module generation.
         * @param state Opaque bytes returned by the old generation of this stable behavior type.
         * @return Success or a typed failure that triggers generation rollback.
         */
        [[nodiscard]] virtual Result<void> RestoreReloadState(std::span<const std::byte> state) {
            return state.empty() ? Result<void>::Success() : Result<void>::Failure(MakeError(GameplayErrors::GameplayReloadRestoreFailed));
        }
    };

    /** @brief Exact-SDK-generation factory functions whose allocator remains owned by the module. */
    struct BehaviorFactoryBinding {
        void *userData{};
        IBehaviorInstance *(*create)(void *userData){};
        void (*destroy)(void *userData, IBehaviorInstance *instance) noexcept {};
    };

    /** @brief One validated descriptor and its module-owned implementation factory. */
    struct BehaviorRegistration {
        BehaviorDescriptor descriptor;
        BehaviorFactoryBinding factory;
    };
}  // namespace Horo::Gameplay
