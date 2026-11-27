#pragma once

/** @file LuaModuleHostApi.h @brief Inert catalog of the engine-owned Lua module host table. */

#include <array>

namespace Horo::Extensions {
    /** @brief Runtime dispatch identity of an engine-owned Lua host member. */
    enum class LuaModuleHostMemberKind {
        Import,
        Unsigned,
        Null
    };

    /** @brief Name and authoring signature shared by runtime registration and editor queries. */
    struct LuaModuleHostMember final {
        LuaModuleHostMemberKind kind;
        const char *name;
        const char *signature;
    };

    inline constexpr char LuaModuleHostReceiver[] = "horo";
    inline constexpr char LuaModuleHostImportName[] = "import";
    inline constexpr char LuaModuleImportAvailable[] = "available";
    inline constexpr char LuaModuleImportErrorCode[] = "code";
    inline constexpr std::array LuaModuleHostMembers{
        LuaModuleHostMember{LuaModuleHostMemberKind::Import, LuaModuleHostImportName, "horo.import(alias: string) -> table"},
        LuaModuleHostMember{LuaModuleHostMemberKind::Unsigned, "u64", "horo.u64(decimal: string) -> uint64"},
        LuaModuleHostMember{LuaModuleHostMemberKind::Null, "null", "horo.null: Null"},
    };
}  // namespace Horo::Extensions
