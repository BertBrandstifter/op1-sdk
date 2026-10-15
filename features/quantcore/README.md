# Quantizer core (library)

Grid maths, timing rule, queue and note-off ordering shared by the live quantizer and requantize. No hooks, no inputs: a library other features depend on.

Shared library of `quantize` and `requantize`. API: `quant_core.h` (include it as `"../quantcore/quant_core.h"`). Spec: `features/quantize/README.md`.

Status: sim-verified. License: MIT.

## Open it on the OP-1

Library — nothing to open on the device. Shared by `quantize` and `requantize`; pulled into the `op1plus` image automatically (via requantize).
