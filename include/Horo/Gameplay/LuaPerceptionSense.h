#pragma once

/** @file LuaPerceptionSense.h
 * @brief Enforced, allocation-free callback tier for custom spatial perception under native-owner revocation.
 */

#include "Horo/AI/PerceptionSenseProvider.h"
#include "Horo/Gameplay/LuaBehavior.h"

#include <utility>

namespace Horo::Gameplay {
    /** @brief Immutable script program; each admitted instance owns a separate bounded Lua VM. */
    class LuaPerceptionSenseProgram final {
    public:
        /** @brief Compiles an owned source chunk returning a scalar candidate predicate.
         * @param source Text-only chunk returning function(x,y,z,listenerX,listenerY,listenerZ); positions are exact int64 millimeters.
         * @param contract Host-selected stable Script sense/stimulus identities and bounded candidate/work policy.
         * @param parent Actual native module perception generation; mandatory and retained through VM destruction.
         * @param limits VM memory and whole-evaluation instruction ceilings, enforced by the existing Lua allocator and hook.
         * @return Program or typed metadata/storage/provider failure. Compile executes only inside the restricted VM.
         * @details The predicate returns boolean sensing or nil to omit that candidate. Evaluation forbids all Lua allocation
         * and exposes no Scene, blackboard, entity mutation, files, clock, logging, native handles or workers. Source and VM
         * storage are admitted at activation. Allocation attempts, invalid output and instruction overruns quarantine that
         * instance; the parent cannot be replaced with an unrelated decision-node or metadata-only authority.
         */
        [[nodiscard]] static Result<std::unique_ptr<LuaPerceptionSenseProgram>> Compile(
            std::string source, AI::PerceptionSenseContract contract, std::shared_ptr<const AI::PerceptionSenseGeneration> parent,
            LuaBehaviorLimits limits = {256U * 1024U, 4096});
        /** @brief Revokes every acquired script generation before releasing the program's image. */
        ~LuaPerceptionSenseProgram();
        LuaPerceptionSenseProgram(const LuaPerceptionSenseProgram &) = delete;
        LuaPerceptionSenseProgram &operator=(const LuaPerceptionSenseProgram &) = delete;
        /** @brief Acquires enforced Script factories under the actual native parent and owning thread.
         * @return Revocable generation, or unavailable after either program or parent retirement.
         */
        [[nodiscard]] Result<std::shared_ptr<const AI::PerceptionSenseGeneration>> Acquire() const;
        /** @brief Closes callback admission without unloading retained VMs or native parents. */
        void Retire() noexcept;

    private:
        struct Image;
        class Instance;

        explicit LuaPerceptionSenseProgram(std::shared_ptr<Image> image) : image_(std::move(image)) {}

        std::shared_ptr<Image> image_;
    };
}  // namespace Horo::Gameplay
