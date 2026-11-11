#pragma once

/** @file ConsoleStartup.h @brief Bounded console launch parsing and root-bound allowlisted script preparation. */
#include "Horo/Debug/CommandRegistry.h"
#include "Horo/Platform/ProjectReadFiles.h"

namespace Horo::Debug {
    /** @brief Resolved feature request; parsing constructs no service, endpoint or UI. */
    struct ConsoleLaunchConfiguration final {
        bool console{};
        bool overlays{};
        std::vector<std::string> panels;
        std::vector<std::string> startupCommands;
        std::vector<std::string> scripts;
    };

    /** @brief Parses the console-specific launch argument slice before subsystem startup.
     * @param arguments Only arguments assigned to this adapter by the host parser.
     * @param profile Fixed artifact identity. @param shippingUi Project-approved local Shipping surface.
     * @return Bounded immutable-by-publication request or typed failure; remote endpoints are unsupported and never opened.
     */
    [[nodiscard]] Result<ConsoleLaunchConfiguration> ParseConsoleLaunch(std::span<const std::string_view> arguments, ProductProfile profile,
                                                                        bool shippingUi = false);
    /** @brief Captures an exact approved script via the existing no-follow rooted filesystem authority.
     * @param path Exact portable relative spelling, not a native or normalized prefix.
     * @param approvedPaths Exact project/user paths approved by the host; no wildcard or directory-prefix grants.
     * @param files Existing root-bound filesystem capability. @param readContext Original authority and deadline.
     * @param registry Same immutable registry used for interactive submissions. @param principal Host principal.
     * @return At most 256 prevalidated command lines from a bounded UTF-8 capture. No command is executed here.
     * @note Invoke during startup or from an I/O job, never during interactive frame rendering.
     */
    [[nodiscard]] Result<std::vector<std::string>> ReadConsoleScript(std::string_view path, std::span<const std::string_view> approvedPaths,
                                                                     const Platform::IProjectReadFiles &files,
                                                                     const Platform::ProjectReadContext &readContext,
                                                                     const CommandRegistry &registry, const DebugCommandContext &principal);
}  // namespace Horo::Debug
