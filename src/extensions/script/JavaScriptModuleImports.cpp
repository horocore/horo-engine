#include "JavaScriptModuleInternal.h"

#include <algorithm>

namespace Horo::Extensions::JavaScriptModuleDetail {
    namespace {
        State &Owner(JSContext *context) {
            auto *state = static_cast<State *>(JS_GetContextOpaque(context));
            if (!state)
                InvalidValue("Module owner is absent.");
            state->RequireLive();
            return *state;
        }

        void Define(JSContext *context, JSValueConst object, const std::string &name, JSValue value) {
            if (JS_DefinePropertyValueStr(context, object, name.c_str(), value, JS_PROP_ENUMERABLE) < 0)
                InvalidValue("Module namespace allocation failed.");
        }

        Value Object(JSContext *context) {
            return {context, Checked(JS_NewObjectProto(context, JS_NULL))};
        }

        Value NamespacePath(State &state, JSValueConst object, std::span<const std::string> segments) {
            Value node{state.context, JS_DupValue(state.context, object)};
            for (const auto &name : segments) {
                auto child = Object(state.context);
                Define(state.context, node.Get(), name, JS_DupValue(state.context, child.Get()));
                node = std::move(child);
            }
            return node;
        }

        void Constants(State &state, JSValueConst object, const ScriptExportDescriptor &api, const ScriptModuleBindingPlan &plan) {
            Budget budget{state.authority->Limits().values, &state.deadline};
            for (const auto index : plan.constantIndices) {
                const auto &constant = api.constants.at(index);
                auto decoded = DecodeScriptValue(constant.canonicalValue, state.authority->Limits().values);
                if (decoded.HasError())
                    throw ConversionFailure{decoded.ErrorValue()};
                Define(state.context, object, constant.id, state.values.Write(decoded.Value(), budget));
            }
        }

        void Methods(State &state, JSValueConst object, const ScriptRuntimeBackendBinding &backend, std::size_t imported) {
            const auto &methods = backend.Descriptor().functions;
            for (const auto index : state.plans[imported]->functionIndices) {
                const auto &method = methods.at(index);
                JSValue data[]{JS_NewInt32(state.context, static_cast<std::int32_t>(imported)),
                               JS_NewInt32(state.context, static_cast<std::int32_t>(index))};
                auto function = Checked(JS_NewCFunctionData(state.context, Invoke, static_cast<int>(method.parameters.size()), 0, 2, data));
                Define(state.context, object, method.id, function);
            }
        }

        JSValue Missing(State &state, const ScriptModuleImport &imported) {
            auto object = Object(state.context);
            Define(state.context, object.Get(), "available", JS_FALSE);
            const auto &error = *imported.Unavailable();
            Define(state.context, object.Get(), "code", Checked(JS_NewString(state.context, error.code.Value().c_str())));
            if (JS_FreezeObject(state.context, object.Get()) < 0)
                InvalidValue("Optional import sealing failed.");
            return object.Release();
        }
    }  // namespace

    JSValue State::Namespace(std::size_t index) {
        const auto &imported = authority->Imports()[index];
        if (!imported.Backend())
            return Missing(*this, imported);
        auto object = Object(context);
        Define(context, object.Get(), "available", JS_TRUE);
        const auto &api = imported.Backend()->Descriptor();
        auto leaf = NamespacePath(*this, object.Get(), plans[index]->namespaceSegments);
        Constants(*this, leaf.Get(), api, *plans[index]);
        Methods(*this, leaf.Get(), *imported.Backend(), index);
        // Every container is owner-built before user evaluation. Read-only properties prevent replacement.
        if (JS_FreezeObject(context, leaf.Get()) < 0 || JS_FreezeObject(context, object.Get()) < 0)
            InvalidValue("Import sealing failed.");
        return object.Release();
    }

    void State::BindImports() {
        imports.reserve(authority->Imports().size());
        for (std::size_t i = 0; i < authority->Imports().size(); ++i)
            imports.push_back(Namespace(i));
        auto object = Object(context);
        Define(context, object.Get(), "import", Checked(JS_NewCFunction(context, Import, "import", 1)));
        if (JS_FreezeObject(context, object.Get()) < 0)
            InvalidValue("Host namespace sealing failed.");
        horo = object.Release();
    }

    JSValue Import(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
        try {
            auto &state = Owner(context);
            if (argc != 1 || !JS_IsString(argv[0]))
                InvalidValue("Import requires one declared alias string.");
            const auto alias = PrimitiveString(context, argv[0], 256);
            const auto imports = state.authority->Imports();
            const auto found = std::ranges::find(imports, alias, &ScriptModuleImport::Alias);
            if (found == imports.end())
                InvalidValue("Module alias was not declared by the actual module import plan.");
            return JS_DupValue(context, state.imports[static_cast<std::size_t>(found - imports.begin())]);
        } catch (const ConversionFailure &error) {
            auto failure = StructuredFailure(context, error.error);
            return JS_IsException(failure) ? failure : JS_Throw(context, failure);
        } catch (...) {
            return Failure(context);
        }
    }

    JSValue Invoke(JSContext *context, JSValueConst, int argc, JSValueConst *argv, int, JSValueConst *data) {
        try {
            auto &state = Owner(context);
            std::int32_t imported{}, method{};
            if (JS_ToInt32(context, &imported, data[0]) < 0 || JS_ToInt32(context, &method, data[1]) < 0 || imported < 0 || method < 0)
                InvalidValue("Host function identity is invalid.");
            return state.Call(static_cast<std::size_t>(imported), static_cast<std::size_t>(method), argc, argv);
        } catch (const ConversionFailure &error) {
            return StructuredFailure(context, error.error);
        } catch (...) {
            return Failure(context);
        }
    }
}  // namespace Horo::Extensions::JavaScriptModuleDetail
