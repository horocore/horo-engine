#pragma once

/** @file GuiLiveReload.h
 * @brief Typed synchronous dispatch for host-owned GUI state and replaceable development drawing.
 */
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>

namespace Horo::Editor::GuiLive {
    /** @brief Stable development route or signature identity; no process-local type_info addresses. */
    struct Id {
        std::uint64_t value{};

        constexpr explicit Id(std::string_view text) : value(14695981039346656037ULL) {
            for (const unsigned char c : text)
                value = (value ^ c) * 1099511628211ULL;
        }

        constexpr bool operator==(const Id &) const = default;
    };

    /** @brief Synchronous thunk, valid only until the next owner-thread module publication. */
    struct Binding {
        void (*invoke)(void *){};
    };

    /** @brief Borrowed GUI-owner-thread service. Never retain bindings or call them from jobs. */
    class IRegistry {
    public:
        virtual ~IRegistry() = default;
        /** @brief Finds an exact route/signature pair without allocation or I/O.
         * @param route Stable registered drawing entry.
         * @param signature Compile-time function-signature fingerprint, including argument qualifiers.
         * @return Empty on disabled, absent or incompatible routes; callers keep their compiled drawing.
         */
        [[nodiscard]] virtual Binding Find(Id route, Id signature) const noexcept = 0;
    };

    namespace Detail {
        template <typename Signature> constexpr Id SignatureId() {
#if defined(_MSC_VER)
            return Id{__FUNCSIG__};
#else
            return Id{__PRETTY_FUNCTION__};
#endif
        }
        template <typename Signature> struct Frame;

        template <typename R, typename... Args> struct Frame<R(Args...)> {
            static_assert(!std::is_reference_v<R>, "Live GUI returns must be values or void");
            std::tuple<Args...> arguments;
            std::optional<R> result;
        };

        template <typename... Args> struct Frame<void(Args...)> {
            std::tuple<Args...> arguments;
        };
        template <typename Method> struct MethodSignature;

        template <typename R, typename C, typename... Args> struct MethodSignature<R (C::*)(Args...)> {
            using Type = R(C &, Args...);
        };

        template <typename R, typename C, typename... Args> struct MethodSignature<R (C::*)(Args...) const> {
            using Type = R(const C &, Args...);
        };
    }  // namespace Detail

    namespace Detail {
        template <auto *Address>
#if defined(HORO_GUI_RUNTIME_COMPILATION) && defined(_MSC_VER)
        __declspec(dllimport)
#elif defined(__APPLE__)
        __attribute__((visibility("default"), noinline))
#if !defined(HORO_GUI_RUNTIME_COMPILATION)
        __attribute__((used))
#endif
#endif
        decltype(Address) HostAddress() noexcept
#if defined(HORO_GUI_RUNTIME_COMPILATION)
            ;
#else
        {
            return Address;
        }
#endif
    }  // namespace Detail

    /** @brief Uses host-owned immutable metadata or an inline API without copying its globals.
     * @tparam Address Pointer to an immutable constant or function owned by the host.
     * @return The original host reference, valid for the host's lifetime.
     */
    template <auto *Address> [[nodiscard]] decltype(auto) HostSymbol() noexcept {
        return *Detail::HostAddress<Address>();
    }

    /** @brief Function type for a member entry, with its borrowed owner as the first argument. */
    template <auto Method> using MethodSignature = typename Detail::MethodSignature<decltype(Method)>::Type;

    /** @brief Build-local identity of a registered function/member; never uses its loaded address. */
    template <auto Entry> constexpr Id EntryId() {
#if defined(_MSC_VER)
        return Id{__FUNCSIG__};
#else
        return Id{__PRETTY_FUNCTION__};
#endif
    }

    /** @brief Concrete host type identity for virtual drawing, independent of DLL RTTI addresses.
     * @param type Static registration type or the dynamic type of a borrowed host instance.
     * @return Compiler-local type name fingerprint; requires the same compiler and header ABI.
     */
    [[nodiscard]] inline Id TypeId(const std::type_info &type) noexcept {
#if defined(_MSC_VER)
        // raw_name avoids MSVC's lazily allocated demangled name on the frame path.
        return Id{type.raw_name()};
#else
        return Id{type.name()};
#endif
    }

    /** @brief Exact typed draw call. Stack frames and referenced state never outlive operator(). */
    template <typename Signature> class Function;

    template <typename R, typename... Args> class Function<R(Args...)> {
    public:
        explicit Function(Binding binding = {}) : binding_(binding) {}

        explicit operator bool() const noexcept {
            return binding_.invoke != nullptr;
        }

        /** @brief Calls an already-resolved binding synchronously; requires a nonempty function. */
        R operator()(Args... arguments) const {
            Detail::Frame<R(Args...)> frame{std::tuple<Args...>{std::forward<Args>(arguments)...}};
            binding_.invoke(&frame);
            if constexpr (!std::is_void_v<R>)
                return std::move(*frame.result);
        }

    private:
        Binding binding_;
    };

    /** @brief Finds a typed entry, including calls from a replacement to another registered component.
     * @param registry Borrowed host service, or null for normal/static drawing.
     * @param route Stable identity shared with the build registration.
     * @return A synchronous typed callable, empty when the compiled fallback should run.
     */
    template <typename Signature> [[nodiscard]] Function<Signature> Find(const IRegistry *registry, Id route) noexcept {
        return Function<Signature>{registry ? registry->Find(route, Detail::SignatureId<Signature>()) : Binding{}};
    }

    namespace Detail {
        template <auto Entry, bool = std::is_member_function_pointer_v<decltype(Entry)>> struct EntrySignature {
            using Type = std::remove_pointer_t<decltype(Entry)>;
        };

        template <auto Entry> struct EntrySignature<Entry, true> {
            using Type = GuiLive::MethodSignature<Entry>;
        };

        template <typename Signature> struct Call;

        template <typename R, typename... Args> struct Call<R(Args...)> {
            template <auto Entry> static R Invoke(const IRegistry *registry, Id route, Args... arguments) {
                if (const auto live = Find<R(Args...)>(registry, route))
                    return live(std::forward<Args>(arguments)...);
                return std::invoke(Entry, std::forward<Args>(arguments)...);
            }
        };
    }  // namespace Detail

    /** @brief Calls a registered free/concrete member entry, or its ordinary compiled fallback.
     * @tparam Entry Original drawing function; also supplies the exact signature and registration identity.
     * @param registry Borrowed owner-thread registry, or null in ordinary builds.
     * @param arguments Original function arguments; include the borrowed object first for a member.
     * @return Original void/value result. No binding or argument is retained.
     */
    template <auto Entry, typename... Args> decltype(auto) Invoke(const IRegistry *registry, Args &&...arguments) {
        return Detail::Call<typename Detail::EntrySignature<Entry>::Type>::template Invoke<Entry>(registry, EntryId<Entry>(),
                                                                                                  std::forward<Args>(arguments)...);
    }

    /** @brief Dispatches a base-interface draw by its concrete host type, falling back to normal virtual drawing.
     * @tparam Entry Base-interface member function registered with BASE in CMake.
     * @param registry Borrowed owner-thread registry, or null in ordinary/headless hosts.
     * @param owner Borrowed polymorphic host object. Its state and virtual table stay host-owned.
     * @param arguments Original drawing arguments.
     * @return Original void/value result. Generated replacements bypass the original virtual table.
     */
    template <auto Entry, typename Owner, typename... Args>
    decltype(auto) InvokeVirtual(const IRegistry *registry, Owner &owner, Args &&...arguments) {
        static_assert(std::is_polymorphic_v<Owner>);
        return Detail::Call<MethodSignature<Entry>>::template Invoke<Entry>(registry, registry ? TypeId(typeid(owner)) : Id{""}, owner,
                                                                            std::forward<Args>(arguments)...);
    }

    /** @brief Constructs a type-checked module thunk; supports void and value results. */
    template <typename Signature, auto Callable> struct Adapter;

    template <typename R, typename... Args, auto Callable> struct Adapter<R(Args...), Callable> {
        static void Invoke(void *erased) {
            auto &frame = *static_cast<Detail::Frame<R(Args...)> *>(erased);
            if constexpr (std::is_void_v<R>)
                std::apply(Callable, frame.arguments);
            else
                frame.result.emplace(std::apply(Callable, frame.arguments));
        }

        static Binding Bind(Id signature) noexcept {
            return signature == Detail::SignatureId<R(Args...)>() ? Binding{&Invoke} : Binding{};
        }
    };
}  // namespace Horo::Editor::GuiLive
