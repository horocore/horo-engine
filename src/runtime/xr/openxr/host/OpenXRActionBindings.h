#pragma once

/** @file OpenXRActionBindings.h
 * @brief Non-installed OpenXR action ownership, suggested paths and active-profile adapter.
 */

#include "Horo/XR/XRActionBindings.h"
#include "OpenXRNativeSession.h"

namespace Horo::XR::OpenXRInternal {
    /** @brief Registered native profile path; native spellings never enter persistence or gameplay identity. */
    struct NativeInteractionProfilePath final {
        XRInteractionProfileId profile;
        std::string_view path;
    };

    /** @brief Registered native component path joined by exact Horo profile/control/role identity. */
    struct NativeActionControlPath final {
        XRInteractionProfileId profile;
        XRPhysicalControlId control;
        XRTrackedDeviceRole role;
        std::string_view path;
    };

    /** @brief Host-localized native binding-editor label, separate from canonical action identity. */
    struct NativeActionLabel final {
        Input::ActionId action;
        std::string_view label;
    };

    /** @brief Host-localized native set label, independent of native names and dynamic Input contexts. */
    struct NativeActionSetLabel final {
        XRActionSetId set;
        std::string_view label;
    };

    /** @brief Borrowed localized presentation copied only during native action creation. */
    struct NativeActionLabels final {
        std::span<const NativeActionLabel> actions;
        std::span<const NativeActionSetLabel> sets;
    };

    /** @brief Private registered pose binding; grip/aim are physical tracking, not newly invented Input actions. */
    struct NativePoseActionPath final {
        XRInteractionProfileId profile;
        XRActionSetId set;
        XRTrackedDeviceRole role;
        XRTrackedPoseKind kind;
        std::string_view path;
        std::string_view label;
    };

    /** @brief Exact current origin and observed reference-space choice for private tracking preparation. */
    struct NativeTrackingSpaceRequest final {
        XRWorldOriginRevision origin;
        XRSpaceKind base{XRSpaceKind::Local};         /**< Only observed Local or Stage; never an implicit World alias. */
        std::optional<Math::Transform> worldFromBase; /**< Explicit rigid mapping of native base coordinates into authored World.
                                                       Its inverse creates a distinct native World reference space; absence grants none. */
    };

    /** @brief Synchronously borrowed native creation request; dispatch and catalogs stay inside the host boundary. */
    struct NativeActionBindingRequest final {
        PFN_xrGetInstanceProcAddr getProc; /**< Official dispatch for the exact retained loader/instance. */
        const XRActionBindingSchema &schema;
        std::span<const XRProfileControl> catalog;
        std::span<const XRActionBindingOverride> overrides;
        std::span<const NativeInteractionProfilePath> profiles;
        std::span<const NativeActionControlPath> controls;
        NativeActionLabels labels;
        std::span<const NativePoseActionPath> posePaths;
        std::optional<NativeTrackingSpaceRequest> tracking;
    };

    /** @brief Fixed scalar sample packet; the canonical XRRuntime owner supplies revision/time and publishes it atomically. */
    class NativeScalarActionSamples final {
    public:
        /** @brief Returns only the populated prefix. @return Borrow valid for this packet's value lifetime. */
        [[nodiscard]] std::span<const XRTrackedActionRecord> Actions() const noexcept {
            return std::span{records_}.first(count_);
        }

    private:
        friend class OpenXRActionBindings;
        std::array<XRTrackedActionRecord, XRActionBindingLimits::MaximumActions> records_{};
        std::size_t count_{};
    };

    /** @brief Fixed native-normalized candidate; XRRuntime alone validates/publishes the immutable snapshot. */
    struct NativeLocatedPose final {
        XRDeviceId device;
        XRTrackedPoseKind kind;
        XRPoseDescriptor pose;
    };

    /** @brief Bounded complete pose candidate, with no native handles or publication authority. */
    class NativePoseSamples final {
    public:
        /** @brief Returns only complete records. @return Borrow valid for this value's lifetime. */
        [[nodiscard]] std::span<const NativeLocatedPose> Poses() const noexcept {
            return std::span{records_}.first(count_);
        }

    private:
        friend class OpenXRActionBindings;
        std::array<NativeLocatedPose, 5> records_{};  // Head plus the two grip/aim pairs.
        std::size_t count_{};
    };

    /** @brief Native observation before XRRuntime issues a device identity; activity never implies a profile or reconnect.
     * @details Pose component validity remains independent of action activity. Inactive actions carry invalid components;
     * they do not prove physical disconnection. Head activity requires actual located component evidence.
     */
    struct NativePoseObservation final {
        XRTrackedDeviceRole role;
        XRTrackedPoseKind kind;
        bool active{};
        XRPoseDescriptor pose;
    };

    /** @brief Fixed complete native evidence; only the canonical XRRuntime owner may attach device generations. */
    class NativePoseObservations final {
    public:
        /** @brief Borrows the populated prefix. @return Values remain valid for this packet's lifetime, never native borrows. */
        [[nodiscard]] std::span<const NativePoseObservation> Poses() const noexcept {
            return std::span{records_}.first(count_);
        }

    private:
        friend class OpenXRActionBindings;
        std::array<NativePoseObservation, 5> records_{};
        std::size_t count_{};
    };

    /** @brief Complete bounded native view result in the retained base space, before canonical view-plan admission.
     * @details Native ABI values remain private. Validity flags are preserved independently; absent tracking never
     * becomes a default-valid camera. The reference-to-World pose comes from the same native time and owned spaces.
     */
    struct NativePresentationViews final {
        std::array<XrView, XRHardLimits::MaximumViews> views{};
        std::uint32_t count{};
        XrViewStateFlags flags{};
        XRCoordinateSpace reference;
        std::array<XRCoordinateSpace, 2> eyes; /**< Logical View observation identities issued only after actual successful xrLocateViews.
                                                 Not XrSpace children and never admitted by BorrowWorldSpace. */
        XRPoseDescriptor referenceToWorld;
    };

    /**
     * @brief Single-control-thread native action transaction; session/loader owner must outlive it.
     *
     * Creation validates every schema, override and native path before allocation, creates sets/actions,
     * suggests all registered profiles, then attaches exactly once before host Ready publication.
     * Input contexts do not create/destroy native sets. The host quiesces sampling, neutralizes the
     * XR/Input coordinator, closes this owner, and only then closes the native session.
     */
    class OpenXRActionBindings final {
    public:
        /** @brief Borrows the exact fenced session owner without resolving native dispatch. */
        explicit OpenXRActionBindings(OpenXRNativeSession &session) noexcept;
        /** @brief Retires native actions; explicit successful Close is required before destruction. */
        ~OpenXRActionBindings();
        OpenXRActionBindings(const OpenXRActionBindings &) = delete;
        OpenXRActionBindings &operator=(const OpenXRActionBindings &) = delete;

        /**
         * @brief Creates and attaches a fully validated native action candidate atomically.
         * @param session Exact current Horo session generation.
         * @param request Product schema, registered private paths, Input overrides, localized labels and official native dispatch.
         * @return Success or typed validation/native/rollback error; allocation and format exceptions become preparation errors.
         * Failed retirement retains handles for Close retry.
         * @throws Other native callback exceptions after attempting rollback; the original exception propagates unchanged.
         */
        [[nodiscard]] Result<void> Create(const XRSessionId &session, const NativeActionBindingRequest &request);
        /** @brief Creates native action children on the same unpublished session without issuing Horo identities.
         * @param preparation Exact retained installation and attempt whose native session already exists.
         * @param maximumActions Validated bounded product-schema capacity; not a negotiated frame-use limit.
         * @param request Complete registered schema/native catalogs copied during preparation.
         * @return Complete native preparation or typed failure, retaining failed cleanup debt for Close.
         * @post No Sync, sampling, Input publication or Ready admission until Bind succeeds.
         * @throws Unsupported callback exceptions after rollback, as for Create.
         */
        [[nodiscard]] Result<void> PrepareAttempt(const NativeSessionPreparation &preparation, std::uint32_t maximumActions,
                                                  const NativeActionBindingRequest &request);
        /** @brief Binds the already attached children to the canonical lifecycle-issued session.
         * @param session Issued candidate already activated by the same OpenXRNativeSession owner.
         * @return Success or typed stale/capacity failure; no native children are recreated or reattached.
         * @pre PrepareAttempt succeeded; the host retains the exact prepared owner on failed binding for cleanup.
         */
        [[nodiscard]] Result<void> Bind(const XRSessionId &session);
        /**
         * @brief Queries both active controller profiles as Horo IDs; unrecognized native paths map to unknown identity.
         * @param session Exact retained session generation.
         * @return Bounded per-role evidence or typed stale/native failure. Host resolves and neutralizes changes before Sync/sample.
         */
        [[nodiscard]] Result<std::array<XRActiveInteractionProfile, 2>> Profiles(const XRSessionId &session) const;
        /** @brief Observes actual profiles on the same unpublished session after its sole action-set attachment.
         * @param preparation Exact live preparation attempt/install/runtime; no canonical session ID is accepted or issued.
         * @return Actual registered profile identities (unknown remains zero), or the original stale/native failure.
         * @pre Owner lane, PrepareAttempt succeeded. The resulting value does not admit Sync, sampling, or Ready.
         */
        [[nodiscard]] Result<std::array<XRActiveInteractionProfile, 2>> PreparedProfiles(const NativeSessionPreparation &preparation) const;
        /** @brief Synchronizes attached native sets after host binding neutralization/admission.
         * @param session Exact retained owner.
         * @return Success or typed focus/session/native failure; no gameplay or Input mutation occurs here.
         */
        [[nodiscard]] Result<void> Sync(const XRSessionId &session) const;
        /** @brief Samples the current synchronized scalar actions without publishing Input or a tracking snapshot.
         * @param session Exact current canonical session generation.
         * @param plan Current existing resolved binding publication, never a native-path plan.
         * @param devices Exact current XRRuntime-issued physical device generations.
         * @return Fixed complete packet or the original native/typed validation failure; no partial packet escapes.
         * @pre Sync has succeeded since the previous sampling attempt. The host has admitted focused simulation sampling.
         * @post Successful normal sampling allocates no storage and produces no transition flags or gameplay mutations.
         */
        [[nodiscard]] Result<NativeScalarActionSamples> Sample(const XRSessionId &session, const XRResolvedActionBindings &plan,
                                                               std::span<const XRTrackedDeviceRecord> devices) const;
        /** @brief Locates the same synchronized native pose spaces at the caller's exact runtime sampling time.
         * @param session Current lifecycle-issued owner.
         * @param time Existing simulation-cutoff clock evidence; runtimeSample is the exact native XrTime, never a host-clock cast.
         * @param origin Current origin generation, required to match prepared spaces.
         * @param devices Current XRRuntime-issued device generations; no device identities are fabricated here.
         * @return Complete bounded independent-validity candidates or original typed/native failure; no partial result escapes.
         * @pre Sync succeeded for this opportunity. Spaces and their native parent remain retained through this call.
         * @post Presentation-only prediction cannot enter the simulation snapshot through this method.
         */
        [[nodiscard]] Result<NativePoseSamples> SamplePoses(const XRSessionId &session, const XRTimeEvidence &time,
                                                            XRWorldOriginRevision origin,
                                                            std::span<const XRTrackedDeviceRecord> devices) const;
        /** @brief Samples actual head and prepared grip/aim sources before canonical device admission.
         * @param session Exact active owner. @param time Same simulation-cutoff native clock conversion.
         * @param origin Exact prepared reference-space origin.
         * @return Complete bounded activity/independent-validity evidence, or original failure with no partial packet.
         * @pre Sync succeeded; this consumes the SAME pose opportunity as SamplePoses and cannot be retried until another Sync.
         * @post No device/sample identity, profile-derived presence, allocation, Input commit or publication is introduced.
         */
        [[nodiscard]] Result<NativePoseObservations> ObservePoses(const XRSessionId &session, const XRTimeEvidence &time,
                                                                  XRWorldOriginRevision origin) const;
        /** @brief Returns the distinct calibrated World identity after native creation and canonical binding.
         * @param session Exact active native owner. @return Current World space or typed unavailable/stale failure.
         */
        [[nodiscard]] Result<XRCoordinateSpace> WorldSpace(const XRSessionId &session) const;
        /** @brief Borrows only the exact retained calibrated World child, never a raw ambient reference space.
         * @param space Exact session/space/origin identity returned by WorldSpace.
         * @return Synchronous native borrow or typed stale failure; no borrow survives owner retirement/reentry.
         */
        [[nodiscard]] Result<XrSpace> BorrowWorldSpace(const XRCoordinateSpace &space) const;
        /** @brief Quiescent idempotent reverse-order retirement; failed destruction retains exact handles for retry.
         * @return Success or native retirement failure. No further sync/profile query is admitted after Close begins.
         */
        [[nodiscard]] Result<void> Close();
        /** @brief Resolves the official presentation query against the same active native parent outside frame work.
         * @param getProc Resolver retained by that parent's installed loader.
         * @return Original native/currentness/capacity failure or complete dispatch. Actual stereo configuration is
         * enumerated cold; distinct logical eye identities reserve from the SAME bound semantic-space allocator.
         * No eye identity is exposed until a successful LocatePresentation, and none grants native XrSpace borrowing.
         * No frame/capability or tracking validity is published.
         */
        [[nodiscard]] Result<void> PreparePresentation(PFN_xrGetInstanceProcAddr getProc);
        /** @brief Locates actual primary-stereo views at the runtime's exact predicted display time.
         * @param session Same issued active owner. @param time Actual OS-converted sample plus native frame prediction, no simulation time.
         * @param expectedViews Complete primary view count observed by the same native system census.
         * @return Fixed complete native evidence, or a typed error without partial publication/identity issuance.
         * @pre Canonical frame owner successfully waited/began this prediction; the host separately checks frame admission.
         * @post No allocation, graph compilation, Input publication, extra clock or queue/compositor work on success.
         */
        [[nodiscard]] Result<NativePresentationViews> LocatePresentation(const XRSessionId &session, const XRTimeEvidence &time,
                                                                         std::uint32_t expectedViews) const;

    private:
        struct Operation final {
            bool &busy;
            const bool previous;

            explicit Operation(bool &state) noexcept : busy(state), previous(state) {
                busy = true;
            }

            ~Operation() {
                busy = previous;
            }

            Operation(const Operation &) = delete;
            Operation &operator=(const Operation &) = delete;
        };

        struct Dispatch final {
            PFN_xrStringToPath stringToPath{};
            PFN_xrCreateActionSet createSet{};
            PFN_xrDestroyActionSet destroySet{};
            PFN_xrCreateAction createAction{};
            PFN_xrDestroyAction destroyAction{};
            PFN_xrSuggestInteractionProfileBindings suggest{};
            PFN_xrAttachSessionActionSets attach{};
            PFN_xrGetCurrentInteractionProfile profile{};
            PFN_xrSyncActions sync{};
            PFN_xrGetActionStateBoolean booleanState{};
            PFN_xrGetActionStateFloat floatState{};
            PFN_xrGetActionStateVector2f vectorState{};
            PFN_xrGetActionStatePose poseState{};
            PFN_xrCreateReferenceSpace createReference{};
            PFN_xrCreateActionSpace createSpace{};
            PFN_xrDestroySpace destroySpace{};
            PFN_xrLocateSpace locate{};
            PFN_xrLocateViews locateViews{};
        };

        struct Set final {
            XRActionSetId id;
            XrActionSet native{XR_NULL_HANDLE};
        };

        struct Action final {
            Input::ActionId id;
            XrAction native{XR_NULL_HANDLE};
            Input::ActionValueType type{Input::ActionValueType::Digital};
            XRTrackedDeviceRole role{XRTrackedDeviceRole::LeftController};
        };

        struct ScalarCandidate final {
            const Action *action{};
            XRTrackedDeviceRecord device;
            std::uint32_t bindingIndex{};
            XRInteractionProfileId profile;
            bool fallback{};
        };

        struct ScalarPlan final {
            std::array<ScalarCandidate, XRActionBindingLimits::MaximumActions> entries{};
            std::size_t count{};
        };

        struct Profile final {
            XRInteractionProfileId id;
            XrPath native{XR_NULL_PATH};
        };

        struct Preparation final {
            XRLoaderPreflightSnapshot preflight;
            XRLoaderPreflightAttempt attempt;
            std::uint32_t maximumActions{};
            bool complete{}; /**< Successful immutable attachment; cleared before any retirement callback. */
        };

        struct Attachment final {
            XrSession native{XR_NULL_HANDLE};
            std::uint64_t incarnation{};
            XRSessionId owner;
        };

        struct PoseAction final {
            XRActionSetId set;
            XRTrackedDeviceRole role;
            XRTrackedPoseKind kind;
            XrAction native{XR_NULL_HANDLE};
            XrSpace space{XR_NULL_HANDLE};
            XRCoordinateSpace identity;
        };

        struct Tracking final {
            std::vector<PoseAction> actions;
            std::optional<NativeTrackingSpaceRequest> request;
            XrSpace base{XR_NULL_HANDLE};
            XrSpace view{XR_NULL_HANDLE};
            XrSpace world{XR_NULL_HANDLE};
            XRCoordinateSpace baseIdentity;
            XRCoordinateSpace viewIdentity;
            XRCoordinateSpace worldIdentity;
            std::array<XRCoordinateSpace, 2> eyeIdentities{};
            std::uint32_t nextSpaceSlot{}; /**< SAME bound per-session semantic space allocator, after native child slots. */
            std::uint32_t generation{};    /**< Non-wrapping space incarnation; retained across Close. */
            mutable std::uint64_t sampledSequence{};
        };

        /** @brief Queries actual registered profiles with a retained native owner; callers fence before and after foreign calls. */
        [[nodiscard]] Result<std::array<XRActiveInteractionProfile, 2>> ReadProfiles(XrSession session) const;
        [[nodiscard]] Result<void> Resolve(const NativeActionBindingRequest &request, XrInstance instance);
        /** @brief Resolves optional complete tracking dispatch before acquiring any tracking child. */
        [[nodiscard]] Result<void> ResolveTracking(const NativeActionBindingRequest &request, XrInstance instance);
        /** @brief Runs one guarded preparation/rollback transaction for either private or legacy admission. */
        [[nodiscard]] Result<void> PrepareTransaction(const NativeActionBindingRequest &request, std::uint32_t maximumActions);
        /** @brief Revalidates the retained preparation or active owner after foreign callbacks. */
        [[nodiscard]] Result<void> ValidateOwner() const;
        /** @brief Rejects foreign control lanes before reading or mutating retained native ownership. */
        [[nodiscard]] Result<void> ControlLane() const;
        /** @brief Validates exact registered grip/aim paths before creating native children. */
        [[nodiscard]] Result<void> ValidatePosePaths(const NativeActionBindingRequest &request, std::uint32_t maximumActions) const;
        /** @brief Creates tracking actions before the sole immutable action-set attachment. */
        [[nodiscard]] Result<void> CreatePoseActions(std::span<const NativePoseActionPath> paths);
        /** @brief Appends registered pose paths to one profile's existing suggestion transaction. */
        [[nodiscard]] Result<std::uint32_t> SuggestPoses(const Profile &profile, std::span<const NativePoseActionPath> paths,
                                                         std::span<XrActionSuggestedBinding> output) const;
        /** @brief Creates native reference/action spaces without publishing Horo IDs. */
        [[nodiscard]] Result<void> PrepareSpaces();
        /** @brief Issues generation-safe space identities only for already created children and issued session. */
        [[nodiscard]] Result<void> BindSpaces(const XRSessionId &session);
        /** @brief Retires spaces before the borrowed pose actions; failed destruction preserves exact handles. */
        [[nodiscard]] Result<void> CloseSpaces();
        /** @brief Destroys pose actions after all their spaces; preserves failed native handles. */
        [[nodiscard]] Result<void> ClosePoseActions();
        /** @brief Validates and normalizes one official locate result without inventing unavailable components. */
        [[nodiscard]] Result<XRPoseDescriptor> Locate(XrSpace source, const XRCoordinateSpace &identity, const XRTimeEvidence &time) const;
        /** @brief Queries one action's activity then locates independently valid components, or publishes exact loss. */
        [[nodiscard]] Result<NativePoseObservation> LocateAction(const PoseAction &action, const XRTimeEvidence &time) const;
        /** @brief Appends actual head evidence to the same fixed packet under the existing sampling operation.
         * @param time Validated simulation cutoff. @param samples Candidate packet, never a committed snapshot.
         * @return Original native/pose failure without issuing device or session identity.
         */
        [[nodiscard]] Result<void> AppendHeadObservation(const XRTimeEvidence &time, NativePoseObservations &samples) const;

        /** @brief Locates and validates the current native base-to-World presentation transform.
         * @param time Exact validated presentation prediction. @return Actual independent-validity pose or native failure.
         * @pre Called within the existing sampling operation between canonical currentness fences.
         */
        [[nodiscard]] Result<XRPoseDescriptor> PresentationReference(const XRTimeEvidence &time) const;

        /** @brief One native sampling implementation; an explicit existing-device selection preserves legacy query filtering.
         * @param session Exact current native owner. @param time Current simulation-cutoff evidence.
         * @param origin Prepared origin. @param devices Existing runtime-owned records, or no selection for native discovery.
         * @return Complete normalized evidence; failed attempts consume their Sync opportunity.
         */
        [[nodiscard]] Result<NativePoseObservations> CollectPoseObservations(
            const XRSessionId &session, const XRTimeEvidence &time, XRWorldOriginRevision origin,
            std::optional<std::span<const XRTrackedDeviceRecord>> devices) const;
        [[nodiscard]] Result<void> Prepare(const XRActionBindingSchema &schema, std::span<const XRProfileControl> catalog,
                                           std::span<const XRActionBindingOverride> overrides,
                                           std::span<const NativeInteractionProfilePath> profiles,
                                           std::span<const NativeActionControlPath> controls, const NativeActionLabels &labels,
                                           std::uint32_t maximumActions, std::span<const NativePoseActionPath> poses);
        [[nodiscard]] Result<void> CreateActions(const XRActionBindingSchema &schema, const NativeActionLabels &labels);
        [[nodiscard]] Result<void> Suggest(const XRActionBindingSchema &schema, std::span<const XRProfileControl> catalog,
                                           std::span<const XRActionBindingOverride> overrides,
                                           std::span<const NativeInteractionProfilePath> profiles,
                                           std::span<const NativeActionControlPath> controls, std::span<const NativePoseActionPath> poses);
        /** @brief Copies bounded scalar provenance before foreign callbacks. @param plan Existing resolved plan.
         * @param devices Current runtime-owned source generations. @return Immutable local candidate metadata or validation failure. */
        [[nodiscard]] Result<ScalarPlan> PrepareScalarPlan(const XRResolvedActionBindings &plan,
                                                           std::span<const XRTrackedDeviceRecord> devices) const;
        /** @brief Samples one official Boolean state. @param info Exact native action/subaction query.
         * @param record Zero candidate with current source provenance. @return Normalized value or original native/typed failure. */
        [[nodiscard]] Result<XRTrackedActionRecord> ReadBoolean(const XrActionStateGetInfo &info, XRTrackedActionRecord record) const;
        /** @brief Samples one official scalar state. @param info Exact native action/subaction query.
         * @param record Zero candidate with current source provenance. @return Normalized value or original native/typed failure. */
        [[nodiscard]] Result<XRTrackedActionRecord> ReadFloat(const XrActionStateGetInfo &info, XRTrackedActionRecord record) const;
        /** @brief Samples one official vector state. @param info Exact native action/subaction query.
         * @param record Zero candidate with current source provenance. @return Normalized value or original native/typed failure. */
        [[nodiscard]] Result<XRTrackedActionRecord> ReadVector(const XrActionStateGetInfo &info, XRTrackedActionRecord record) const;
        /** @brief Dispatches the registered exact native value shape. @param action Stable action-owner metadata.
         * @param device Current runtime-owned source. @param bindingIndex Exact plan-local position.
         * @return Normalized complete record or typed native/validation failure. */
        [[nodiscard]] Result<XRTrackedActionRecord> ReadScalar(const Action &action, const XRTrackedDeviceRecord &device,
                                                               std::uint32_t bindingIndex) const;
        [[nodiscard]] Result<NativeSessionBorrow> Current(const XRSessionId &session) const;
        OpenXRNativeSession *session_;
        const std::thread::id controlThread_{std::this_thread::get_id()};
        XRSessionId owner_;
        Attachment attached_;
        NativeSessionBorrow native_{};
        Dispatch dispatch_;
        std::array<XrPath, 2> hands_{};
        std::vector<Set> sets_;
        std::vector<Action> actions_;
        std::vector<Profile> profiles_;
        bool active_{};
        std::uint32_t schemaVersion_{};
        XRInteractionProfileId fallbackProfile_;
        mutable std::uint64_t syncSequence_{}; /**< Operation receipt, not a simulation/render clock or frame authority. */
        mutable std::uint64_t sampledSequence_{};
        mutable bool sampling_{};                /**< Prevents reentrant native mutation while action references are borrowed. */
        std::optional<Preparation> preparation_; /**< Attempt-only admission; contains no canonical session or frame authority. */
        Tracking tracking_;
    };
}  // namespace Horo::XR::OpenXRInternal
