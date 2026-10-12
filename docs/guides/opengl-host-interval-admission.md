# OpenGL host interval admission

`HoroOpenGLHostAdmissionInternal` owns the non-installed
`OpenGLSwapIntervalAdmission.h` header in its dedicated one-header include
directory. EditorViewportOpenGL, its test-owned material host, and backend
regressions opt in with private dependencies. RenderApi and RenderOpenGL expose
no new public header and gain no editor or SDL dependency. The standalone
`HoroOpenGLHostAdmissionInternalConsumer` links only that internal target, proving
that the contract needs neither a broad source include nor another module.

The synchronous callable seam retains no callback, native handle or allocation.
Hosts migrate blind interval setters to exact readback-first admission. Failed
readback cannot prove the initialized zero value; a different active interval
requires both successful setting and exact subsequent verification. Hosts map
query, setter and mismatch outcomes to their existing typed error descriptors.
An already active SDL-reported zero interval needs no swap-control extension.
This is SDL contract evidence, not an independent GPU query or hardware claim.

No persisted format, public API or backend-selection policy changes. The native
material qualification still rejects software identities. A separate explicit
software coverage entry invokes the same full pixel, caller-state and retirement
fixture and claims no hardware qualification.
