#pragma once
#include "Horo/Extensions/ScriptModuleRuntime.h"
#include "quickjs.h"

#include <stdexcept>

namespace Horo::Extensions::JavaScriptModuleDetail {
    struct ConversionFailure final {
        Error error;
    };

    [[noreturn]] void InvalidValue(std::string_view reason);

    /** Owns an actual JS value, never a borrowed descriptor value. */
    class Value final {
    public:
        Value(JSContext *context, JSValue value) : context_(context), value_(value) {}

        ~Value() {
            JS_FreeValue(context_, value_);
        }

        Value(const Value &) = delete;
        Value &operator=(const Value &) = delete;

        Value(Value &&other) noexcept : context_(other.context_), value_(other.Release()) {}

        Value &operator=(Value &&other) noexcept {
            if (this != &other) {
                JS_FreeValue(context_, value_);
                context_ = other.context_;
                value_ = other.Release();
            }
            return *this;
        }

        JSValue Get() const noexcept {
            return value_;
        }

        JSValue Release() noexcept {
            auto result = value_;
            value_ = JS_UNDEFINED;
            return result;
        }

    private:
        JSContext *context_;
        JSValue value_;
    };

    /** Per-conversion budget; runtime interrupt/deadline remains active during every operation. */
    struct Budget final {
        const ScriptValueLimits &limits;
        const std::chrono::steady_clock::time_point *deadline{};
        std::size_t nodes{};
        std::size_t bytes{};
        void Charge(std::size_t depth, std::size_t payload = 0);
    };

    struct HandleBudget final {
        std::size_t maximumBytes{};
        std::size_t maximumValues{};
        std::size_t bytes{};
        std::size_t values{};
    };

    struct HandleBox final {
        ScriptHandle handle;
        std::shared_ptr<HandleBudget> budget;
        std::size_t cost{};
        ~HandleBox();
    };

    void FinalizeHandle(JSRuntime *, JSValue) noexcept;

    struct OutcomeBox final {
        std::vector<std::byte> encoded;
        std::shared_ptr<HandleBudget> budget;
        std::size_t cost{};
        ~OutcomeBox();
    };

    void FinalizeOutcome(JSRuntime *, JSValue) noexcept;

    struct Values final {
        JSContext *context{};
        JSClassID objectClass{};
        JSClassID arrayClass{};
        JSClassID handleClass{};
        JSClassID outcomeClass{};
        std::shared_ptr<ScriptModuleContext> authority;
        std::shared_ptr<HandleBudget> handles;
        const std::chrono::steady_clock::time_point *deadline{};
        [[nodiscard]] JSValue WriteHandle(const ScriptHandle &);
        [[nodiscard]] JSValue WriteOutcome(ScriptCallResult);
        [[nodiscard]] ScriptCallResult ReadOutcome(JSValueConst) const;
        [[nodiscard]] ScriptValue Read(JSValueConst, const ScriptExportTypeReference &, const ScriptExportDescriptor &, Budget &,
                                       std::size_t = 0);
        [[nodiscard]] JSValue Write(const ScriptValue &, Budget &, std::size_t = 0);
        /** Checks exact nonproxy class before any property operation. */
        void Plain(JSValueConst, bool array) const;
        [[nodiscard]] Value Own(JSValueConst, JSAtom) const;
        [[nodiscard]] std::vector<std::string> Keys(JSValueConst, Budget &) const;
        [[nodiscard]] std::size_t Length(JSValueConst, Budget &) const;
        [[nodiscard]] ScriptValue ReadPrimitive(JSValueConst, ScriptExportPrimitiveKind, Budget &, std::size_t);
        [[nodiscard]] ScriptValue ReadNamed(JSValueConst, const ScriptExportTypeDescriptor &, const ScriptExportDescriptor &, Budget &,
                                            std::size_t);
        [[nodiscard]] ScriptValue ReadArray(JSValueConst, const ScriptExportTypeDescriptor &, const ScriptExportDescriptor &, Budget &,
                                            std::size_t);
        [[nodiscard]] ScriptValue ReadMap(JSValueConst, const ScriptExportTypeDescriptor &, const ScriptExportDescriptor &, Budget &,
                                          std::size_t);
        [[nodiscard]] ScriptValue ReadStruct(JSValueConst, const ScriptExportTypeDescriptor &, const ScriptExportDescriptor &, Budget &,
                                             std::size_t);
        [[nodiscard]] JSValue WriteCollection(const ScriptValue &, Budget &, std::size_t);
    };

    [[nodiscard]] std::string PrimitiveString(JSContext *, JSValueConst, std::size_t maximum);
    [[nodiscard]] std::int64_t Signed(JSContext *, JSValueConst);
    [[nodiscard]] std::uint64_t Unsigned(JSContext *, JSValueConst);
    [[nodiscard]] JSValue Checked(JSValue);
}  // namespace Horo::Extensions::JavaScriptModuleDetail
