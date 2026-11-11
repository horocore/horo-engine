#pragma once

/** @file LuaExtensionServiceBinding.h
 * @brief Bounded Lua projection of the canonical native scalar service profile.
 * @details Owned by the optional Lua binding target, not the source-free module SDK.
 * It reuses the engine's supported Lua dependency and ScriptCapabilityContext authority.
 */
#include "Horo/Extensions/NativeExtensionServiceHost.h"

namespace Horo::Extensions {
    class HeadlessExtensionHost;

    /** @brief Finite host resource policy for one protected reference-script execution. */
    struct LuaExtensionServiceLimits final {
        std::size_t maximumSourceBytes{64U * 1024U};
        std::size_t maximumMemoryBytes{1024U * 1024U};
        std::uint32_t maximumInstructions{100'000}; /**< Exact ceiling; supported multiples1000 in [1000,1000000]. */
    };

    /** @brief Executes actual Lua with only assert, error, tostring and the admitted `api` table.
     * @param binding Actual exact-generation script binding issued by the native service host.
     * @param source Caller-owned script text, copied by Lua within the admitted bound.
     * @param cancellation Host cancellation polled by both instruction hook and provider calls.
     * @param limits Explicit source, allocator and instruction ceilings.
     * @return Script's sole signed integer result or typed runtime/admission/cancellation error.
     * @details No file, process, network, package-loader, debug or engine-global Lua authority is installed.
     */
    [[nodiscard]] Result<std::int64_t> ExecuteLuaExtensionService(NativeExtensionScriptSession &binding, std::string_view source,
                                                                  const CancellationToken &cancellation = {},
                                                                  LuaExtensionServiceLimits limits = {});
    /** @brief Product headless-owner operation; resolves the declared import on the actual active host before running Lua.
     * @param host Actual sole headless composition. @param extension Active package. @param moduleId Importing module.
     * @param importId Manifest-local import. @param policy Actual authenticated script policy/scopes.
     * @param source Authenticated immutable packaged source. @param cancellation Original host token.
     * @param limits Bounded runtime policy. @return Typed result; no provider/VM authority is exposed to Lua.
     */
    [[nodiscard]] Result<std::int64_t> ExecuteHeadlessLuaExtensionService(HeadlessExtensionHost &host, std::string_view extension,
                                                                          std::string_view moduleId, std::string_view importId,
                                                                          ScriptCapabilityContextDescriptor policy, std::string_view source,
                                                                          const CancellationToken &cancellation = {},
                                                                          LuaExtensionServiceLimits limits = {});

}  // namespace Horo::Extensions
