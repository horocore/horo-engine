# Runtime UI text editing

HORO-722 / #722 / [RUI-003.6] adds `UiTextEditBuffer` and integrates it into
`UiControlStateMachine`; existing append-input callers remain source compatible.
The RuntimeUi target owns `UiTextEditing.h`. Consumers need only the RuntimeUi
dependency, not utf8proc headers or native clipboard/input types.

## Ownership and admission

Create the buffer with explicit byte/grapheme bounds, history depth, validation
and password policy. Storage is fixed: at most 256 UTF-8 bytes, 256 graphemes and
16 undo snapshots plus 16 redo snapshots. Only text controls reserve this history;
other controls do not pay its storage cost. Successful editing allocates nothing
and performs no I/O. Unicode segmentation uses the existing pinned utf8proc.

Control callers route input first and apply `EditText(source, sequence, command)`
only when the UI-local default is admitted. It rejects stale owner/tree/interaction
identity, repeated sequence numbers, pending defaults, unfocused/disabled controls
and retired/stopped owners. It does not dispatch gameplay actions. Existing submit
continues through the control default-action/semantic-action boundary.
The aggregate `UiAnimationOwner` and runtime participant expose `EditControlText`
through the existing presented-view/source gate and bounded command admission.
Neither exposes its live control owner to an adapter.

`maximumTextBytes` remains the text descriptor's sole byte ceiling. Its trailing
`editing` options configure grapheme count, undo depth, validation and password
presentation. No existing caller needs to supply the new options. Selection uses
logical extended-grapheme indexes, not bytes or shaped glyph indexes. Previous and
Next do not implement visual bidi cursor movement.

## Host input and clipboard

Insert and Paste take owned normalized UTF-8. Copy and Cut return owned text;
the host optionally accesses its platform clipboard outside frame processing.
There is no native input session, platform handle or borrowed clipboard buffer in
this API. IME composition/session integration remains the separate RUI-003.7 work.

Validation applies atomically to the complete resulting draft. The typed policies
are unrestricted Unicode, single-line Unicode, and ASCII digits; all allow an
empty draft. Malformed input, invalid selections and capacity failures preserve
text, selection and history. Hosts must retain an error when clipboard access
fails; they must not represent a failed read as a successful empty Paste.

## Presentation and lifecycle

Use `TextDisplay()` / `Display()` for rendering. Password presentation returns
one bullet per grapheme, and Copy/Cut fail without revealing the secret. Semantic
snapshots and submit payloads still contain the owner value; they are not display
or logging surfaces. Clearing retained values is not a cryptographic secure-erase
guarantee.

Provider reconciliation, cancellation, focus loss, availability changes,
retirement and structural reload clear selection/history. Compatible reload
preserves validated semantic text, not undo history or native state. An
interaction-only projection preserves the same edit model while invalidating
the previous interaction identity. Shutdown closes admission and clears retained
text/history. Standalone buffers are independent owner-thread value copies.

Regression coverage is in `UiTextEditingTests.cpp`, alongside existing controls,
reload and public-header-consumer coverage.
