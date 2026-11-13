#pragma once
/** @file VfxGpuKernel.h @brief Complete authentic compute/direct-draw cooked family for the GPU particle word ABI. */
#include "Horo/Runtime/Render/RenderResourceDescriptors.h"
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"
#include "Horo/Runtime/Render/ShaderReflection.h"
#include "Horo/Runtime/Render/VfxGpuSimulation.h"

#include <memory>

namespace Horo::Render {
    /** @brief Logical canonical compute bindings; native placement is always read from final-artifact reflection. */
    enum class VfxGpuComputeBinding : std::uint32_t {
        Source = 1,
        Destination = 2,
        Forces = 3,
        Observations = 4,
        Dispatch = 5
    };
    /** @brief Logical GPU-buffer draw interface, independent of concrete backend slots. */
    enum class VfxGpuDrawBinding : std::uint32_t {
        Particles = 1,
        View = 2
    };

    /** @brief Six uint4 values matching SimulationParameters exactly; float lanes carry explicit IEEE-754 bits. */
    namespace VfxGpuDispatchWords {
        inline constexpr std::size_t Capacity = 0, StreamStride = 1, StreamCount = 2, ForceCount = 3;
        inline constexpr std::size_t Delta = 4, TickLow = 5, TickHigh = 6, ObservationCount = 7;
        inline constexpr std::size_t EffectSeedLow = 8, EffectSeedHigh = 9, ActivationScopeLow = 10, ActivationScopeHigh = 11;
        inline constexpr std::size_t ActivationSlot = 12, ActivationGeneration = 13, EmitterScopeLow = 14, EmitterScopeHigh = 15;
        inline constexpr std::size_t EmitterSlot = 16, EmitterGeneration = 17, SourceGenerationLow = 18, SourceGenerationHigh = 19;
        inline constexpr std::size_t TargetGenerationLow = 20, TargetGenerationHigh = 21, LayoutVersion = 22, RandomVersion = 23;
        inline constexpr std::size_t Count = 24, Bytes = Count * 4;
    }  // namespace VfxGpuDispatchWords

    /** @brief Normalized schema-one observation words; rows are ordered by stable identity, not arbitrary native storage order.
     * @details PreparedParticles seeds are strictly ID-ordered and never compacted/reused; observing the first N slots therefore selects
     * the first N stable identities, including actual inactive records. Result metadata separately carries source identity/tick/generation.
     * No private slot generation, custom-stream data, pointers or shader padding is exposed. Reserved words are canonical zero.
     */
    namespace VfxGpuObservationWords {
        inline constexpr std::uint32_t SchemaVersion = 1;
        inline constexpr std::size_t ParticleLow = 0, ParticleHigh = 1, PositionX = 2, PositionY = 3, PositionZ = 4;
        inline constexpr std::size_t VelocityX = 5, VelocityY = 6, VelocityZ = 7, SizeX = 8, SizeY = 9;
        inline constexpr std::size_t Rotation = 10, AngularVelocity = 11, Age = 12, MaximumAge = 13;
        inline constexpr std::size_t PackedColor = 14, CustomFlags = 15, Alive = 16, Reserved0 = 17, Reserved1 = 18, Reserved2 = 19;
        inline constexpr std::size_t Count = 20, Bytes = Count * 4;
    }  // namespace VfxGpuObservationWords

    /** @brief Thirty-two canonical draw words: matrix16, right/width4, up/height4, originDelta/reserved4, layout4. */
    namespace VfxGpuDrawWords {
        inline constexpr std::size_t ViewProjection = 0, CameraRight = 16, CameraUp = 20, OriginDelta = 24, Layout = 28;
        inline constexpr std::size_t Count = 32, Bytes = Count * 4;
    }  // namespace VfxGpuDrawWords

    /** @brief Captured canonical view; caller computes originDelta from this emitter's simulation origin and captured render origin. */
    struct VfxGpuDrawView final {
        Math::Mat4 viewProjection{Math::Mat4::Identity()};
        Math::Vec3 cameraRight{1, 0, 0};
        Math::Vec3 cameraUp{0, 1, 0};
        Math::Vec3 originDelta;
        FramebufferExtent extent;
    };

    /** @brief Packs the single canonical compute uniform ABI without allocating or writing on failure.
     * @param descriptor Cold validated source metadata. @param work Exact immutable candidate.
     * @param destination At least VfxGpuDispatchWords::Count disjoint numeric words.
     * @return True after all words were written; false for invalid work, dimensions or aliased inputs, preserving destination.
     */
    [[nodiscard]] bool PackVfxGpuDispatchParameters(const VfxGpuSimulationDescriptor &descriptor, const GpuVfxSimulationWork &work,
                                                    std::span<std::uint32_t> destination) noexcept;
    /** @brief Packs the qualified column-native matrix route and direct GPU billboard view ABI without allocation.
     * @param descriptor Immutable admitted state/envelope. @param view Captured canonical camera/origin/extent.
     * @param destination At least VfxGpuDrawWords::Count disjoint numeric words.
     * @return True only for finite safe view arithmetic and a valid layout; failures preserve every destination byte.
     */
    [[nodiscard]] bool PackVfxGpuDrawParameters(const VfxGpuSimulationDescriptor &descriptor, const VfxGpuDrawView &view,
                                                std::span<std::uint32_t> destination) noexcept;
    /** @brief Canonical entry family; renderer selection never chooses a source or runtime compiler. */
    enum class VfxGpuProgramRole : std::uint8_t {
        Simulation,
        Draw
    };

    /** @brief Host-owned actual compiled artifact and exact final reflection for one canonical role. */
    struct CookedVfxGpuProgram final {
        ShaderManifest manifest;
        ShaderTargetRequirement target;
        CompiledShaderArtifact artifact;
        ShaderReflectionCandidate reflection;
    };

    /** @brief Required atomic family; neither a compute-only program nor a CPU draw substitute admits this path. */
    struct CookedVfxGpuPrograms final {
        CookedVfxGpuProgram simulation;
        CookedVfxGpuProgram draw;
    };

    /** @brief Native complete-family pipeline lease; backend subclasses stay private and submitted journals retain their own use. */
    class IResidentVfxGpuKernel {
    public:
        virtual ~IResidentVfxGpuKernel() = default;
    };

    class IRenderGraphResourceLease;

    /** @brief Backend-owned exact simulation/ring lease; private native state never crosses the renderer boundary. */
    class IResidentVfxGpuSimulation {
    public:
        virtual ~IResidentVfxGpuSimulation() = default;
        /** @brief Returns the actual retained native-owner object and CPU vector-capacity payload bytes after cold preparation.
         * @details Excludes allocator/control-block overhead and native driver memory; it is not an exact RSS measurement.
         * The frontend separately accounts native backing and its own retained payloads in the shared ledger.
         */
        [[nodiscard]] virtual std::size_t RetainedPayloadBytes() const noexcept = 0;
    };

    /** @brief Registry-resolved buffer generation and opaque existing backend instance identity, never a native API handle. */
    struct NativeVfxGpuBuffer final {
        RenderBufferHandle handle;
        std::uint64_t instance{};
    };

    /** @brief Cold native parameter-storage requirements fulfilled through ordinary frontend buffer ownership.
     * @details No arena uses zero byteSize, operationSlots and parameterStrideBytes. Otherwise the descriptor must be exactly
     * Uniform/HostVisible, with operationSlots positive and byteSize equal to their checked product with parameterStrideBytes.
     * The stride is at least 128 bytes and aligned to 16 bytes. Native adapters may impose additional device alignment.
     */
    struct NativeVfxGpuPreparationResources final {
        RenderBufferDescriptor parameterArena;
        std::uint32_t operationSlots{};
        std::uint32_t parameterStrideBytes{};
    };

    /** @brief Synchronous cold native realization inputs; every span expires when preparation returns.
     * @details The returned native simulation retains scalar descriptor/inventory and resourceLease, not preparation source spans.
     * Native state tracks exact issued simulation/device, complete kernel/ring inventory, current generation and actual commit journals.
     */
    struct NativeVfxGpuSimulationPreparation final {
        VfxGpuSimulationId id;
        std::uint64_t deviceIncarnation{};
        VfxGpuSimulationPreparation preparation;
        std::shared_ptr<IResidentVfxGpuKernel> kernel;
        std::span<const NativeVfxGpuBuffer> states;
        NativeVfxGpuBuffer forces;
        std::span<const NativeVfxGpuBuffer> observations;
        std::shared_ptr<IRenderGraphResourceLease> resourceLease;
        NativeVfxGpuBuffer parameterArena;
        NativeVfxGpuPreparationResources preparationResources;
    };

    /** @brief Creates one exact portable interface for an explicitly selected target, without compiling or registering anything.
     * @param role Simulation (SimulateParticles) or Draw (VfxGpuVSMain/VfxGpuPSMain).
     * @param target Captured target requirement, including actual storage/compute admission.
     * @return Inert canonical manifest; native preparation must additionally validate actual cooked/native ABI evidence.
     */
    [[nodiscard]] ShaderManifest MakeVfxGpuShaderManifest(VfxGpuProgramRole role, const ShaderTargetRequirement &target);
    /** @brief Provides canonical source identities; these are asset-relative paths, never tool discovery. */
    [[nodiscard]] std::string_view VfxGpuShaderSource(VfxGpuProgramRole role) noexcept;
    /** @brief Supplies exact logical binding/parameter names for offline reflection of the canonical role. */
    [[nodiscard]] std::vector<ShaderReflectionBindingName> VfxGpuShaderReflectionNames(VfxGpuProgramRole role);
    /** @brief Borrows one exact canonical stage from the existing bounded HOROSHDR artifact envelope.
     * @param artifact Actual caller-owned package. @param role Canonical complete-family role.
     * @param stage Compute for Simulation, Vertex/Fragment for Draw.
     * @return Native stage bytes only; duplicate/foreign entries, trailing bytes and unsupported stages fail typed validation.
     */
    [[nodiscard]] Result<std::span<const std::uint8_t>> VfxGpuNativePayload(const CompiledShaderArtifact &artifact, VfxGpuProgramRole role,
                                                                            ShaderStage stage);
    /** @brief Validates the complete authentic cooked family and selected backend without native mutation.
     * @param programs Actual artifacts/reflection owned by cold host preparation.
     * @param expectedBackend Backend selected by composition; this function never falls back or switches it.
     * @return Original validation failure or success; actual native pipeline/argument checks remain mandatory.
     */
    [[nodiscard]] Result<void> ValidateCookedVfxGpuPrograms(const CookedVfxGpuPrograms &programs, ShaderTargetBackend expectedBackend);
}  // namespace Horo::Render
