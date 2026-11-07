# Clustered-lit Metal capture, 2026-11-06

These six files are the unmodified original SPIR-V, Cross JSON and translated MSL outputs for the current canonical clustered-lit shader. Portable provenance records the captured engine HLSL hash, pinned tool identities, original-to-fixture filename mapping, actual file hashes and source receipt digest. No AIR, machine-specific path or ephemeral source is included.

Both actual stages declare the complete 688-byte ViewportParameters block, including BaseColor as vec4 at byte 672. The existing packed-light/membership/reference layouts remain unchanged. The captured parser test uses the real current canonical recipe; no logical manifest or shader evidence is fabricated.

This is parser regression evidence. It does not establish authenticated cook trust, runtime pipeline creation or native pixel/GPU behavior. The historical 2026-11-01 fixture is preserved separately. These shaders are engine-authored and contain no generated OCIO analytic transform.
