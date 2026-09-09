These probes run independently of Kenshi and do not deploy anything. They require
Visual Studio 2022 C++ Build Tools, a Windows SDK and Python. The GPU probes use
Windows' WARP software renderer, so an NVIDIA/AMD SDK or supported physical GPU
is not required for the probes themselves.

Build the mod first (`powershell -File build.ps1`), then run:

```powershell
python tests/run_native_tests.py
python -m unittest discover -s tests -p "test_*.py"
```

You can pass individual executable names to the native runner, for example
`python tests/run_native_tests.py InteropLifetimeTests`.

- `InteropLifetimeTests`: blocks a real D3D12 WARP queue across timeout and
  shutdown, then verifies completion, recovery of a missing completion signal,
  and explicit device removal. Only the standalone test's device is removed.
- `GpuTimingTests`: pending-query backpressure, non-flushing reads, consumption
  exactly once, and rejection of disjoint/invalid/failed samples.
- `ReprojectionMathTests`: independent pivoted matrix-inversion oracle over
  large-coordinate camera pairs, stationary cameras, and invalid inputs.
- `ShaderCacheStampTests`: locked cache, locked stamp, absent cache, unchanged
  stamp and damaged stamp, using isolated temporary files.
- `ShadowCasterBiasTests`: renders slope-bias probes on WARP with viewports
  from 1024 to 16384, checking RTW compensation, the original maximum bias,
  unchanged native/CSM calculations, unbound-buffer fallback, resolution
  switches, and constant-buffer cleanup. To additionally compile eight real
  game shader variants, run `tests/build/ShadowCasterBiasTests/ShadowCasterBiasTests.exe`
  with the path to Kenshi's `data/materials/common/shadowcaster.hlsl`.
- `ShadowAtlasTests`: exercises the production atlas manager and OM/SRV/
  viewport/ClearState hooks on WARP, using raw D3D calls as their trampolines.
  Covers repeated switches between pooled depth-only CSM and RTW color/depth
  pairs, failed depth allocation, native sampling fallback, per-atlas companion
  depth, filter texel size, and rapid switches exceeding the tracking table.
  The runner extracts the relevant production bodies into an ignored generated
  header so the test does not require the GUI/upscalers or install game hooks.
- `test_shader_fingerprint.py`: the real MSBuild fingerprint target, with
  isolated shader/layout changes and an unrelated source-file control.
- `LazyResourcesTests`: allocation failure rollback, retry delay, reuse and reset.
- `EffectResourcesTests`: loads the seven affected Release effect DLLs on WARP,
  compiles their real shaders, observes texture creation, injects an allocation
  failure after a partial allocation, and exercises disabled startup/resize,
  first enable, retry, reuse and cold debug rendering. It patches only its own
  test device's CreateTexture2D entry, restoring it before exit.

The lazy allocation change covers resolution-dependent targets in SSAO, SSIL,
RTGI (including its light-volume AO output), DOF, Bloom, SMAA and Clarity.
Shaders, settings and small fallback/lookup resources still initialize normally.
Allocated targets remain reusable when an effect is toggled off; resize and
shutdown release them. A failed allocation releases the partial set and retries
at most once per second while the effect is used. RTGI resets temporal validity
when its targets are released or first created.

These checks establish lifecycle behavior and numerical correctness, not
in-game visual quality or an FPS improvement. FSR3/FSR4 hardware interop still
needs a game run covering backend switches and resolution changes under GPU load.
