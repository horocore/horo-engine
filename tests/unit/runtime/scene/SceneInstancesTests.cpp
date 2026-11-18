#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Scene/SceneInstances.h"
#include "RuntimeSceneTestSupport.h"

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace RuntimeSceneTestSupport;

    /** @brief Models an immutable typed source revision held by Scene with a revalidated owner receipt. */
    class ChildPublication final : public ScenePublicationCheck {
    public:
        ChildPublication(std::shared_ptr<const SceneDefinitionRevision> active, const SceneDefinitionRevision expected) noexcept
            : active_(std::move(active)), expected_(expected) {}

        Result<void> ValidatePublication() const override {
            return *active_ == expected_ ? Result<void>::Success()
                                         : Result<void>::Failure(Error{ErrorCode{"test.scene.embedded_source_stale"}});
        }

    private:
        std::shared_ptr<const SceneDefinitionRevision> active_;
        SceneDefinitionRevision expected_;
    };

    struct Fixture final {
        static constexpr SceneDefinitionId Child{23};
        static constexpr SceneBaselineAttachmentLimits Limits{2, 16, 4};
        CancellationSource parentLifetime;
        RuntimeSceneService scene;
        std::shared_ptr<SceneDefinitionRevision> source = std::make_shared<SceneDefinitionRevision>(SceneDefinitionRevision{1});
        SceneInstanceAddress address;

        Fixture() {
            REQUIRE(scene.Startup({}).HasValue());
            ActivateParent();
            address = {scene.ActiveScene()->RuntimeId(), {2}, Child, SceneInstanceMode::Embedded};
        }

        void Commit() {
            REQUIRE(scene.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context({})).HasValue());
        }

        void ActivateParent() {
            REQUIRE(scene.QueuePreparation(Definition()).HasValue());
            Commit();
            REQUIRE(scene.ActiveScene());
        }

        static RuntimeSceneDefinition MakeChild(const SceneDefinitionRevision revision) {
            SceneDefinitionBuilder child{Child, revision};
            child.Add(Entity(10));
            child.Add(Entity(11, 10));
            auto built = std::move(child).Build();
            REQUIRE(built.HasValue());
            return std::move(built).Value();
        }

        SceneStructuralAdmission Admission() const {
            return {.scene = address.parent, .ownerCancellation = parentLifetime.Token()};
        }

        std::shared_ptr<const ScenePublicationCheck> Source() const {
            return std::make_shared<const ChildPublication>(source, *source);
        }

        Result<SceneInstanceOperation> Queue(const SceneInstanceOperation *previous = nullptr) {
            return QueueEmbeddedSceneInstance(scene, address, MakeChild(*source), {}, Admission(), Limits, previous, Source());
        }

        std::optional<SceneBaselineAttachmentView> Find() const {
            const auto parent = scene.ActiveScene();
            return parent ? FindEmbeddedSceneInstance(*parent, address) : std::nullopt;
        }
    };

    TEST_CASE("Embedded Scene instances publish replace and remove through the actual parent transaction", "[unit][scene][instances]") {
        Fixture f;
        const auto root = f.scene.ActiveScene()->Find({1});
        REQUIRE(root);
        const auto first = f.Queue();
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().Receipt());
        CHECK_FALSE(first.Value().Receipt()->Complete());
        CHECK_FALSE(f.Find());
        f.Commit();
        REQUIRE(first.Value().Receipt()->ResultValue());
        auto child = f.Find();
        REQUIRE(child);
        REQUIRE(child->entities.size() == 2);
        const auto firstChild = f.scene.ActiveScene()->Find({10});
        REQUIRE(firstChild);
        CHECK(child->entities.front() == firstChild->entity);
        const auto leaf = f.scene.ActiveScene()->Get({f.address.parent, child->entities.back()});
        REQUIRE(leaf.HasValue());
        CHECK(leaf.Value().parent == firstChild);
        CHECK(firstChild->runtime == root->runtime);

        auto other = f.address;
        other.instance = {3};
        CHECK_FALSE(FindEmbeddedSceneInstance(*f.scene.ActiveScene(), other));
        *f.source = {2};
        const auto foreign = QueueEmbeddedSceneInstance(f.scene, other, Fixture::MakeChild(*f.source), {}, f.Admission(), Fixture::Limits,
                                                        &first.Value(), f.Source());
        CHECK(foreign.HasError());
        const auto preserved = f.Find();
        REQUIRE(preserved);
        CHECK(preserved->revision == SceneDefinitionRevision{1});

        const auto replacement = f.Queue(&first.Value());
        REQUIRE(replacement.HasValue());
        f.Commit();
        REQUIRE(replacement.Value().Receipt()->ResultValue());
        child = f.Find();
        REQUIRE(child);
        CHECK(child->revision == SceneDefinitionRevision{2});
        CHECK(f.scene.ActiveScene()->Get(*firstChild).HasError());
        CHECK(f.scene.ActiveScene()->Find({1}) == root);

        // The child remains explicitly removable when the source is retired.
        *f.source = {};
        const auto removal = QueueEmbeddedSceneRemoval(f.scene, replacement.Value(), f.Admission());
        REQUIRE(removal.HasValue());
        CHECK(f.Find());
        f.Commit();
        REQUIRE(removal.Value().Receipt()->ResultValue());
        CHECK_FALSE(f.Find());
        CHECK_FALSE(f.scene.ActiveScene()->Find({10}));
        CHECK(f.scene.ActiveScene()->Find({1}) == root);

        *f.source = {2};
        const auto remounted = f.Queue();
        REQUIRE(remounted.HasValue());
        f.Commit();
        REQUIRE(remounted.Value().Receipt()->ResultValue());
        // The visible address and authored revision match; the old occurrence still cannot retire this new mount.
        CHECK(QueueEmbeddedSceneRemoval(f.scene, replacement.Value(), f.Admission()).HasError());
        CHECK(f.Find());
        REQUIRE(f.scene.QueueUnload().HasValue());
        f.Commit();
        CHECK_FALSE(f.scene.ActiveScene());
        CHECK_FALSE(f.Find());
    }

    TEST_CASE("An old embedded operation cannot cancel a newer attempt for the same visible child",
              "[unit][scene][instances][cancellation]") {
        Fixture f;
        const auto cancelled = f.Queue();
        REQUIRE(cancelled.HasValue());
        REQUIRE(cancelled.Value().Cancel(f.scene).HasValue());
        const auto *cancelledError = cancelled.Value().Receipt()->Failure();
        REQUIRE(cancelledError);
        CHECK(IsJobCancelled(*cancelledError));
        CHECK_FALSE(f.Find());

        const auto retried = f.Queue();
        REQUIRE(retried.HasValue());
        CHECK(cancelled.Value().Cancel(f.scene).HasError());
        CHECK_FALSE(retried.Value().Receipt()->Complete());
        f.Commit();
        REQUIRE(retried.Value().Receipt()->ResultValue());
        CHECK(f.Find());
        CHECK(cancelled.Value().Receipt()->Failure() == cancelledError);
    }

    TEST_CASE("Parent cancellation and stale source both prevent late embedded publication", "[unit][scene][instances][cancellation]") {
        Fixture f;
        const auto pending = f.Queue();
        REQUIRE(pending.HasValue());
        SECTION("parent cancellation after queueing") {
            f.parentLifetime.RequestCancellation();
            f.Commit();
            const auto failure = pending.Value().Receipt()->Failure();
            REQUIRE(failure);
            CHECK(IsJobCancelled(*failure));
        }
        SECTION("source replacement after queueing") {
            *f.source = {2};
            f.Commit();
            const auto failure = pending.Value().Receipt()->Failure();
            REQUIRE(failure);
            CHECK(failure->code.Value() == "test.scene.embedded_source_stale");
        }
        SECTION("parent service shutdown") {
            f.scene.Shutdown();
            CHECK(pending.Value().Receipt()->Failure());
        }
        CHECK_FALSE(f.Find());
    }

    TEST_CASE("Embedded Scene admission rejects invalid mode parent and limits", "[unit][scene][instances]") {
        Fixture f;
        SECTION("streaming and independent activation require their own authorities") {
            for (const auto mode : {SceneInstanceMode::Streamed, SceneInstanceMode::Activated}) {
                f.address.mode = mode;
                CHECK(f.Queue().HasError());
            }
        }
        SECTION("a previous parent occurrence cannot attach to a replacement") {
            f.ActivateParent();
            CHECK(f.scene.ActiveScene()->RuntimeId() != f.address.parent);
            CHECK(f.Queue().HasError());
        }
        SECTION("out of bound child keeps the active parent") {
            CHECK(
                QueueEmbeddedSceneInstance(f.scene, f.address, Fixture::MakeChild(*f.source), {}, f.Admission(), {1, 1, 1}, {}, f.Source())
                    .HasError());
        }
        CHECK(f.scene.ActiveScene()->BaselineCount() == 0);
    }
}  // namespace
