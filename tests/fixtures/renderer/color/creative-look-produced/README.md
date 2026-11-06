# Produced creative look codec fixtures

Original outputs of `scripts/renderer/color_look_cook.py`, generated with pinned
OpenColorIO 2.5.0, NumPy 2.3.5 and CG config 4.0.0 / ACES 2.0. `source.json` is the
explicit linear AP1/red-fastest input; the other three files are unchanged output.
Both modes contain 13 actual OCIO CPU reference probes; nine shared probes per
mode matched the earlier independently generated LUT oracle exactly.

The codec tests establish actual producer/loader interoperability and byte identity.
These fixtures do not establish publisher trust, native GPU execution or display
calibration. Source/payload/config/tool hashes remain inside their original data.
