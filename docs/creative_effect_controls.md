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

## Effect order and DoF into Kuwahara

Set Kuwahara's **Render Stage** to **After tonemapping (LDR)**. Its default
position in that stage is immediately after DoF, so it filters the DoF output.
The original HDR stage remains the default for existing presets. HDR and LDR
orders are saved independently, so switching stages preserves both arrangements.

Expand **Effect Order** above the effect settings to move compatible effects up
or down within their stage. Disabled effects retain their positions. An asterisk
marks an order changed since loading/saving the preset. Use **Save** or **Save
As** in the preset controls to keep the arrangement. Loading an older preset
restores the default order and Kuwahara's original HDR stage.

Fixed passes are barriers. In particular, LUT rebuilds the LDR image from HDR,
so it must precede effects that consume that image. Shadow and lighting passes
also stay fixed. The ordering controls affect post callbacks; autofocus, HDR
capture and other pre callbacks retain their original timing.

Plugin API v9 uses flags on existing settings (no struct size change):
`POST_STAGE` is an enum selecting HDR/LDR; `POST_ORDER_HDR` and `POST_ORDER_LDR`
mark integer order settings. Effects must explicitly opt in. Plugins built
against earlier APIs retain their fixed scheduling. Dispatch caches ordered
indices and rebuilds only when scheduling changes; effect objects stay in place.

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
image entering Clarity, including earlier effects in the selected order.
Reversed/equal thresholds are handled without division by zero. The debug view
continues to show the raw extracted detail layer. Old presets default protection
to 0, including when loaded after a preset that enables it.
