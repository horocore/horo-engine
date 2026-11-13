#include "JavaScriptModuleInternal.h"

#include <algorithm>
#include <utility>

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

        void Constants(State &state, Values &projector, JSValueConst object, const ScriptExportDescriptor &api,
                       const ScriptModuleBindingPlan &plan) {
            Budget budget{projector.authority->Limits().values, &state.deadline};
            for (const auto index : plan.constantIndices) {
                const auto &constant = api.constants.at(index);
                auto decoded = DecodeScriptValue(constant.canonicalValue, projector.authority->Limits().values);
                if (decoded.HasError())
                    throw ConversionFailure{decoded.ErrorValue()};
                Define(state.context, object, constant.id, projector.Write(decoded.Value(), budget));
            }
        }

        void Methods(State &state, JSValueConst object, const ScriptModuleImport &imported, const ScriptModuleBindingPlan &plan) {
            const auto &methods = imported.Backend()->Descriptor().functions;
            for (const auto index : plan.functionIndices) {
                const auto &method = methods.at(index);
                const auto &alias = imported.Alias();
                Value aliasValue{state.context, Checked(JS_NewStringLen(state.context, alias.data(), alias.size()))};
                Value methodValue{state.context, Checked(JS_NewStringLen(state.context, method.id.data(), method.id.size()))};
                JSValue data[]{aliasValue.Get(), methodValue.Get()};
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

        JSValue BuildNamespace(State &state, Values &projector, const ScriptModuleImport &imported,
                               const std::shared_ptr<const ScriptModuleBindingPlan> &plan) {
            if (!imported.Backend())
                return Missing(state, imported);
            auto object = Object(state.context);
            Define(state.context, object.Get(), "available", JS_TRUE);
            auto leaf = NamespacePath(state, object.Get(), plan->namespaceSegments);
            Constants(state, projector, leaf.Get(), imported.Backend()->Descriptor(), *plan);
            Methods(state, leaf.Get(), imported, *plan);
            if (JS_FreezeObject(state.context, leaf.Get()) < 0 || JS_FreezeObject(state.context, object.Get()) < 0)
                InvalidValue("Import sealing failed.");
            return object.Release();
        }

        struct NamespaceOwner final {
            JSContext *context;
            std::vector<JSValue> values;

            explicit NamespaceOwner(JSContext *value) : context(value) {}

            NamespaceOwner(const NamespaceOwner &) = delete;
            NamespaceOwner &operator=(const NamespaceOwner &) = delete;

            ~NamespaceOwner() {
                for (auto value : values)
                    JS_FreeValue(context, value);
            }
        };
    }  // namespace

    std::vector<JSValue> PrepareJavaScriptNamespaces(State &state, const std::shared_ptr<ScriptModuleContext> &context,
                                                     const std::vector<std::shared_ptr<const ScriptModuleBindingPlan>> &plans) {
        NamespaceOwner output{state.context};
        output.values.reserve(context->Imports().size());
        auto projector = state.values;
        projector.authority = context;
        for (std::size_t index = 0; index < context->Imports().size(); ++index)
            output.values.push_back(BuildNamespace(state, projector, context->Imports()[index], plans[index]));
        return std::exchange(output.values, {});
    }

    void State::BindImports() {
        imports = PrepareJavaScriptNamespaces(*this, authority, plans);
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
            const auto imported = PrimitiveString(context, data[0], 256);
            const auto method = PrimitiveString(context, data[1], 256);
            return state.Call(imported, method, argc, argv);
        } catch (const ConversionFailure &error) {
            return StructuredFailure(context, error.error);
        } catch (...) {
            return Failure(context);
        }
    }
}  // namespace Horo::Extensions::JavaScriptModuleDetail
