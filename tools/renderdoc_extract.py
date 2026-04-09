"""
RenderDoc Python script to extract rendering pipeline info.
Paste this into RenderDoc's Python Shell (Window > Python Shell)
with a Kenshi capture loaded.

It will find the fog pass and extract all relevant details.
"""

import renderdoc as rd

def extract_pipeline_info(controller):
    # Get all draw calls (flattened)
    draws = controller.GetRootActions()

    all_actions = []
    def flatten(actions, depth=0):
        for a in actions:
            all_actions.append((a, depth))
            flatten(a.children, depth + 1)
    flatten(draws)

    # Find all fullscreen Draw(3) and Draw(4) calls in Colour Pass #2
    # These are our post-processing passes
    fullscreen_draws = []
    for action, depth in all_actions:
        flags = action.flags
        # Look for Draw calls (not DrawIndexed) that draw 3 or 4 vertices
        if (flags & rd.ActionFlags.Drawcall) and not (flags & rd.ActionFlags.Indexed):
            if action.numIndices in [3, 4]:
                fullscreen_draws.append(action)

    print("=" * 80)
    print(f"TOTAL ACTIONS: {len(all_actions)}")
    print(f"FULLSCREEN DRAWS (3 or 4 verts): {len(fullscreen_draws)}")
    print("=" * 80)

    for i, action in enumerate(fullscreen_draws):
        eid = action.eventId
        verts = action.numIndices

        # Move to this event
        controller.SetFrameEvent(eid, False)

        # Get pipeline state
        state = controller.GetPipelineState()

        # Get pixel shader
        ps = state.GetShader(rd.ShaderStage.Pixel)
        ps_refl = state.GetShaderReflection(rd.ShaderStage.Pixel)
        vs = state.GetShader(rd.ShaderStage.Vertex)

        # Get render targets
        om_targets = state.GetOutputTargets()
        depth_target = state.GetDepthTarget()

        # Get PS shader resources (SRVs)
        ps_srvs = state.GetReadOnlyResources(rd.ShaderStage.Pixel)

        # Get PS constant buffers
        ps_cbs = state.GetConstantBuffers(rd.ShaderStage.Pixel)

        # Get blend state
        blend = state.GetColorBlend()

        # Get depth state
        depth_state = state.GetDepthState()

        print(f"\n{'='*80}")
        print(f"FULLSCREEN DRAW #{i} — EID {eid}, Draw({verts})")
        print(f"{'='*80}")

        # Shaders
        print(f"\n  Vertex Shader:  {vs}")
        print(f"  Pixel Shader:   {ps}")
        if ps_refl:
            print(f"  PS Debug Name:  {ps_refl.debugInfo.debugName if ps_refl.debugInfo else 'N/A'}")

        # Render targets
        print(f"\n  Render Targets:")
        for j, rt in enumerate(om_targets):
            if rt.resourceId != rd.ResourceId.Null():
                desc = controller.GetTexture(rt.resourceId)
                if desc:
                    print(f"    RT{j}: ID={rt.resourceId} ({desc.width}x{desc.height}, {desc.format.Name()})")
                else:
                    print(f"    RT{j}: ID={rt.resourceId}")

        if depth_target.resourceId != rd.ResourceId.Null():
            desc = controller.GetTexture(depth_target.resourceId)
            if desc:
                print(f"    DS:  ID={depth_target.resourceId} ({desc.width}x{desc.height}, {desc.format.Name()})")

        # PS SRVs
        print(f"\n  PS Shader Resources:")
        for bind in ps_srvs:
            for j, res in enumerate(bind.resources):
                if res.resourceId != rd.ResourceId.Null():
                    desc = controller.GetTexture(res.resourceId)
                    if desc and desc.width > 0:
                        print(f"    SRV slot {bind.firstIndex + j}: ID={res.resourceId} ({desc.width}x{desc.height}, {desc.format.Name()})")
                    else:
                        buf = controller.GetBuffer(res.resourceId)
                        if buf:
                            print(f"    SRV slot {bind.firstIndex + j}: ID={res.resourceId} (Buffer, {buf.length} bytes)")

        # PS Constant Buffers
        print(f"\n  PS Constant Buffers:")
        for bind in ps_cbs:
            for j, cb in enumerate(bind.resources):
                if cb.resourceId != rd.ResourceId.Null():
                    slot = bind.firstIndex + j
                    buf = controller.GetBuffer(cb.resourceId)
                    size = buf.length if buf else 0
                    print(f"    CB slot {slot}: ID={cb.resourceId} ({size} bytes, offset={cb.byteOffset}, size={cb.byteSize})")

                    # Try to read constant buffer contents
                    if cb.byteSize > 0 and cb.byteSize <= 2048:
                        try:
                            data = controller.GetBufferData(cb.resourceId, cb.byteOffset, cb.byteSize)
                            # Interpret as floats
                            import struct
                            num_floats = len(data) // 4
                            floats = struct.unpack(f'{num_floats}f', bytes(data[:num_floats*4]))
                            print(f"    CB{slot} as floats ({num_floats} values):")
                            for k in range(0, num_floats, 4):
                                row = floats[k:k+4]
                                row_str = ", ".join(f"{v:12.6f}" for v in row)
                                print(f"      [{k:3d}-{k+len(row)-1:3d}]: {row_str}")
                        except Exception as e:
                            print(f"    (Could not read CB data: {e})")

        # Blend state
        print(f"\n  Blend State:")
        if blend and len(blend.blends) > 0:
            b = blend.blends[0]
            print(f"    Enabled:      {b.enabled}")
            if b.enabled:
                print(f"    Color Src:    {b.colorBlend.source}")
                print(f"    Color Dst:    {b.colorBlend.destination}")
                print(f"    Color Op:     {b.colorBlend.operation}")
                print(f"    Alpha Src:    {b.alphaBlend.source}")
                print(f"    Alpha Dst:    {b.alphaBlend.destination}")
                print(f"    Alpha Op:     {b.alphaBlend.operation}")
                print(f"    Write Mask:   {b.writeMask}")

        # Depth state
        print(f"\n  Depth State:")
        print(f"    Depth Test:   {depth_state.depthEnabled}")
        print(f"    Depth Write:  {depth_state.depthWrites}")
        if depth_state.depthEnabled:
            print(f"    Depth Func:   {depth_state.depthFunction}")

    print("\n" + "=" * 80)
    print("DONE — Pipeline extraction complete")
    print("=" * 80)


# Run it
controller = pyrenderdoc.Replay().GetController()
if controller is None:
    # Try alternative access methods
    try:
        extract_pipeline_info(pyrenderdoc.CurController())
    except:
        print("ERROR: Could not get replay controller.")
        print("Make sure a capture is loaded and you can see the texture viewer.")
        print("Try: Window > Python Shell, then paste this script.")
else:
    extract_pipeline_info(controller)
