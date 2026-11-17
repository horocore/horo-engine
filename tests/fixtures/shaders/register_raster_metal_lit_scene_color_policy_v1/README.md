# Current scene-color policy Metal capture

Original DXC strict SPIR-V, SPIRV-Cross reflection/MSL and successful native Metal
object compilation for the current clustered lit shader. SceneColorPolicy is
parameter 21 at byte offset 688; the reflected uniform block is 704 bytes.
The prior BaseColor-only capture remains unchanged in its separate directory.

Parser tests use these original bytes. They do not run compilers or establish GPU,
publisher-trust or physical-display qualification. See capture-provenance.json
for exact compiler arguments and source/output hashes.
