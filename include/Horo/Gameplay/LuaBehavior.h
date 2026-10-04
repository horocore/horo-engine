#pragma once

/**
 * @file LuaBehavior.h
 * @brief Sandboxed Lua 5.4 adapter producing the same behavior descriptor and lifecycle as native gameplay code.
 */

#include "Horo/Gameplay/BehaviorRegistry.h"
#include "Horo/Gameplay/ReplicationRegistration.h"

#include <filesystem>
#include <memory>

namespace Horo::Gameplay {
    class LuaBehaviorInstance;

    /** @brief Per-instance Lua VM resource budgets. */
    struct LuaBehaviorLimits {
        std::size_t maximumMemoryBytes{4U * 1024U * 1024U};
        std::uint32_t maximumInstructionsPerCallback{100'000};
    };

    /** @brief Validated script program whose factory creates scene-scoped Lua behavior instances. */
    class LuaBehaviorProgram final {
    public:
        /**
         * @brief Compiles a non-replicated behavior using the existing source-compatible entry point.
         * @param source UTF-8 Lua source returning a behavior table.
         * @param canonicalTypeId Sidecar-owned stable behavior identity.
         * @param sourceName Diagnostic source name.
         * @param limits Per-instance sandbox budgets.
         * @return Validated program or source-contextual diagnostics; replication requires the identity overload.
         */
        [[nodiscard]] static Result<std::unique_ptr<LuaBehaviorProgram>> Compile(std::string source, const BehaviorTypeId &canonicalTypeId,
                                                                                 std::string sourceName, LuaBehaviorLimits limits = {});

        /**
         * @brief Compiles and validates Lua behavior source against a sidecar-owned stable identity.
         * @param source UTF-8 Lua source returning `horo.behavior { ... }`.
         * @param canonicalTypeId Stable identity read from the script sidecar.
         * @param sourceName Diagnostic source name.
         * @param limits Per-instance sandbox budgets.
         * @param canonicalSchemaId Sidecar-owned schema identity; invalid only for non-replicated scripts.
         * @param canonicalModuleId Sidecar-owned gameplay module declaring the schema.
         * @return Validated program or source-contextual compile/declaration diagnostics.
         */
        [[nodiscard]] static Result<std::unique_ptr<LuaBehaviorProgram>> Compile(std::string source, const BehaviorTypeId &canonicalTypeId,
                                                                                 std::string sourceName, LuaBehaviorLimits limits,
                                                                                 Network::ReplicationSchemaId canonicalSchemaId,
                                                                                 const ModuleId &canonicalModuleId);

        /** @brief Loads `.horo_script` source and JSON metadata sidecar from bounded files. */
        [[nodiscard]] static Result<std::unique_ptr<LuaBehaviorProgram>> LoadFiles(const std::filesystem::path &sourcePath,
                                                                                   const std::filesystem::path &sidecarPath,
                                                                                   LuaBehaviorLimits limits = {});

        ~LuaBehaviorProgram();
        LuaBehaviorProgram(const LuaBehaviorProgram &) = delete;
        LuaBehaviorProgram &operator=(const LuaBehaviorProgram &) = delete;
        /** @brief Returns the language-neutral descriptor discovered from the script. */
        [[nodiscard]] const BehaviorDescriptor &Descriptor() const noexcept;
        /** @brief Returns a factory binding valid while this program remains alive. */
        [[nodiscard]] BehaviorRegistration Registration() noexcept;
        /**
         * @brief Clones the exact validated source generation without reading mutable project files.
         * @return Independent program with the same descriptor, source, limits, and revision.
         */
        [[nodiscard]] Result<std::unique_ptr<LuaBehaviorProgram>> Clone() const;
        /**
         * @brief Atomically accepts a compatible candidate for safe-point reload.
         * @param candidate Fully validated candidate with the same stable schema and schedule.
         * @pre Caller owns the program at a between-tick safe point with no behavior callback running.
         * @return Success or a typed incompatibility error; the previous source remains active on failure.
         */
        [[nodiscard]] Result<void> ReplaceCompatible(std::unique_ptr<LuaBehaviorProgram> candidate);
        /**
         * @brief Returns the generated inert declaration for explicit host composition.
         * @return Borrowed declaration valid until replacement/destruction, or null when replication was not declared.
         */
        [[nodiscard]] const GameplayReplicationRegistration *ReplicationDeclaration() const noexcept;
        /**
         * @brief Acquires immutable typed descriptors and codecs independent of the Lua VM.
         * @return Owning generation lease, or invalid-registration for a non-replicated program.
         */
        [[nodiscard]] Result<GameplayReplicationLease> AcquireReplication() const;
        /**
         * @brief Compiles changed files and publishes only a completely compatible generation.
         * @param sourcePath Script source providing diagnostic context.
         * @param sidecarPath Canonical behavior and replication identities.
         * @pre Caller owns the program at a between-tick safe point with no behavior callback running.
         * @return Success or source diagnostics; failure retains source, revision and registry.
         */
        [[nodiscard]] Result<void> ReloadFiles(const std::filesystem::path &sourcePath, const std::filesystem::path &sidecarPath);
        /** @brief Monotonic source revision observed by live script instances at callback safe points. */
        [[nodiscard]] std::uint64_t Revision() const noexcept;

    private:
        friend class LuaBehaviorInstance;
        struct Impl;
        explicit LuaBehaviorProgram(std::unique_ptr<Impl> impl) noexcept;
        static IBehaviorInstance *CreateInstance(void *userData);
        static void DestroyInstance(void *userData, IBehaviorInstance *instance) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Gameplay
