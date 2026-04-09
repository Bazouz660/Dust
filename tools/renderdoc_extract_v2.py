"""
RenderDoc pipeline extraction script v2.
Paste into RenderDoc Python Shell (Window > Python Shell).
Capture must be loaded first.

API types used:
- GetOutputTargets() -> list[Descriptor]          .resource for ResourceId
- GetDepthTarget()   -> Descriptor                .resource for ResourceId
- GetReadOnlyResources() -> list[UsedDescriptor]  .descriptor.resource, .access.index
- GetConstantBlocks()    -> list[UsedDescriptor]  .descriptor.resource, .access.index
- GetColorBlends()       -> list[ColorBlend]      .enabled, .colorBlend, .alphaBlend
- GetTextures() -> list[TextureDescription]        .resourceId, .width, .height, .format
- GetBuffers()  -> list[BufferDescription]         .resourceId, .length
"""
import renderdoc as rd

def analyze(controller):
    # Build lookup dictionaries
    tex_lookup = {}
    for t in controller.GetTextures():
        tex_lookup[t.resourceId] = t

    buf_lookup = {}
    for b in controller.GetBuffers():
        buf_lookup[b.resourceId] = b

    actions = controller.GetRootActions()

    all_actions = []
    def flatten(alist):
        for a in alist:
            all_actions.append(a)
            flatten(a.children)
    flatten(actions)

    # Find non-indexed draws with 3 or 4 verts (fullscreen passes)
    targets = []
    for a in all_actions:
        if (a.flags & rd.ActionFlags.Drawcall) and not (a.flags & rd.ActionFlags.Indexed):
            if a.numIndices in [3, 4]:
                targets.append(a)

    print(f"Found {len(targets)} fullscreen draws\n")

    for idx, action in enumerate(targets):
        controller.SetFrameEvent(action.eventId, False)

        state = controller.GetPipelineState()
        pipe = state.GetGraphicsPipelineObject()
        entry_ps = state.GetShaderEntryPoint(rd.ShaderStage.Pixel)

        ps_id = state.GetShader(rd.ShaderStage.Pixel)
        vs_id = state.GetShader(rd.ShaderStage.Vertex)
        ps_refl = state.GetShaderReflection(rd.ShaderStage.Pixel)

        # list[Descriptor]
        outs = state.GetOutputTargets()
        # Descriptor
        ds = state.GetDepthTarget()
        # list[UsedDescriptor] - each has .access.index and .descriptor.resource
        srvs = state.GetReadOnlyResources(rd.ShaderStage.Pixel)
        # list[UsedDescriptor]
        cbs = state.GetConstantBlocks(rd.ShaderStage.Pixel)
        # list[ColorBlend]
        blends = state.GetColorBlends()

        # D3D11-specific state for depth
        d3d11 = controller.GetD3D11PipelineState()

        print("=" * 70)
        print(f"FULLSCREEN #{idx}  EID={action.eventId}  Draw({action.numIndices})")
        print(f"  VS={vs_id}  PS={ps_id}")

        if ps_refl and ps_refl.debugInfo and ps_refl.debugInfo.files:
            for f in ps_refl.debugInfo.files:
                print(f"  PS source file: {f.filename}")

        # Render targets - outs is list[Descriptor], Descriptor.resource is ResourceId
        for j, rt in enumerate(outs):
            if rt.resource != rd.ResourceId.Null():
                t = tex_lookup.get(rt.resource)
                if t:
                    print(f"  RT{j}: {rt.resource} {t.width}x{t.height} {t.format.Name()}")

        # Depth target - ds is Descriptor
        if ds.resource != rd.ResourceId.Null():
            t = tex_lookup.get(ds.resource)
            if t:
                print(f"  DS: {ds.resource} {t.width}x{t.height} {t.format.Name()}")

        # PS SRVs - srvs is list[UsedDescriptor]
        # UsedDescriptor has .access (DescriptorAccess) and .descriptor (Descriptor)
        for ud in srvs:
            res_id = ud.descriptor.resource
            slot = ud.access.index
            if res_id != rd.ResourceId.Null():
                t = tex_lookup.get(res_id)
                if t:
                    print(f"  SRV[{slot}]: {res_id} {t.width}x{t.height} {t.format.Name()}")
                else:
                    b = buf_lookup.get(res_id)
                    if b:
                        print(f"  SRV[{slot}]: {res_id} buf {b.length}B")

        # PS Constant buffers - cbs is list[UsedDescriptor]
        for ud in cbs:
            res_id = ud.descriptor.resource
            slot = ud.access.index
            if res_id != rd.ResourceId.Null():
                b = buf_lookup.get(res_id)
                sz = b.length if b else 0
                desc = ud.descriptor
                print(f"  CB[{slot}]: {res_id} bufSize={sz} offset={desc.byteOffset} range={desc.byteSize}")

                # Get named variable contents
                if ps_refl:
                    try:
                        cb_vars = controller.GetCBufferVariableContents(
                            pipe, ps_refl.resourceId, rd.ShaderStage.Pixel,
                            entry_ps, slot, res_id,
                            desc.byteOffset, desc.byteSize
                        )
                        for v in cb_vars:
                            printVar(v, "    ")
                    except Exception as e:
                        print(f"    (CB read error: {e})")

        # Blend state - blends is list[ColorBlend]
        if blends and len(blends) > 0:
            b0 = blends[0]
            if b0.enabled:
                print(f"  Blend: ON src={b0.colorBlend.source} dst={b0.colorBlend.destination} op={b0.colorBlend.operation}")
                print(f"  BlendA: src={b0.alphaBlend.source} dst={b0.alphaBlend.destination} op={b0.alphaBlend.operation}")
            else:
                print(f"  Blend: OFF")

        # Depth state (D3D11 specific)
        if d3d11:
            dss = d3d11.outputMerger.depthStencilState
            print(f"  Depth: enable={dss.depthEnable} write={dss.depthWrites} func={dss.depthFunction}")

        # Disassemble pixel shader (first 60 lines)
        if ps_refl:
            try:
                disasm_targets = controller.GetDisassemblyTargets(True)
                if len(disasm_targets) > 0:
                    disasm = controller.DisassembleShader(pipe, ps_refl, disasm_targets[0])
                    lines = disasm.split('\n')
                    print(f"  PS Disassembly ({len(lines)} lines, showing first 60):")
                    for line in lines[:60]:
                        print(f"    {line}")
                    if len(lines) > 60:
                        print(f"    ... ({len(lines) - 60} more lines)")
            except Exception as e:
                print(f"  (Disassembly error: {e})")

        print()

    print("DONE")


def printVar(v, indent=''):
    if len(v.members) == 0:
        valstr = ""
        for r in range(0, v.rows):
            for c in range(0, v.columns):
                valstr += '%.6f ' % v.value.f32v[r * v.columns + c]
        print(f"{indent}{v.name}: {valstr}")
    else:
        print(f"{indent}{v.name}:")
        for m in v.members:
            printVar(m, indent + '  ')


pyrenderdoc.Replay().BlockInvoke(analyze)
