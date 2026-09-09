# Creative effect controls

## Depth-dependent Kuwahara

Enable **Depth Dependent** to vary the filter with the depth buffer. **Near
Radius** and **Near Strength** apply at and before **Depth Start**. The existing
**Radius** and **Strength** apply at and beyond **Depth End**, with a smooth ramp
between them. Set both near values to zero to preserve close-up detail.

Depth uses Kenshi's linear distance divided by the camera far clip, the same
units used by DoF. Reversed endpoints are sorted; equal endpoints form a short,
finite transition. Sky uses the far values. Missing depth falls back to the
ordinary constant-radius filter. Fractional radii gradually introduce samples
instead of rounding the radius to an integer. Radius zero bypasses the filter.

The feature defaults off. Loading an older preset without these controls resets
them to their defaults, even after a preset that enabled them. Debug View shows
full filter strength, retaining the depth-dependent radius.
