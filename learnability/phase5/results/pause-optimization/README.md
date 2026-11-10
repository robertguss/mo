# Pause optimization development evidence

All five development series are retained, including the first ten-sample
confirmation whose debug maximum missed the unchanged <=0.5 second gate.
The second confirmation (native long-string counter) passed both five-sample
series. Every sample used 5000 distinct 2048-byte jobs and 500 leases, verified
state before and after, and committed ordinal 1. No samples were replaced.

These are development measurements, not a release freeze or a wall-time guarantee.
The original official pause series and evaluation 0/4 remain unchanged. Raw store
folders and generated sources remain in the corresponding build directories;
this archive retains measurement results, commands, verification replies and logs.
Builds used GCC -O0/-O2 with compile-time nested timing, including synchronous
stderr writes. Host/storage details and exact binary hashes are in results.json.
`native-inputs.json` additionally records runtime/library and instrument hashes.

The experiment history, failed fixture invocations and validation status are in
[the development record](../../pause-optimization-development.md).
