# Runtime UI Unicode preparation

HORO-718 adds headless Unicode paragraph preparation to the RuntimeUi text owner.
HarfBuzz remains the private glyph shaper; utf8proc remains the existing grapheme
decoder. Neither supplies paragraph bidi or dictionary line breaking.

The prepared paragraph path uses ICU grapheme boundaries from the same qualified
Unicode data as bidi/break analysis; existing explicitly resolved run shaping
retains its previous utf8proc behavior. This avoids mixing Unicode versions for
fallback clusters in the new full-paragraph path.

## Dependency qualification

The selected backend is ICU4C 78.2, release commit
`f1b3db8ecd39d5b3a6eff4d5641b176c7f914dfb`. The upstream source release is
`https://github.com/unicode-org/icu/releases/download/release-78.2/icu4c-78.2-sources.tgz`,
SHA-256 `3e99687b5c435d4b209630e2d2ebb79906c984685e78635078b672e03c89df35`.
The downloaded archive was independently hashed before dependency addition.

The complete upstream LICENSE is shipped, not just its Unicode-3.0 heading.
It includes ICU legacy notices and dictionary/data notices (Chinese/Japanese,
Lao and Burmese), double-conversion and build-tool notices. LICENSE SHA-256 is
`e55522d81edc687a341a4411e0776e54ca654e90147f354a90458aaced4116af`.

Only the upstream common library source manifest is compiled, together with its
stub data entry point. `common/sources.txt` SHA-256 is
`c9b33a6ae1fb56b39eff6d364404582768e21ae2262674a6b65b5a11380e7405`.
That library contains ubidi, ubrk, rule-based break iterators and dictionary
engines. No editor, renderer, platform font discovery or ICU formatting library
is introduced. ICU symbols use a Horo-specific upstream-supported library suffix.

The exact shipped `source/data/in/icudt78l.dat` is 33,107,248 bytes, SHA-256
`a3d49eacd189624769dc77457c6aa1789a89219fb217a6d87d5b076c269aaf1f`.
The full package preserves locale rules and dictionary resources; Thai is not
implemented by character-only or ASCII break heuristics. Unicode data is 17.0.
The package is little-endian, matching supported Linux/macOS/Windows targets.
Other endian profiles must provide a separately qualified package, not silently
reinterpret these bytes. This is source/build qualification, not evidence that
platform compilation or relocated-package execution has already passed.

Linux/GCC and macOS/Clang CI build all native targets and include the headless
Unicode regressions. The focused Windows/MSVC registry explicitly includes the
three Unicode executables, existing shaping/layout regressions and RuntimeUi
public-header consumers. Their private link closure compiles the actual ICU
target, not a system-library substitute. Sonar's native coverage labels include
these same headless regressions. CI wiring is not a claim that a new head has
passed on any runner. CMake rejects every non-little-endian profile before
target generation; runtime creation independently rejects incompatible native
endianness or package digest. Supported x86-64/AArch64 little-endian profiles use
the same byte package; other architectures remain unqualified, not fallback.

## Ownership and phases

The host loads the installed package before activating text owners and explicitly
creates the Unicode runtime from its bytes. Creation checks the exact digest and
copies aligned immutable bytes before registering them with ICU. Native types
remain private. The private ICU build disables filesystem loading and plugins.

One process runtime owns initialization; analyzers retain it while native bidi
objects and break iterators exist. Text, locale, font or document reload replaces
analyzer/candidate state, never the process Unicode data. Closing admission does
not invalidate immutable result leases. The host explicitly calls `Shutdown` at
its safe point; surviving analyzer/result leases return pending failure and retain
the process data pin. Their destructors never reset ICU. Shutdown retries cleanup
only after every analyzer has closed its ICU objects. No other instance
may initialize this private runtime concurrently or reset it during frame work.

The host serializes admission closure with its preparation thread and joins that
thread before retrying final shutdown. `Close` is not permission to destroy data
under a running native call. Abandoning the host without successful `Shutdown`
deliberately retains the process data pin until process exit rather than freeing
registered bytes; it is not a successful unload. This is the text-strategy portion
of ADR-075/ADR-081's close, join, retire, drain, then release ordering.

Changed-text analysis is bounded owner/load preparation: ICU may allocate scratch
within admitted text limits. It is not claimed allocation-free. Cached immutable
queries, source/visual mapping and already-prepared layout consume bounded stored
evidence and do not load data or initialize Unicode services. Locale and direction
are explicit; no process-default locale is consulted.

At a content/locale/direction change the owner calls `Analyze`, then supplies that
exact lease to `UiTextShapingRequest::unicode`. The shaper rejects mismatched
source/content/language evidence and copies levels/breaks into its immutable
cluster output. `LayoutShaped` consumes those two prepared leases; it never calls
`Analyze` or loads Unicode data. Unchanged frames retain prepared shape/analysis
leases and may re-layout for a changed assigned box using reserved output/scratch.
Legacy explicitly resolved shaping runs do not acquire a hidden global runtime.

`UiTextLayoutResult::Clusters` owns visual-order boxes with source byte ranges,
logical cluster identity, line and line-resolved caret direction. The mapping
survives source-owner shutdown. It is not a borrowed pointer into analyzer scratch.

## Production integration and verification

Paragraph levels determine shaping direction/run boundaries. Break opportunities
are intersected with complete shaped-cluster boundaries. Wrapping precedes UAX #9
line-specific whitespace reset and reordering. Results retain source-byte mapping
instead of treating reordered text as the authoritative input.

Required regressions include mixed directions/numerals, isolates/embeddings,
brackets/neutrals, combining/emoji clusters, mandatory breaks, NBSP/ZWSP, CJK and
Thai dictionary boundaries, source/visual round trips, malformed UTF-8, capacity
and lease exhaustion, locale/content/font replacement, and shutdown with old
results alive. Public consumer and relocated data-package coverage are required.
No GPU, editor interaction or zero-allocation changed-text claim follows from
these headless algorithm tests.
