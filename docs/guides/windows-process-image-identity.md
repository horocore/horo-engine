# Windows main executable identity

The Platform-owned process image provider binds the actual main executable to a
retained read-only file mapping through the kernel's `NtAreMappedFilesTheSame`
operation. The loader-reported path locates a candidate; neither that string,
PE headers, nor equality of file digests authorizes an image. A byte-identical
copy is a different backing file and must be rejected.

The native operation is resolved at runtime from the existing system `ntdll`
module. An absent operation, unsupported mapping, missing file identity, denied
immutable open, changed metadata or failed comparison returns the existing typed
`platform.process_image.unavailable` result. There is no pathname fallback or
permission change. Native API availability must be qualified on each supported
Windows profile; this provider does not establish publisher trust or cross-build
Physics determinism.

The evidence owner retains the file, mapping and data view. New write/delete
opens are denied for the complete evidence lifetime. A complete-file hash is
bounded to 512 MiB, and file identity, size, write/change metadata and kernel
mapping equality are checked before publication and on later verification.
Access-time updates are excluded from content comparisons. The final lease may
release these thread-independent Windows resources on another thread.

Static participating addresses must additionally be in a committed executable
`MEM_IMAGE` region whose allocation base is the actual main executable. Null,
heap/stack, main-image data and foreign DLL addresses do not qualify. A dynamic
module needs its own loader lifetime; main-image evidence never substitutes for
that ownership.

`HoroProcessImageIdentityTests` exercises the real Windows comparison, copied-file
denial in a spaced Unicode path, invalid mappings and addresses, write/delete-open
denial, allocation failure and foreign last-drop handle ownership.
An owned compiled child also exercises actual pathname replacement before
capture, including rejection without leaked handles, and replacement denial
while captured evidence remains retained. Only two synchronization events are
inherited; the test changes no production identity inputs or permissions.
The existing
Physics World, native and product host suites remain mandatory and unchanged.
The Windows CI registry builds and selects these suites, including the unchanged
allocation-free restore regression. Native-only suites are registered when the
native Physics target is enabled; the image provider also qualifies independently
of that optional solver composition.
Recorded runs, rather than this implementation description, establish which
profiles and integration paths were actually qualified.
