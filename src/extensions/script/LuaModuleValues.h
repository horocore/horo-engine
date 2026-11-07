#pragma once
#include "Horo/Extensions/ScriptModuleRuntime.h"
extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace Horo::Extensions::LuaModuleDetail {
    struct Failure final {
        Error error;
    };

    [[noreturn]] void Invalid(std::string_view);

    struct Budget final {
        const ScriptValueLimits &limits;
        const std::chrono::steady_clock::time_point &deadline;
        std::size_t nodes{}, bytes{};
        void Charge(std::size_t depth, std::size_t payload = 0);
    };

    struct Ledger final {
        std::size_t maximumBytes{}, maximumValues{}, bytes{}, values{};
    };
    enum class BoxKind : std::uint8_t {
        Null,
        Unsigned,
        Handle,
        Outcome
    };

    struct Box final {
        BoxKind kind{BoxKind::Null};
        std::vector<std::byte> encoded;
        std::shared_ptr<Ledger> ledger;
        std::size_t cost{};
        ~Box();
    };

    /** Read operations use only raw, nonallocating Lua operations after caller stack admission. */
    struct Values final {
        lua_State *lua{};
        int metatable{LUA_NOREF};
        std::shared_ptr<ScriptModuleContext> authority;
        std::shared_ptr<Ledger> ledger;
        std::chrono::steady_clock::time_point *deadline{};
        std::unique_ptr<Box> preparedBox;
        [[nodiscard]] const Box *GetBox(int) const;
        [[nodiscard]] ScriptValue Unbox(int, BoxKind) const;
        [[nodiscard]] ScriptValue Read(int, const ScriptExportTypeReference &, const ScriptExportDescriptor &, Budget &, std::size_t = 0);
        [[nodiscard]] ScriptValue Primitive(int, ScriptExportPrimitiveKind, Budget &, std::size_t);
        [[nodiscard]] ScriptValue Named(int, const ScriptExportTypeDescriptor &, const ScriptExportDescriptor &, Budget &, std::size_t);
        [[nodiscard]] ScriptValue Array(int, const ScriptExportTypeDescriptor &, const ScriptExportDescriptor &, Budget &, std::size_t);
        [[nodiscard]] ScriptValue Map(int, const ScriptExportTypeDescriptor &, const ScriptExportDescriptor &, Budget &, std::size_t);
        [[nodiscard]] ScriptValue Struct(int, const ScriptExportTypeDescriptor &, const ScriptExportDescriptor &, Budget &, std::size_t);
        void Plain(int) const;
        [[nodiscard]] std::size_t Length(int, Budget &) const;
        void PrepareBox(BoxKind, std::vector<std::byte>);
        void PrepareValueBox(BoxKind, const ScriptValue &);
        /** POD-only allocation/push layer: no C++ owner may live across these functions. */
        void PushPreparedBox();
        void Push(const ScriptValue &, std::size_t = 0);
        bool PushTyped(const ScriptValue &);
        void PushCollection(const ScriptValue &, std::size_t);
        void PrepareOutcome(const ScriptCallResult &);
    };

    int CollectBox(lua_State *);
    int BoxName(lua_State *);
}  // namespace Horo::Extensions::LuaModuleDetail
