#pragma once

/** @file MaterialBinding.h
 * @brief Generic reflected material layouts, bounded resident tables and explicit fallback admission.
 */

#include "Horo/Runtime/Render/MaterialBindingBackend.h"
#include "Horo/Runtime/Render/StandardPbrMaterial.h"

#include <memory>
#include <optional>
#include <variant>

namespace Horo::Render {
    /** @brief Exact renderer-owned material table generation. */
    struct MaterialBindingGenerationId {
        RenderResourceOwnerId owner;
        std::uint64_t value{0};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return owner.IsValid() && value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const MaterialBindingGenerationId &) const noexcept = default;
    };

    /** @brief Exact buffer generation and admitted byte range. */
    struct MaterialBufferBinding {
        RenderBufferHandle buffer;
        std::size_t offset{0};
        std::size_t byteCount{0};
    };

    /** @brief Owned reflected parameter block; native packing is fixed by final-artifact reflection. */
    struct MaterialParameterBinding {
        std::vector<std::byte> bytes;
    };

    /** @brief One generic logical resource; concrete API objects never cross this boundary. */
    using MaterialBindingValue =
        std::variant<MaterialParameterBinding, MaterialBufferBinding, RenderTextureViewHandle, RenderSamplerHandle>;

    /** @brief Exactly one element of an active reflected resource array. */
    struct MaterialResourceBinding {
        ShaderBindingId binding;
        std::uint32_t arrayElement{0};
        MaterialBindingValue value;
    };

    /** @brief Owned canonical layout consumed unchanged by a selected renderer adapter. */
    struct MaterialBindingLayout {
        ShaderTargetBackend backend{ShaderTargetBackend::Null};
        ShaderInterfaceCompatibilityId shaderInterface;
        std::vector<ShaderReflectedBinding> resources;
        std::vector<ShaderTargetBindingMapEntry> targetBindings;
        std::vector<ShaderReflectedParameter> parameters;
    };

    /** @brief Finite bounds covering current and consumer-retained material generations together. */
    struct MaterialBindingLimits {
        std::size_t maximumGenerations{1024};
        std::size_t maximumResources{128};
        std::size_t maximumParameterBytes{64U * 1024U};
        std::size_t maximumRetainedParameterBytes{16U * 1024U * 1024U};
        /** @brief Validates finite bounds within fixed hard ceilings. @return True for admissible limits. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Complete admitted material input; no source compilation or variant selection occurs here. */
    struct MaterialBindingDescriptor {
        MaterialRuntimeId material;
        std::uint64_t revision{0};
        RenderPipelineHandle pipeline;
        MaterialBindingLayout layout;
        std::vector<MaterialResourceBinding> resources;
        bool required{true};
        std::optional<MaterialBindingGenerationId> authoredFallback;
    };

    /** @brief Immutable consumer lease retaining both descriptor storage and native resource pins. */
    class ResidentMaterialBinding final {
    public:
        /** @brief Returns the exact generation. @return Stable identity for this lease lifetime. */
        [[nodiscard]] MaterialBindingGenerationId Id() const noexcept;
        /** @brief Returns owned logical bindings. @return Borrowed immutable descriptor valid for the lease lifetime. */
        [[nodiscard]] const MaterialBindingDescriptor &Descriptor() const noexcept;
        /** @brief Borrows the renderer-owned adapter lease. @return Native adapter interface, never a native API handle. */
        [[nodiscard]] const IResidentMaterialBinding &BackendBinding() const noexcept;
        ~ResidentMaterialBinding();

    private:
        friend class MaterialBindingTable;
        struct Storage;
        explicit ResidentMaterialBinding(std::unique_ptr<Storage> storage);
        std::unique_ptr<Storage> storage_;
    };

    /** @brief Visible admission outcome; an authored fallback carries the exact unsupported cause. */
    struct MaterialBindingSelection {
        MaterialBindingGenerationId binding;
        bool usedFallback{false};
        std::optional<Error> fallbackReason;
    };

    /** @brief Bounded accounting includes released generations still held by consumers. */
    struct MaterialBindingSnapshot {
        std::size_t retainedGenerations{0};
        std::size_t retainedParameterBytes{0};
        bool accepting{false};
    };

    /**
     * @brief Render-owner table with immutable generation leases and atomic preparation.
     * @details All methods, acquired lease destruction and table destruction run on the creating thread.
     * Publish performs load-time allocation only. Acquire performs bounded lookup without allocation/native work.
     * Failed publication preserves every current generation. Release/Shutdown stop table discovery while existing
     * consumers retain their exact data and native pins. Backend/registry lifetime must exceed all acquired leases.
     */
    class MaterialBindingTable final {
    public:
        /** @brief Creates a finite table. @param owner Frontend incarnation. @param backend Borrowed selected adapter.
         * @param limits Shared live/retained storage bounds. @return Owned table or typed configuration/allocation failure. */
        [[nodiscard]] static Result<std::unique_ptr<MaterialBindingTable>> Create(RenderResourceOwnerId owner,
                                                                                  IMaterialBindingBackend &backend,
                                                                                  const MaterialBindingLimits &limits = {});
        ~MaterialBindingTable();
        MaterialBindingTable(const MaterialBindingTable &) = delete;
        MaterialBindingTable &operator=(const MaterialBindingTable &) = delete;
        /** @brief Atomically realizes one generation or selects its explicit compatible optional fallback.
         * @param descriptor Owned exact cooked material input. @return Generation or typed failure, with no partial publication. */
        [[nodiscard]] Result<MaterialBindingSelection> Publish(MaterialBindingDescriptor descriptor);
        /** @brief Acquires an exact immutable generation. @param binding Exact table identity.
         * @return Owning lease or typed stale/closed/thread failure. */
        [[nodiscard]] Result<std::shared_ptr<const ResidentMaterialBinding>> Acquire(MaterialBindingGenerationId binding) const;
        /** @brief Removes a generation from discovery without invalidating consumer leases.
         * @param binding Exact current table identity. @return Success or typed identity/thread failure. */
        [[nodiscard]] Result<void> Release(MaterialBindingGenerationId binding);
        /** @brief Reports retained budgets. @return Coherent owner-thread snapshot. */
        [[nodiscard]] MaterialBindingSnapshot Snapshot() const noexcept;
        /** @brief Stops admission and removes table references; consumer leases remain valid. @return Success or wrong-thread failure. */
        [[nodiscard]] Result<void> Shutdown();

    private:
        /** @brief Publishes prevalidated prepared storage after adapter realization succeeds. */
        [[nodiscard]] Result<MaterialBindingSelection> Commit(MaterialBindingDescriptor descriptor,
                                                              std::unique_ptr<IResidentMaterialBinding> native, std::size_t parameterBytes);
        class Impl;
        explicit MaterialBindingTable(std::unique_ptr<Impl> implementation);
        std::unique_ptr<Impl> implementation_;
    };

    /** @brief Normalizes validated final-artifact reflection into one owned generic binding layout.
     * @param reflection Exact cooked target reflection. @param limits Finite layout bounds.
     * @return Canonical layout or typed invalid/unsupported/allocation failure. */
    [[nodiscard]] Result<MaterialBindingLayout> PrepareMaterialBindingLayout(const NormalizedShaderReflection &reflection,
                                                                             const MaterialBindingLimits &limits = {});
    /** @brief Validates logical resource membership, packing, arrays and frontend ownership before realization.
     * @param descriptor Exact material input. @param owner Owning frontend. @param limits Finite input bounds.
     * @return Parameter byte charge or typed validation failure. */
    [[nodiscard]] Result<std::size_t> ValidateMaterialBindingDescriptor(const MaterialBindingDescriptor &descriptor,
                                                                        RenderResourceOwnerId owner,
                                                                        const MaterialBindingLimits &limits = {});
}  // namespace Horo::Render
