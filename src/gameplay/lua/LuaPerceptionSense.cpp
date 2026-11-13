#include "Horo/Gameplay/LuaPerceptionSense.h"

#include "LuaBehaviorSandbox.h"
#include "LuaDecisionNodeBoundary.h"

#include <initializer_list>
#include <limits>
#include <new>
#include <stdexcept>

namespace Horo::Gameplay {
    namespace {
        using Detail::DecisionScript::Protected;

        /** @brief Keeps failure translation inside its boundary without allocating a replacement diagnostic. */
        Error BoundaryFailure(const ErrorCodeDescriptor &descriptor) noexcept {
            return {.code = descriptor.code, .domain = descriptor.domain, .severity = descriptor.defaultSeverity};
        }

        /** @brief Existing allocator accounting plus owner-only instruction and no-allocation callback fences. */
        struct VmBudget final {
            Detail::LuaBudget memory;
            std::uint32_t remaining{};
            bool frozen{};
        };

        /** @brief Forbids native allocation inside Evaluate while retaining the existing bounded Lua allocator at activation. */
        void *Allocate(void *raw, void *pointer, const std::size_t oldSize, const std::size_t newSize) {
            auto &budget = *static_cast<VmBudget *>(raw);
            if (budget.frozen && newSize != 0)
                return nullptr;
            return Detail::BudgetAllocate(&budget.memory, pointer, oldSize, newSize);
        }

        /** @brief Applies a single instruction allowance across all candidate predicate invocations. */
        void InstructionHook(lua_State *state, lua_Debug *) {
            void *raw{};
            static_cast<void>(lua_getallocf(state, &raw));
            auto &budget = *static_cast<VmBudget *>(raw);
            if (budget.remaining == 0)
                luaL_error(state, "Perception instruction limit");
            --budget.remaining;
        }

        /** @brief Validates source and instruction ceilings before compiling untrusted script bytes. */
        bool ValidSource(const std::string &source, const LuaBehaviorLimits limits) noexcept {
            return !source.empty() && source.size() <= 256U * 1024U && source.find('\0') == std::string::npos &&
                   limits.maximumInstructionsPerCallback > 0 && limits.maximumInstructionsPerCallback <= 100'000;
        }

        /** @brief Bounds VM storage and per-candidate cooperative admission before native instance creation. */
        bool ValidPolicy(const AI::PerceptionSenseContract &contract, const LuaBehaviorLimits limits,
                         const std::size_t nativeBytes) noexcept {
            return limits.maximumMemoryBytes >= 16U * 1024U &&
                   limits.maximumMemoryBytes <= AI::PerceptionSenseGeneration::MaximumInstanceBytes - nativeBytes &&
                   contract.sense.origin.kind == AI::PerceptionDescriptorSourceKind::Script &&
                   contract.workUnits >= contract.maximumCandidates;
        }

        /** @brief Removes native variable-work libraries and error-catching paths that bypass the scalar instruction tier. */
        void OpenScalarSandbox(lua_State *state) {
            Detail::OpenLuaBehaviorSandbox(state);
            for (const char *name : {"print",  "warn",   "load",   "string",   "table",  "utf8",         "horo",
                                     "pairs",  "ipairs", "next",   "pcall",    "xpcall", "getmetatable", "setmetatable",
                                     "rawget", "rawset", "rawlen", "rawequal", "select", "tostring",     "tonumber"}) {
                lua_pushnil(state);
                lua_setglobal(state, name);
            }
            // luaopen_string installs methods on every string, independently of the global library table.
            lua_pushliteral(state, "");
            lua_pushnil(state);
            lua_setmetatable(state, -2);
            lua_pop(state, 1);
            lua_getglobal(state, "math");
            for (const char *name : {"random", "randomseed"}) {
                lua_pushnil(state);
                lua_setfield(state, -2, name);
            }
            lua_pop(state, 1);
        }

        /** @brief One per-instance VM, with all source loading and stack reservation outside the sensing path. */
        class Vm final {
        public:
            explicit Vm(const LuaBehaviorLimits limits) : budget_{{0, limits.maximumMemoryBytes}, limits.maximumInstructionsPerCallback} {
                static_assert(std::numeric_limits<lua_Integer>::digits >= std::numeric_limits<std::int64_t>::digits);
                state_ = lua_newstate(Allocate, &budget_);
            }

            ~Vm() {
                if (state_)
                    lua_close(state_);
            }

            Vm(const Vm &) = delete;
            Vm &operator=(const Vm &) = delete;

            bool Load(const std::string &source, const SourceLocation &location) {
                if (!state_ || !lua_checkstack(state_, 512))
                    return false;
                lua_sethook(state_, InstructionHook, LUA_MASKCOUNT, 1);
                const bool loaded = Protected(state_, [this, &source, &location](lua_State *state) {
                    OpenScalarSandbox(state);
                    if (luaL_loadbufferx(state, source.data(), source.size(), location.source.c_str(), "t") != LUA_OK)
                        return lua_error(state);
                    lua_call(state, 0, 1);
                    if (!lua_isfunction(state, -1))
                        return luaL_error(state, "Expected perception predicate");
                    predicate_ = luaL_ref(state, LUA_REGISTRYINDEX);
                    return 0;
                });
                lua_settop(state_, 0);
                lua_gc(state_, LUA_GCSTOP);
                return loaded;
            }

            bool Evaluate(const AI::PerceptionSenseInput &input, std::span<AI::PerceptionSenseSample> output, AI::AiWorkBudget &work,
                          const std::uint32_t instructions, std::size_t &count) {
                budget_.remaining = instructions;
                budget_.frozen = true;
                const bool called = Protected(state_, [this, &input, output, &work, &count](lua_State *state) {
                    const auto listener = input.position.Millimeters();
                    for (std::size_t index = 0; index < input.candidates.size(); ++index) {
                        if (!work.Consume())
                            return luaL_error(state, "Perception work limit");
                        lua_rawgeti(state, LUA_REGISTRYINDEX, predicate_);
                        for (const auto value : input.candidates[index].position.Millimeters())
                            lua_pushinteger(state, static_cast<lua_Integer>(value));
                        for (const auto value : listener)
                            lua_pushinteger(state, static_cast<lua_Integer>(value));
                        lua_call(state, 6, 1);
                        if (lua_isboolean(state, -1))
                            output[count++] = {static_cast<std::uint32_t>(index), static_cast<bool>(lua_toboolean(state, -1))};
                        else if (!lua_isnil(state, -1))
                            return luaL_error(state, "Expected boolean or nil sensing result");
                        lua_pop(state, 1);
                    }
                    return 0;
                });
                budget_.frozen = false;
                lua_settop(state_, 0);
                return called;
            }

        private:
            VmBudget budget_;
            lua_State *state_{};
            int predicate_{LUA_NOREF};
        };
    }  // namespace

    struct LuaPerceptionSenseProgram::Image final {
        std::string source;
        AI::PerceptionSenseContract contract;
        LuaBehaviorLimits limits;
        std::shared_ptr<const AI::PerceptionSenseGeneration> parent;
        std::atomic_bool admission{true};
        std::thread::id owner{std::this_thread::get_id()};

        bool CanAdmit() const noexcept {
            return owner == std::this_thread::get_id() && admission.load(std::memory_order_acquire) && parent->CanAdmit();
        }

        static AI::IPerceptionSenseProvider *Create(void *raw);

        static void Destroy(void *, AI::IPerceptionSenseProvider *instance) noexcept {
            delete instance;
        }
    };

    class LuaPerceptionSenseProgram::Instance final : public AI::IPerceptionSenseProvider {
    public:
        explicit Instance(const Image &image) : vm_(image.limits), instructions_(image.limits.maximumInstructionsPerCallback) {
            if (!vm_.Load(image.source, image.contract.source))
                throw std::invalid_argument("Unable to activate perception predicate");
        }

        Result<std::size_t> Evaluate(const AI::PerceptionSenseInput &input, std::span<AI::PerceptionSenseSample> output,
                                     AI::AiWorkBudget &budget) override {
            std::size_t count{};
            if (!vm_.Evaluate(input, output, budget, instructions_, count))
                throw std::runtime_error("Perception predicate failed its enforced boundary");
            return Result<std::size_t>::Success(count);
        }

    private:
        Vm vm_;
        std::uint32_t instructions_{};
    };

    AI::IPerceptionSenseProvider *LuaPerceptionSenseProgram::Image::Create(void *raw) {
        return new Instance{*static_cast<Image *>(raw)};
    }

    /** @copydoc LuaPerceptionSenseProgram::Compile */
    Result<std::unique_ptr<LuaPerceptionSenseProgram>> LuaPerceptionSenseProgram::Compile(
        std::string source, AI::PerceptionSenseContract contract, std::shared_ptr<const AI::PerceptionSenseGeneration> parent,
        const LuaBehaviorLimits limits) {
        using Output = Result<std::unique_ptr<LuaPerceptionSenseProgram>>;
        if (!parent || !parent->CanAdmit())
            return Output::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        if (!ValidSource(source, limits) || !ValidPolicy(contract, limits, sizeof(Instance)))
            return Output::Failure(MakeError(AI::AIErrors::PerceptionDescriptorInvalid));
        try {
            auto image = std::make_shared<Image>();
            image->source = std::move(source);
            image->contract = std::move(contract);
            image->contract.instanceBytes = sizeof(Instance) + limits.maximumMemoryBytes;
            image->limits = limits;
            image->parent = std::move(parent);
            auto program = std::unique_ptr<LuaPerceptionSenseProgram>{new LuaPerceptionSenseProgram{image}};
            if (auto captured = program->Acquire(); captured.HasError())
                return Output::Failure(captured.ErrorValue());
            Vm compiler(limits);
            if (!compiler.Load(image->source, image->contract.source))
                return Output::Failure(MakeError(AI::AIErrors::PerceptionProviderFailed));
            if (!image->CanAdmit())
                return Output::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
            return Output::Success(std::move(program));
        } catch (const std::bad_alloc &) {
            return Output::Failure(BoundaryFailure(AI::AIErrors::PerceptionRegistryStorageUnavailable));
        } catch (...) {
            return Output::Failure(BoundaryFailure(AI::AIErrors::PerceptionProviderFailed));
        }
    }

    /** @copydoc LuaPerceptionSenseProgram::~LuaPerceptionSenseProgram */
    LuaPerceptionSenseProgram::~LuaPerceptionSenseProgram() {
        Retire();
    }

    /** @copydoc LuaPerceptionSenseProgram::Retire */
    void LuaPerceptionSenseProgram::Retire() noexcept {
        image_->admission.store(false, std::memory_order_release);
    }

    /** @copydoc LuaPerceptionSenseProgram::Acquire */
    Result<std::shared_ptr<const AI::PerceptionSenseGeneration>> LuaPerceptionSenseProgram::Acquire() const {
        if (!image_->CanAdmit())
            return Result<std::shared_ptr<const AI::PerceptionSenseGeneration>>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        try {
            const AI::PerceptionSenseRegistration registration{image_->contract, {image_.get(), Image::Create, Image::Destroy}};
            return AI::PerceptionSenseGeneration::CaptureVm(std::span{&registration, 1},
                                                            std::shared_ptr<const std::atomic_bool>{image_, &image_->admission},
                                                            image_->parent);
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<const AI::PerceptionSenseGeneration>>::Failure(
                BoundaryFailure(AI::AIErrors::PerceptionRegistryStorageUnavailable));
        }
    }
}  // namespace Horo::Gameplay
