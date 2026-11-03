#pragma once
#include "ScriptModuleHostTestSupport.h"

namespace Horo::Extensions::Tests::ProductRecoverySupport {
    using namespace ModuleHostSupport;
    inline constexpr ScriptModuleRuntimeKind Vms[]{ScriptModuleRuntimeKind::Lua54, ScriptModuleRuntimeKind::JavaScriptNg};

    inline std::string Source(ScriptModuleRuntimeKind runtime, bool expectUpdate = true) {
        if (runtime == ScriptModuleRuntimeKind::Lua54)
            return std::string{R"(local r=horo.import("horo.reference").horo.reference
local held=r.sum;local localValue=41
r.digest("abc")
)"} +
                   (expectUpdate
                        ? R"(if horo.import("horo.reference").horo.reference.apirevision().values[1]~="1.1.0" then error("candidate not published") end
)"
                        : R"(if horo.import("horo.reference").horo.reference.apirevision~=nil then error("partial batch published") end
)") + "return held(localValue)";
        return std::string{R"(const r=horo.import("horo.reference").horo.reference;
const held=r.sum;const localValue=41n;
await r.digest([97,98,99]);
)"} +
               (expectUpdate
                    ? R"(if(horo.import("horo.reference").horo.reference.apirevision().values[0]!=="1.1.0")throw new Error("candidate not published");
)"
                    : R"(if(horo.import("horo.reference").horo.reference.apirevision!==undefined)throw new Error("partial batch published");
)") + "return held(localValue);";
    }

    inline void Answer42(const ScriptCallResult &result) {
        INFO((result.error ? result.error->message : "ok"));
        REQUIRE(result.IsSuccess());
        REQUIRE(result.values.size() == 1);
        REQUIRE(result.values.front().AsSignedInteger());
        CHECK(*result.values.front().AsSignedInteger() == 42);
    }

    inline void Compatible(const ScriptModuleUpdateReport &report) {
        CHECK(report.kind == ScriptModuleUpdateKind::Compatible);
        CHECK_FALSE(report.diagnostic);
        REQUIRE(report.previousFingerprints.size() == report.candidateFingerprints.size());
        CHECK(report.previousFingerprints != report.candidateFingerprints);
    }
}  // namespace Horo::Extensions::Tests::ProductRecoverySupport
