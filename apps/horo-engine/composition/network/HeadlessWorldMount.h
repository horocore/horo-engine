#pragma once

/** @file HeadlessWorldMount.h
 * @brief Explicit standalone application ownership of a committed World mount.
 */
#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Math/WorldCoordinate64.h"
#include "Horo/WorldStreaming/OriginFrame.h"

#include <memory>

namespace Horo::Runtime {
    class RuntimeSceneService;
}

namespace Horo::Application::Internal {
    /** @brief Exact project launch selection for one separate generated World cook generation. */
    struct HeadlessWorldSelection final {
        std::filesystem::path targetRoot;
        Assets::AssetCookTargetId target;
        Sha256Digest manifest;
        Assets::AssetId index;
        Assets::AssetId dependencies;
        Math::WorldCoordinate64 origin; /**< Explicit authoritative fixed-point startup origin in Horo axes. */
    };

    /** @brief Private candidate and active World owners retained by the actual standalone Scene record. */
    class HeadlessWorldMount final {
    public:
        /** @brief Prepare one verified generation, owned registry, origin, planner and resident-state authority.
         * @param files Explicit host filesystem, borrowed only for cold admission.
         * @param selection Exact project launch selection.
         * @param generation Non-zero unique owner-lane generation of the receiving Scene record.
         * @param scene Real runtime Scene service that outlives the mount.
         * @param cancellation Parent preparation cancellation.
         * @return Complete owned mount or typed error. No authority escapes on failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<HeadlessWorldMount>> Prepare(const DurableFileSystem &files,
                                                                                 const HeadlessWorldSelection &selection,
                                                                                 std::uint64_t generation,
                                                                                 Runtime::RuntimeSceneService &scene,
                                                                                 const CancellationToken &cancellation = {});

        /** @brief Observe the exact mounted World frame; the caller keeps this complete mount alive. */
        [[nodiscard]] const WorldStreaming::OriginFrameOwner &Origin() const noexcept;
        /** @brief Revoke mounted authority and close the registry before releasing the borrowed Scene service. */
        ~HeadlessWorldMount();

    private:
        struct State;
        explicit HeadlessWorldMount(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Application::Internal
