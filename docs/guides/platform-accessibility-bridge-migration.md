# Platform accessibility announcement completion migration

ACC-001.6 adds the pure virtual
`IPlatformAccessibilityBridge::TryDequeueAnnouncementCompletion(cursor)`.
Rebuild bridge implementations and their consumers: this extends the C++ vtable.
The Runtime UI semantic schema, cursor identities and setting ownership do not
change. `AccessibilityBridge.h` remains owned exclusively by the optional
`HoroPlatformAccessibility` target; baseline Platform gains no UI dependency.

## Reason and ownership

`Submit` reports bounded queue admission. An asynchronous native adapter still
needs to tell the existing UI publisher when an announcement prefix has reached
native API admission or observed cancellation. Using queue acceptance as that
signal would release the publisher's retained delivery records before the adapter
could finish or cancel them. UI-owned cancellation and retry records must survive
that interval. A native failure, resynchronization or queue overflow
must not manufacture an acknowledgment.

The composing UI owner drains completion cursors and passes each to its existing
`UiAccessibilityChangePublisher::Acknowledge`. That owner validates the exact
instance, canvas, generation and sequence. Native callbacks never mutate the
publisher or become another announcement authority. Completion is a transport
handoff result; it does not prove speech synthesis, audible output or assistive
technology qualification.
Owner-driven retirement/cancellation continues through the existing publisher
lifecycle. Unaccepted work does not acquire native ownership, and an Unsupported
adapter cannot force the UI owner to wait for a completion it will never produce.

## Adapter migration

Implement the new method explicitly. Return false without changing the cursor
when no completion is available. Supported asynchronous adapters retain copied
completions within their bounded admission budget and preserve publisher/session
prefix order. Only complete a prefix after all earlier accepted occurrences are
terminal; a failed native call alone is insufficient. Completion storage must
remain drainable during retirement, while the host retains adapter lifetime.
Deduplicate resubmitted pending cursors so retries cannot repeat an occurrence
already queued or handed to the native API.

The current production implementation is `NullPlatformAccessibilityBridge`.
It returns false before and after shutdown, changes no output, allocates no
storage and remains Unsupported with zero native queues. The explicit public
consumer and existing Null allocation/lifecycle regression cover the new method
through the base interface. The seven-target native MSVC Debug batch at `0b1f6eb`
passed **49/49 cases, zero skipped**, in 0.72 seconds; evidence is
`build/acc-caption-completion-0b1f6eb-results.xml` and its discovery JSON. Native adapters,
host draining, editor semantics and real assistive-technology checks remain
implementation/qualification work; this contract does not qualify ACC-001.6.
