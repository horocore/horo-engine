#pragma once

/**
 * @file CpuParticleSimulator.h
 * @brief Deterministic, allocation-free CPU particle simulation with an atomic seven-stage step.
 */

#include "Horo/Math/SceneMath.h"
#include "Horo/Vfx/CpuParticleBuffer.h"
#include "Horo/Vfx/ParticleSystemDescriptor.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace Horo::Vfx {
    namespace Detail {
        struct CpuParticleSimulatorState;
    }

    /** @brief Fixed logical order of one committed CPU simulation tick. */
    enum class CpuParticleStage : std::uint8_t {
        Spawn,
        Initialize,
        Forces,
        Integrate,
        Collide,
        Kill,
        Extract,
        Count,
    };

    inline constexpr std::array<CpuParticleStage, 7> CpuParticleStageOrder{CpuParticleStage::Spawn,   CpuParticleStage::Initialize,
                                                                           CpuParticleStage::Forces,  CpuParticleStage::Integrate,
                                                                           CpuParticleStage::Collide, CpuParticleStage::Kill,
                                                                           CpuParticleStage::Extract};

    /**
     * @brief Optional owner-thread sentinel used to qualify stage order and inject a contained failure.
     * @param context Caller-owned sentinel state; the simulator retains no ownership.
     * @param stage Stage that is about to execute.
     * @return Success or a typed failure; a failure discards the candidate generation.
     */
    using CpuParticleStageObserver = Result<void> (*)(void *context, CpuParticleStage stage) noexcept;

    /** @brief Bounded preparation ceilings for one CPU simulation instance. */
    struct CpuParticleSimulationHardLimits final {
        static constexpr std::uint32_t ForceModules = 64;          /**< Maximum compiled force modules. */
        static constexpr std::uint32_t Planes = 64;                /**< Maximum analytic collision planes. */
        static constexpr std::uint32_t CurveKeys = 32;             /**< Maximum keys in one over-life curve. */
        static constexpr std::uint32_t PayloadChannels = 32;       /**< Maximum typed custom channels. */
        static constexpr std::uint32_t BurstParticles = 1'000'000; /**< Maximum requested burst per step. */
        static constexpr float DeltaSeconds = 60.0F;               /**< Maximum selected-clock delta per step. */
        static constexpr std::uint32_t RandomAlgorithmVersion = 1; /**< Versioned counter-hash algorithm. */
    };

    /** @brief Force kernel selected by a compiled CPU simulation plan. */
    enum class CpuParticleForceKind : std::uint8_t {
        Gravity,
        Wind,
        Attraction,
        Noise,
        Count,
    };

    /**
     * @brief Immutable force parameters captured during simulation preparation.
     *
     * Gravity and Wind use `vector * strength`. Attraction points at `center`; `falloff` is
     * the distance attenuation coefficient. Noise uses the versioned per-particle counter stream;
     * `strength * frequency` scales its acceleration. Noise channels 1-9 are reserved for
     * initialization, and each noise module in a stack requires a distinct channel.
     */
    struct CpuParticleForceModule final {
        CpuParticleForceKind kind{CpuParticleForceKind::Gravity};
        Math::Vec3 vector{};
        Math::Vec3 center{};
        float strength{1.0F};
        float falloff{};
        float frequency{1.0F};
        std::uint8_t randomChannel{16};
    };

    /** @brief One scalar over-life curve key in normalized age space. Keys must increase strictly. */
    struct CpuParticleCurveKey final {
        float normalizedAge{};
        float value{};
        constexpr auto operator<=>(const CpuParticleCurveKey &) const noexcept = default;
    };

    /** @brief One unit-range color over-life curve key in normalized age space. */
    struct CpuParticleColorKey final {
        float normalizedAge{};
        Math::Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
        constexpr auto operator<=>(const CpuParticleColorKey &) const noexcept = default;
    };

    /** @brief Collision response applied after an immutable query reports a hit. */
    enum class CpuParticleCollisionResponse : std::uint8_t {
        Bounce,
        Die,
        Count,
    };

    /** @brief Backend-neutral analytic collision plane. Planes are tested in declared order. */
    struct CpuParticlePlane final {
        Math::Vec3 point{};
        Math::Vec3 normal{0.0F, 1.0F, 0.0F};
        float restitution{1.0F};
    };

    /** @brief Typed immutable request handed to a scene-depth or Physics adapter. */
    struct CpuParticleCollisionQueryRequest final {
        ParticleSimulationId particle;
        Math::Vec3 previousPosition{};
        Math::Vec3 position{};
        Math::Vec3 velocity{};
        float deltaSeconds{};
        std::uint64_t tick{};
        std::uint64_t sceneGeneration{};
        std::uint64_t snapshotGeneration{};
    };

    /** @brief Reduced collision evidence returned by a typed scene or Physics adapter. */
    struct CpuParticleCollisionHit final {
        bool hit{};
        Math::Vec3 position{};
        Math::Vec3 normal{0.0F, 1.0F, 0.0F};
        float distance{};
        float restitution{1.0F};
        std::uint64_t stableTarget{};
        std::uint32_t stableFeature{};
    };

    /**
     * @brief Non-owning typed seam between VFX and an immutable scene/Physics snapshot.
     *
     * Native worlds, bodies, shapes, collectors, and solver handles never cross the VFX target
     * boundary. The adapter owns `context` and must remain valid for the complete step.
     */
    using CpuParticleCollisionProbe = Result<CpuParticleCollisionHit> (*)(void *context,
                                                                          const CpuParticleCollisionQueryRequest &request) noexcept;

    /** @brief One explicitly captured collision query seam and its generation evidence. */
    struct CpuParticleCollisionQuerySeam final {
        void *context{};
        CpuParticleCollisionProbe probe{};
        std::uint64_t sceneGeneration{};
        std::uint64_t snapshotGeneration{};
        bool required{};
    };

    /** @brief Access classification for custom particle payload channels. */
    enum class CpuParticlePayloadClass : std::uint8_t {
        SimulationInternal,
        RenderOnly,
        GameplayInput,
        GameplayOutput,
        Count,
    };

    /**
     * @brief Bounded schema entry for one custom float channel.
     *
     * `customFloatStream` identifies the preallocated SoA stream; it is not exposed to gameplay.
     */
    struct CpuParticlePayloadChannel final {
        std::uint16_t channel{};
        CpuParticlePayloadClass classification{CpuParticlePayloadClass::SimulationInternal};
        std::uint32_t customFloatStream{};
        float minimum{};
        float maximum{};
    };

    /** @brief Supported bounded CPU payload transforms selected by the gameplay descriptor. */
    enum class CpuParticlePayloadOperation : std::uint8_t {
        Affine,    /**< Output is input * scale + bias. */
        Threshold, /**< Output selects belowValue or atOrAboveValue. */
        Count,
    };

    /**
     * @brief Scalar gameplay payload writer with a recorded operation, stage, and channel boundary.
     *
     * Only Integrate may read a GameplayInput channel and write a distinct GameplayOutput channel.
     * The selected operation runs once per live particle after age integration. Invalid operation,
     * stage, class, duplicate-writer, and range contracts fail preparation with typed diagnostics;
     * gameplay never receives mutable particle storage.
     */
    struct CpuParticlePayloadModule final {
        CpuParticleStage stage{CpuParticleStage::Integrate};
        CpuParticlePayloadOperation operation{CpuParticlePayloadOperation::Affine};
        std::uint16_t readChannel{};
        std::uint16_t writeChannel{};
        float scale{1.0F};
        float bias{};
        float threshold{0.5F};
        float belowValue{};
        float atOrAboveValue{1.0F};
    };

    /** @brief Immutable preparation inputs for one owner-thread CPU simulator. */
    struct CpuParticleSimulatorCreateInfo final {
        ParticleBufferId buffer{};
        EffectSystemId activation{};
        std::uint64_t effectSeed{};
        std::uint32_t maximumBurstParticles{};
        float maximumDeltaSeconds{CpuParticleSimulationHardLimits::DeltaSeconds};
        std::size_t maximumBufferBytes{CpuParticleBufferHardLimits::Bytes};
        std::uint32_t customFloatStreams{};
        bool requiredGameplay{};
        std::span<const CpuParticleForceModule> forces{};
        std::span<const CpuParticlePlane> planes{};
        CpuParticleCollisionQuerySeam sceneDepth{};
        CpuParticleCollisionQuerySeam physicsWorld{};
        CpuParticleCollisionResponse collisionResponse{CpuParticleCollisionResponse::Bounce};
        std::span<const CpuParticleCurveKey> sizeOverLife{};
        std::span<const CpuParticleCurveKey> opacityOverLife{};
        std::span<const CpuParticleColorKey> colorOverLife{};
        std::span<const CpuParticlePayloadChannel> payloadChannels{};
        std::span<const CpuParticlePayloadModule> payloadModules{};
        CpuParticleStageObserver stageObserver{};
        void *stageObserverContext{};
    };

    /** @brief One owner-thread fixed-step request; no field is retained by reference. */
    struct CpuParticleSimulationStep final {
        float deltaSeconds{};
        std::uint32_t burstCount{};
        std::uint64_t tick{};
        bool cancelled{};
    };

    /** @brief Exact result of a successfully committed candidate generation. */
    struct CpuParticleSimulationStepResult final {
        std::uint64_t requestedBirths{};
        std::uint32_t spawned{};
        std::uint64_t dropped{};
        std::uint32_t killed{};
        std::uint32_t collisions{};
        std::uint32_t active{};
        std::uint64_t committedGeneration{};
        constexpr auto operator<=>(const CpuParticleSimulationStepResult &) const noexcept = default;
    };

    /** @brief Read-only committed CPU particle streams valid until the next owner-thread mutation. */
    struct CpuParticleExtractView final {
        std::uint64_t committedGeneration{};
        std::span<const float> positionX, positionY, positionZ;
        std::span<const float> velocityX, velocityY, velocityZ;
        std::span<const float> sizeX, sizeY;
        std::span<const float> rotation, angularVelocity;
        std::span<const std::uint32_t> packedColor;
        std::span<const float> age, maximumAge;
        std::span<const std::uint32_t> customFlags;
        /** @brief Only declared RenderOnly streams are populated; other indices are empty. */
        std::array<std::span<const float>, CpuParticleBufferHardLimits::CustomFloatStreams> customFloats{};
        std::uint32_t customFloatStreamCount{}; /**< Allocated stream count; protected indices remain empty. */
    };

    /** @brief Allocation-free lifetime diagnostics for one CPU simulator. */
    struct CpuParticleSimulationStatistics final {
        std::uint64_t committedGeneration{};
        std::uint64_t steps{};
        std::uint64_t spawned{};
        std::uint64_t dropped{};
        std::uint64_t killed{};
        std::uint64_t collisions{};
        std::uint64_t nextSpawnOrdinal{};
        std::uint32_t active{};
    };

    /**
     * @brief Deterministic owner-thread CPU particle simulator with an atomic seven-stage commit.
     *
     * Create allocates the committed/candidate buffers and every scratch range. Advance executes
     * `Spawn -> Initialize -> Forces -> Integrate -> Collide -> Kill -> Extract` against the
     * candidate generation and publishes it only after all stages succeed. Failure, cancellation,
     * mandatory-capacity exhaustion, and query failure leave the prior committed generation intact.
     */
    class CpuParticleSimulator final {
    public:
        CpuParticleSimulator(const CpuParticleSimulator &) = delete;
        CpuParticleSimulator &operator=(const CpuParticleSimulator &) = delete;
        /** @brief Transfers sole simulator ownership. @param other Source simulator. */
        CpuParticleSimulator(CpuParticleSimulator &&other) noexcept;
        /** @brief Transfers sole simulator ownership. @param other Source simulator. @return This simulator. */
        CpuParticleSimulator &operator=(CpuParticleSimulator &&other) noexcept;
        /** @brief Releases prepared state after readers have quiesced. */
        ~CpuParticleSimulator();

        /**
         * @brief Prepares both generations, fixed scratch, force modules, curves, and payload schema.
         * @param descriptor Validated particle descriptor.
         * @param info Immutable runtime compilation and seam inputs.
         * @return Prepared simulator or a typed descriptor, limit, or allocation failure.
         */
        [[nodiscard]] static Result<CpuParticleSimulator> Create(const ParticleSystemDescriptor &descriptor,
                                                                 const CpuParticleSimulatorCreateInfo &info);

        /**
         * @brief Executes one atomic seven-stage simulation tick.
         * @param step Fixed tick, delta, burst, and cancellation fence.
         * @return Committed result or a typed zero-mutation failure.
         */
        [[nodiscard]] Result<CpuParticleSimulationStepResult> Advance(const CpuParticleSimulationStep &step);

        /**
         * @brief Queues one exact committed particle for the next Kill stage.
         * @param handle Current generation-safe particle handle.
         * @return Success or a typed stale, capacity, thread, or lifecycle failure.
         */
        [[nodiscard]] Result<void> SignalKill(const CpuParticleHandle &handle);

        /**
         * @brief Returns the committed handle at a dense index.
         * @param dense Index inside the current committed live prefix.
         * @return Stable handle or a typed bounds, thread, or lifecycle failure.
         */
        [[nodiscard]] Result<CpuParticleHandle> HandleAtDenseIndex(std::uint32_t dense);

        /** @brief Extracts immutable committed streams. @return Read-only view until the next mutation. */
        [[nodiscard]] Result<CpuParticleExtractView> Extract();

        /**
         * @brief Submits one bounded GameplayInput value for the next step.
         * @param channel Declared payload channel identity.
         * @param value Finite value inside the declared range.
         * @return Success or typed access/schema/thread/lifecycle failure.
         */
        [[nodiscard]] Result<void> SubmitGameplayInput(std::uint16_t channel, float value);

        /**
         * @brief Reads one declared GameplayOutput value from the exact committed generation.
         * @param channel Declared output channel identity.
         * @param dense Current committed dense index.
         * @param committedGeneration Generation returned by Extract or Advance.
         * @return Value or typed access/schema/stale-generation failure.
         */
        [[nodiscard]] Result<float> ReadGameplayOutput(std::uint16_t channel, std::uint32_t dense, std::uint64_t committedGeneration) const;

        /** @brief Returns allocation-free committed diagnostics. @return Current lifetime counters. */
        [[nodiscard]] CpuParticleSimulationStatistics Statistics() const noexcept;

        /** @brief Idempotently closes the simulator and both generations. @return Success or thread failure. */
        [[nodiscard]] Result<void> Shutdown();

    private:
        /** @brief Adopts a completely initialized simulator state. @param state Sole owner state. */
        explicit CpuParticleSimulator(std::unique_ptr<Detail::CpuParticleSimulatorState> state) noexcept;
        std::unique_ptr<Detail::CpuParticleSimulatorState> state_;
    };
}  // namespace Horo::Vfx
