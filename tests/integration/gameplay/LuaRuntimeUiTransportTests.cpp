#include "Horo/Gameplay/GameplayNavigationContext.h"
#include "LuaNavigationBinding.h"
#include "LuaRuntimeUiBinding.h"
#include "LuaRuntimeUiProtected.h"
#include "RuntimeUiCapabilityTestSupport.h"

#include <cstdlib>
#include <limits>
#include <string_view>
extern "C" {
#include <lualib.h>
}

namespace Horo::Tests::UiAccess {
    namespace {
        /** @brief Exercises a nonstandard native failure with an owner that must unwind before Lua error propagation. */
        int ThrowNative(lua_State *state) {
            struct Unwind final {
                int &count;

                ~Unwind() {
                    ++count;
                }
            } unwind{*static_cast<int *>(lua_touserdata(state, lua_upvalueindex(1)))};

            throw 42;
        }

        /** @brief Exercises typed capacity projection and its protected allocation-failure fallback. */
        int ThrowMemory(lua_State *) {
            throw std::bad_alloc{};
        }

        /** @brief Fails every growth from one exact allocation onward, including Lua's emergency-GC retry. */
        struct Fault final {
            std::size_t attempts{};
            std::size_t firstFailure{std::numeric_limits<std::size_t>::max()};
            std::size_t failures{};
            bool enabled{};
        };

        void *Allocate(void *context, void *memory, const std::size_t oldSize, const std::size_t size) {
            auto &fault = *static_cast<Fault *>(context);
            if (!size) {
                std::free(memory);
                return nullptr;
            }
            if (fault.enabled && (!memory || size > oldSize) && ++fault.attempts >= fault.firstFailure) {
                ++fault.failures;
                return nullptr;
            }
            return std::realloc(memory, size);
        }

        /** @brief Owns the real Lua VM outside every protected marshalling frame. */
        struct Vm final {
            Fault fault;
            CancellationSource navigationLifetime;
            CancellationToken navigationToken{navigationLifetime.Token()};
            lua_State *state{lua_newstate(Allocate, &fault)};

            Vm() {
                REQUIRE(state != nullptr);
                luaL_openlibs(state);
            }

            ~Vm() {
                fault.enabled = false;
                navigationLifetime.RequestCancellation();
                lua_close(state);
            }

            Vm(const Vm &) = delete;
            Vm &operator=(const Vm &) = delete;

            void Context(const std::shared_ptr<const RuntimeUiClient> &client,
                         const std::shared_ptr<const Gameplay::GameplayNavigationContext> &navigation = {}) const {
                Gameplay::Detail::UiScript::Protected(state, [&](lua_State *inner) {
                    Gameplay::Detail::PushLuaNavigationContext(inner, navigation, navigationToken);
                    lua_setglobal(inner, "navigation");
                    Gameplay::Detail::PushLuaRuntimeUi(inner, client);
                    lua_setglobal(inner, "ui");
                    return 0;
                });
            }

            void Load(const std::string &source) const {
                REQUIRE(luaL_loadstring(state, source.c_str()) == LUA_OK);
                REQUIRE(lua_pcall(state, 0, 1, 0) == LUA_OK);
                REQUIRE(lua_isfunction(state, -1));
            }
        };

        /** @brief Runs a precompiled Lua closure and observes its complete result without a native owner inside Lua's jump frame. */
        int Invoke(Vm &vm) {
            return lua_pcall(vm.state, 0, LUA_MULTRET, 0);
        }

        enum class Phase {
            Context,
            Read,
            DecodeAndQueue,
            Terminal,
            Event,
            TerminalProjection,
            EventProjection
        };

        struct Observation final {
            std::size_t attempts{};
            std::size_t failures{};
            bool failed{};
            std::size_t queued{};
        };

        /** @brief Prepare the actual canonical phase with faults disabled; every owner remains in the caller's VM scope. */
        void PreparePhase(const Phase phase, Fixture &fixture, Vm &vm, const std::shared_ptr<const RuntimeUiClient> &client,
                          const std::shared_ptr<const Gameplay::GameplayNavigationContext> &navigation) {
            if (phase != Phase::Context)
                vm.Context(client, navigation);
            if (phase == Phase::Read)
                vm.Load("return function() return ui:read('0000000000000000000000000000001e') end");
            if (phase == Phase::DecodeAndQueue)
                vm.Load("local value=ui:binding_value(5,'changed'); local updates={{0,value}}; "
                        "return function() return ui:publish(1,1,0,'" +
                        std::to_string(fixture.schema.Version().fingerprint) + "','1','2',updates) end");
            if (phase == Phase::Terminal || phase == Phase::TerminalProjection) {
                vm.Load("request=ui:screen(0,'00000000000000000000000000000009'); return function() return ui:poll(request) end");
                REQUIRE(Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands)) == 1);
                if (phase == Phase::TerminalProjection)
                    vm.Load("terminal=ui:poll(request); return function() return terminal:unpack() end");
            }
            if (phase == Phase::Event || phase == Phase::EventProjection) {
                const auto target = Take(client->Find(Stable<UiElementId>(11)));
                const UiEventRoute route{fixture.actionOwner.instance,
                                         fixture.actionOwner.canvas,
                                         fixture.actionOwner.document,
                                         fixture.actionOwner.treeRevision,
                                         fixture.actionOwner.interaction,
                                         target,
                                         {}};
                Events handler;
                REQUIRE(fixture.service.DispatchEvent(Stable<UiCanvasId>(2), route, {UiEventKind::Submit, 1}, handler).HasValue());
                if (phase == Phase::EventProjection)
                    vm.Load("notification=ui:next_event(); return function() return notification:unpack() end");
                else
                    vm.Load("return function() return ui:next_event() end");
            }
        }

        /** @brief Prove failed projection preserves the native observation or supports safe same-VM context retry. */
        void VerifyRecovery(const Phase phase, Vm &vm, const std::shared_ptr<const RuntimeUiClient> &client,
                            const std::shared_ptr<const Gameplay::GameplayNavigationContext> &navigation) {
            if (phase == Phase::Terminal)
                REQUIRE(Take(client->Poll({1})).has_value());
            if (phase == Phase::Event)
                REQUIRE(Take(client->NextEvent()).has_value());
            if (phase == Phase::TerminalProjection) {
                vm.Load("return function() return terminal:unpack() end");
                REQUIRE(Invoke(vm) == LUA_OK);
            }
            if (phase == Phase::EventProjection) {
                vm.Load("return function() return notification:unpack() end");
                REQUIRE(Invoke(vm) == LUA_OK);
            }
            if (phase == Phase::Context)
                vm.Context(client, navigation);
        }

        /** @brief Exercises one real allocation offset, then closes the VM before releasing the final native client. */
        Observation Observe(const Phase phase, const std::size_t failAt) {
            Fixture fixture{false, true};
            auto anchor = fixture.Client();
            auto client = Take(anchor->Derive());
            auto navigation =
                Take(Gameplay::GameplayNavigationContext::Create({.moduleId = "game.ui-tests"}, nullptr, nullptr, nullptr, {}));
            CancellationToken retiredVmToken;
            Observation observed;
            {
                Vm vm;
                PreparePhase(phase, fixture, vm, client, navigation);
                retiredVmToken = vm.navigationToken;
                vm.fault.firstFailure = failAt;
                vm.fault.enabled = true;
                if (phase == Phase::Context) {
                    try {
                        vm.Context(client, navigation);
                    } catch (const Gameplay::Detail::UiScript::LuaTransportFailure &) {
                        observed.failed = true;
                    } catch (const std::bad_alloc &) {
                        observed.failed = true;
                    }
                } else {
                    const int status = Invoke(vm);
                    observed.failed = status != LUA_OK || lua_isnil(vm.state, 1);
                }
                vm.fault.enabled = false;
                observed.attempts = vm.fault.attempts;
                observed.failures = vm.fault.failures;
                observed.queued = Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::BeforeVariableUpdate));
                if (observed.failed)
                    VerifyRecovery(phase, vm, client, navigation);
            }
            CHECK(retiredVmToken.IsCancellationRequested());
            CHECK(navigation.use_count() == 1);
            CHECK(client.use_count() == 1);
            CHECK(client->Validate().HasValue());
            return observed;
        }
    }  // namespace

    TEST_CASE("Lua Runtime UI noexcept C boundary unwinds native owners and preserves typed capacity errors", "[runtime-ui][lua][oom]") {
        Vm vm;
        int unwound{};
        lua_pushlightuserdata(vm.state, &unwound);
        lua_pushcclosure(vm.state, Gameplay::Detail::UiScript::Boundary<ThrowNative>, 1);
        REQUIRE(lua_pcall(vm.state, 0, LUA_MULTRET, 0) == LUA_ERRRUN);
        CHECK(unwound == 1);
        CHECK(std::string_view{lua_tostring(vm.state, -1)} == "Runtime UI transport native failure.");
        lua_settop(vm.state, 0);

        lua_pushcfunction(vm.state, Gameplay::Detail::UiScript::Boundary<ThrowMemory>);
        REQUIRE(lua_pcall(vm.state, 0, LUA_MULTRET, 0) == LUA_OK);
        REQUIRE(lua_gettop(vm.state) == 2);
        CHECK(lua_isnil(vm.state, 1));
        lua_getfield(vm.state, 2, "code");
        CHECK(std::string_view{lua_tostring(vm.state, -1)} == UiErrors::CapacityExceeded.code.Value());
        lua_settop(vm.state, 0);

        lua_pushcfunction(vm.state, Gameplay::Detail::UiScript::Boundary<ThrowMemory>);
        vm.fault.firstFailure = 1;
        vm.fault.enabled = true;
        const int status = lua_pcall(vm.state, 0, LUA_MULTRET, 0);
        vm.fault.enabled = false;
        CHECK(status != LUA_OK);
        CHECK(vm.fault.failures > 0);
    }

    TEST_CASE("Lua Runtime UI protects every context and projection allocation without leaking native holders", "[runtime-ui][lua][oom]") {
        for (const auto phase : {Phase::Context, Phase::Read, Phase::DecodeAndQueue, Phase::Terminal, Phase::Event,
                                 Phase::TerminalProjection, Phase::EventProjection}) {
            const auto baseline = Observe(phase, std::numeric_limits<std::size_t>::max());
            REQUIRE_FALSE(baseline.failed);
            REQUIRE(baseline.attempts > 0);
            REQUIRE(baseline.attempts <= 256);
            for (std::size_t failure = 1; failure <= baseline.attempts; ++failure) {
                INFO(static_cast<int>(phase) << " allocation=" << failure);
                const auto observed = Observe(phase, failure);
                REQUIRE(observed.failures > 0);
                REQUIRE(observed.failed);
                CHECK(observed.queued == 0);
            }
        }
    }

    TEST_CASE("Lua Runtime UI rejects sparse and metamethod payloads before native admission", "[runtime-ui][lua][binding]") {
        Fixture fixture{false, true};
        auto client = fixture.Client();
        Vm vm;
        vm.Context(client);
        const std::string source =
            "return function() local value=ui:binding_value(5,'changed'); "
            "local payload=setmetatable({[1]={0,value},[3]={0,value}}, {__index=function() error('metamethod') end}); "
            "local request,err=ui:publish(1,1,0,'" +
            std::to_string(fixture.schema.Version().fingerprint) +
            "','1','2',payload); assert(request==nil and err.code=='runtime_ui.capability.unsupported'); return true end";
        vm.Load(source.c_str());
        REQUIRE(Invoke(vm) == LUA_OK);
        CHECK(Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::BeforeVariableUpdate)) == 0);
    }

    TEST_CASE("Lua Runtime UI closed values preserve scalar identities and complete typed action payloads", "[runtime-ui][lua][values]") {
        Fixture fixture;
        auto client = fixture.Client();
        Vm vm;
        vm.Context(client);
        vm.Load(R"(
            return function()
                local binding={
                    ui:binding_value(0),ui:binding_value(1,true),ui:binding_value(2,'-9223372036854775808'),
                    ui:binding_value(3,'18446744073709551615'),ui:binding_value(4,1.25),ui:binding_value(5,'text'),
                    ui:binding_value(6,'ui.message'),ui:binding_value(7,'18446744073709551615'),ui:binding_value(8,'7'),
                    ui:binding_value(9,1,2),ui:binding_value(10,1,2,3),ui:binding_value(11,1,0,0,1),ui:binding_value(12,0,'asset')}
                for i,value in ipairs(binding) do assert(value:unpack()==i-1) end
                local _,minimum=binding[3]:unpack(); assert(minimum=='-9223372036854775808')
                local _,maximum=binding[4]:unpack(); assert(maximum=='18446744073709551615')
                local invalid,err=ui:binding_value(9,1e300,0); assert(invalid==nil and err~=nil)
                local element=ui:find('0000000000000000000000000000000b')
                assert(element==ui:find('0000000000000000000000000000000b'))
                local source=ui:source(element,'1')
                local values={ui:action_value(0,true),ui:action_value(1,-7),ui:action_value(2,'18446744073709551615'),
                    ui:action_value(3,1.25),ui:action_value(4,'copied'),ui:action_value(5,'00000000000000000000000000000016'),
                    ui:action_value(6,'00000000000000000000000000000001'),ui:action_value(7,'00000000000000000000000000000002'),
                    ui:action_value(8,'0000000000000000000000000000000b')}
                for _,value in ipairs(values) do assert(ui:action(source,3,'00000000000000000000000000000016',0,{value})~=nil) end
            end
        )");
        REQUIRE(Invoke(vm) == LUA_OK);
        REQUIRE(Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands)) == 9);

        struct Handler final : UiActionHandler {
            std::size_t calls{};

            Result<UiActionResult> Handle(const UiActionRequest &request) override {
                const auto &payload = std::get<UiGameplayActionCommand>(request.command).payload;
                REQUIRE(payload.Size() == 1);
                CHECK(payload.Values()[0].index() == calls);
                if (calls == 2)
                    CHECK(std::get<std::uint64_t>(payload.Values()[0]) == std::numeric_limits<std::uint64_t>::max());
                ++calls;
                return UiActionResult::Handled(request.id);
            }
        } handler;

        CHECK(Take(fixture.service.DispatchActions(Instance(), Stable<UiCanvasId>(2), handler)) == 9);
        CHECK(handler.calls == 9);
    }

    TEST_CASE("Lua Runtime UI retains complete terminal and event observations for repeatable projection", "[runtime-ui][lua][transport]") {
        Fixture fixture;
        auto client = fixture.Client();
        Vm vm;
        vm.Context(client);
        vm.Load("request=ui:screen(0,'00000000000000000000000000000009'); return function() end");
        REQUIRE(Invoke(vm) == LUA_OK);
        REQUIRE(Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands)) == 1);
        vm.Load(R"(
            terminal=ui:poll(request)
            return function()
                local kind,outcome,rejection,revision,owner,sequence,operation,route=terminal:unpack()
                assert(kind==0 and outcome==0 and rejection==0 and revision=='2' and owner~='0' and sequence~='0' and operation==0 and route~=nil)
                assert(terminal:unpack()==0)
                local value,err=ui:poll(request); assert(value==nil and err~=nil)
            end
        )");
        REQUIRE(Invoke(vm) == LUA_OK);
        const auto target = Take(client->Find(Stable<UiElementId>(11)));
        const UiEventRoute route{fixture.actionOwner.instance,
                                 fixture.actionOwner.canvas,
                                 fixture.actionOwner.document,
                                 fixture.actionOwner.treeRevision,
                                 fixture.actionOwner.interaction,
                                 target,
                                 {}};
        Events events;
        REQUIRE(fixture.service.DispatchEvent(Stable<UiCanvasId>(2), route, {UiEventKind::Submit, 17}, events).HasValue());
        vm.Load(R"(
            local event=ui:next_event()
            return function()
                local target,kind,sequence,x,y,position,gesture=event:unpack()
                assert(target==ui:find('0000000000000000000000000000000b') and sequence=='17' and not position and gesture==nil)
                assert(event:unpack()==target)
                assert(ui:next_event()==nil)
            end
        )");
        REQUIRE(Invoke(vm) == LUA_OK);
    }
}  // namespace Horo::Tests::UiAccess
