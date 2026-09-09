# Creative effect controls

## Depth-dependent Kuwahara

Enable **Depth Dependent** to vary the filter with the depth buffer. **Near
Radius** and **Near Strength** apply at and before **Near Depth**. The existing
**Radius** and **Strength** are labeled **Far Radius** and **Far Strength** when
depth dependence is enabled, and apply at and beyond **Far Depth**, with a smooth ramp
between them. Set both near values to zero to preserve close-up detail.

Depth uses Kenshi's linear distance divided by the camera far clip, the same
units used by DoF. Reversed endpoints are sorted; equal endpoints form a short,
finite transition. Sky uses the far values. Missing depth falls back to the
ordinary constant-radius filter. Fractional radii gradually introduce samples
instead of rounding the radius to an integer. Radius zero bypasses the filter.

Near Radius/Strength describe how much filtering to apply nearby, rather than
how much nearby detail to protect. Raising them above the far values makes the
filter stronger nearby. To filter distant objects only, leave both near values
at zero and increase the far values. **Distance Fade Preview** shows the near
region as black, the far region as white, and the transition as gray. Magenta
indicates disabled/unavailable depth modulation. Later effects can modify this
preview, so view it in LDR with later effects disabled when diagnosing the ramp.

The feature defaults off. Loading an older preset without these controls resets
them to their defaults, even after a preset that enabled them. **Preview Full
Strength** (formerly Debug View) temporarily sets near and far strength to 1,
retaining the depth-dependent radius. Where strength is already 1 it produces
the same image. Distance Fade Preview takes precedence when both are enabled.
The saved INI key remains `DebugView` for compatibility.

## Kuwahara after DoF

Set Kuwahara's **Render Stage** to **After tonemapping (LDR)** to filter the
DoF output. The original HDR stage remains the default for existing presets.

Interactive effect ordering has been removed for now. The settings list is
compact again and effects use their fixed default execution order. Previously
saved custom ordering values are ignored and are no longer written to presets.
The direct Kuwahara stage choice remains available.

## Clarity luminance protection

**Luminance Protect** fades Clarity according to each input pixel's luminance
before Clarity modifies it. At **1**, pixels at or below **Luminance Start**
receive no enhancement. The contribution ramps smoothly to full strength at
**Luminance End**. Values between 0 and 1 provide partial protection; 0 preserves
the previous behavior. The starting thresholds are 0.05 and 0.25 in the current
LDR image (0 = black, 1 = white).

This mask multiplies the existing midtone mask. It does not alter exposure or
darken the input image: it suppresses Clarity's contribution to dark pixels.
Bright lights at night can still receive enhancement. "Original" means the
image entering Clarity, including earlier effects in the pipeline.
Reversed/equal thresholds are handled without division by zero. The debug view
continues to show the raw extracted detail layer. Old presets default protection
to 0, including when loaded after a preset that enables it.
