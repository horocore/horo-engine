#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/GuiLiveReload.h"

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <string>

namespace {
    using namespace Horo::Editor;

    struct State {
        int clicks{};
        std::string search{"kept"};
    };

    int Draw(State &state, const int step) {
        return state.clicks += step;
    }

    void Clear(State &state) {
        state.clicks = 0;
    }

    struct Registry : GuiLive::IRegistry {
        bool incompatible{};

        GuiLive::Binding Find(GuiLive::Id route, GuiLive::Id signature) const noexcept override {
            if (incompatible)
                return {};
            if (route == GuiLive::EntryId<&Draw>())
                return GuiLive::Adapter<decltype(Draw), &Draw>::Bind(signature);
            if (route == GuiLive::EntryId<&Clear>())
                return GuiLive::Adapter<decltype(Clear), &Clear>::Bind(signature);
            return {};
        }
    };
}  // namespace

TEST_CASE("GUI dispatch retains host state and preserves function signatures", "[editor][live-ui][unit]") {
    Registry registry;
    State state;
    auto draw = GuiLive::Find<decltype(Draw)>(&registry, GuiLive::EntryId<&Draw>());
    REQUIRE(static_cast<bool>(draw));
    REQUIRE(draw(state, 3) == 3);
    REQUIRE(state.search == "kept");
    REQUIRE_FALSE(static_cast<bool>(GuiLive::Find<void(State &)>(&registry, GuiLive::EntryId<&Draw>())));
    REQUIRE_FALSE(static_cast<bool>(GuiLive::Find<decltype(Draw)>(nullptr, GuiLive::EntryId<&Draw>())));
    REQUIRE_FALSE(static_cast<bool>(GuiLive::Find<decltype(Draw)>(&registry, GuiLive::Id{"Missing"})));
    auto clear = GuiLive::Find<decltype(Clear)>(&registry, GuiLive::EntryId<&Clear>());
    REQUIRE(static_cast<bool>(clear));
    clear(state);
    REQUIRE(state.clicks == 0);
    registry.incompatible = true;
    REQUIRE_FALSE(static_cast<bool>(GuiLive::Find<decltype(Draw)>(&registry, GuiLive::EntryId<&Draw>())));
    REQUIRE(GuiLive::Invoke<&Draw>(&registry, state, 4) == 4);
    REQUIRE(GuiLive::Invoke<&Draw>(nullptr, state, 2) == 6);
    GuiLive::Invoke<&Clear>(nullptr, state);
    REQUIRE(state.clicks == 0);
}

namespace {
    struct Owner {
        virtual ~Owner() = default;

        virtual void Draw(int &result) {
            result += 1;
        }
    };

    struct Derived : Owner {
        void Draw(int &result) override {
            result += 100;
        }
    };

    struct Unregistered : Owner {
        void Draw(int &result) override {
            result += 200;
        }
    };

    struct MemberRegistry : GuiLive::IRegistry {
        GuiLive::Binding Find(GuiLive::Id route, GuiLive::Id signature) const noexcept override {
            if (route != GuiLive::TypeId(typeid(Derived)))
                return {};
            return GuiLive::Adapter<GuiLive::MethodSignature<&Owner::Draw>, [](auto &self, auto &&...args) {
                self.Owner::Draw(std::forward<decltype(args)>(args)...);
            }>::Bind(signature);
        }
    };

    struct Modal : EditorModal {
        bool fail{};
        int calls{};

        ModalId Id() const override {
            return ModalId{1};
        }

        ModalPresentation Presentation() const override {
            return {};
        }

        ModalClosePolicy ClosePolicy() const override {
            return {};
        }

        Horo::Result<void> OnOpen(EditorModalContext &) override {
            return Horo::Result<void>::Success();
        }

        CloseDecision CanClose(ModalCloseReason) override {
            return CloseDecision::Allow;
        }

        ModalFrameResult Draw() override {
            ++calls;
            if (fail)
                throw std::runtime_error("deliberate draw failure");
            return ModalFrameResult::None();
        }
    };

    struct ModalRegistry : GuiLive::IRegistry {
        GuiLive::Binding Find(GuiLive::Id route, GuiLive::Id signature) const noexcept override {
            if (route != GuiLive::TypeId(typeid(Modal)))
                return {};
            return GuiLive::Adapter<GuiLive::MethodSignature<&EditorModal::Draw>, [](EditorModal &base) {
                auto &modal = static_cast<Modal &>(base);
                modal.calls += 10;
                if (modal.fail)
                    throw std::runtime_error("deliberate replacement failure");
                return ModalFrameResult::None();
            }>::Bind(signature);
        }
    };
}  // namespace

TEST_CASE("Member GUI adapters bypass the original object's virtual table", "[editor][live-ui][unit]") {
    MemberRegistry registry;
    Derived owner;
    int result = 0;
    Owner &base = owner;
    GuiLive::InvokeVirtual<&Owner::Draw>(&registry, base, result);
    REQUIRE(result == 1);
    GuiLive::InvokeVirtual<&Owner::Draw>(nullptr, base, result);
    REQUIRE(result == 101);
    Unregistered unregistered;
    Owner &other = unregistered;
    GuiLive::InvokeVirtual<&Owner::Draw>(&registry, other, result);
    REQUIRE(result == 301);
}

TEST_CASE("Modal host dispatches replacements without storing a registry on a modal", "[editor][live-ui][unit]") {
    EditorDataBus events;
    Horo::Input::InputRouter input;
    EditorModalHost host{events, input};
    ModalRegistry registry;
    auto owned = std::make_unique<Modal>();
    auto *modal = owned.get();
    REQUIRE(host.OpenRoot(std::move(owned)).HasValue());
    host.Draw(&registry);
    REQUIRE(modal->calls == 10);
    host.Draw();
    REQUIRE(modal->calls == 11);
    modal->fail = true;
    REQUIRE_THROWS_AS(host.Draw(&registry), std::runtime_error);
    REQUIRE(modal->calls == 21);
    modal->fail = false;
    host.Draw();
    REQUIRE(modal->calls == 22);
}

namespace {
    const int hostMetadata = 7;

    int HostApi(int value) {
        return value + hostMetadata;
    }
}  // namespace

TEST_CASE("GUI host symbols retain original data and inline API identity", "[editor][live-ui][unit]") {
    REQUIRE(&GuiLive::HostSymbol<&hostMetadata>() == &hostMetadata);
    REQUIRE(GuiLive::HostSymbol<&HostApi>()(3) == 10);
}

namespace {
    struct Pane {
        int Draw(State &state, int step) const {
            return state.clicks += step;
        }
    };

    struct PaneRegistry : GuiLive::IRegistry {
        GuiLive::Binding Find(GuiLive::Id route, GuiLive::Id signature) const noexcept override {
            if (route != GuiLive::EntryId<&Pane::Draw>())
                return {};
            return GuiLive::Adapter<GuiLive::MethodSignature<&Pane::Draw>, [](const Pane &, State &state, int step) {
                return state.clicks += step * 2;
            }>::Bind(signature);
        }
    };
}  // namespace

TEST_CASE("Common invocation routes concrete const members and retains their fallback", "[editor][live-ui][unit]") {
    const Pane pane;
    PaneRegistry registry;
    State state;
    REQUIRE(GuiLive::Invoke<&Pane::Draw>(&registry, pane, state, 3) == 6);
    REQUIRE(GuiLive::Invoke<&Pane::Draw>(nullptr, pane, state, 3) == 9);
    REQUIRE(state.search == "kept");
}
