"""
Simpler RenderDoc extraction script.
Paste this into RenderDoc's Python Shell (Window > Python Shell).
Make sure a capture is loaded first.
"""
import struct
import renderdoc as rd

def analyze(ctx, controller):
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
        pipe = controller.GetPipelineState()

        ps = pipe.GetShader(rd.ShaderStage.Pixel)
        vs = pipe.GetShader(rd.ShaderStage.Vertex)
        ps_refl = pipe.GetShaderReflection(rd.ShaderStage.Pixel)

        outs = pipe.GetOutputTargets()
        ds = pipe.GetDepthTarget()
        srvs = pipe.GetReadOnlyResources(rd.ShaderStage.Pixel)
        cbs = pipe.GetConstantBuffers(rd.ShaderStage.Pixel)
        blend_state = pipe.GetColorBlend()
        depth_state = pipe.GetDepthState()

        print("=" * 70)
        print(f"FULLSCREEN #{idx}  EID={action.eventId}  Draw({action.numIndices})")
        print(f"  VS={vs}  PS={ps}")

        # PS debug name
        if ps_refl and ps_refl.debugInfo:
            print(f"  PS name: {ps_refl.debugInfo.debugName}")

        # Render targets
        for j, rt in enumerate(outs):
            if rt.resourceId != rd.ResourceId.Null():
                t = controller.GetTexture(rt.resourceId)
                if t and t.width > 0:
                    print(f"  RT{j}: {rt.resourceId} {t.width}x{t.height} {t.format.Name()}")

        if ds.resourceId != rd.ResourceId.Null():
            t = controller.GetTexture(ds.resourceId)
            if t and t.width > 0:
                print(f"  DS: {ds.resourceId} {t.width}x{t.height} {t.format.Name()}")

        # SRVs
        for bind in srvs:
            for j, r in enumerate(bind.resources):
                if r.resourceId != rd.ResourceId.Null():
                    slot = bind.firstIndex + j
                    t = controller.GetTexture(r.resourceId)
                    if t and t.width > 0:
                        print(f"  SRV[{slot}]: {r.resourceId} {t.width}x{t.height} {t.format.Name()}")
                    else:
                        b = controller.GetBuffer(r.resourceId)
                        if b:
                            print(f"  SRV[{slot}]: {r.resourceId} buf {b.length}B")

        # Constant buffers - print contents as floats
        for bind in cbs:
            for j, cb in enumerate(bind.resources):
                if cb.resourceId != rd.ResourceId.Null():
                    slot = bind.firstIndex + j
                    sz = cb.byteSize
                    print(f"  CB[{slot}]: {cb.resourceId} size={sz}")
                    if 0 < sz <= 2048:
                        try:
                            raw = controller.GetBufferData(cb.resourceId, cb.byteOffset, sz)
                            nf = len(raw) // 4
                            vals = struct.unpack(f'{nf}f', bytes(raw[:nf*4]))
                            for k in range(0, nf, 4):
                                chunk = vals[k:k+4]
                                s = " ".join(f"{v:>12.4f}" for v in chunk)
                                print(f"    [{k:3d}]: {s}")
                        except Exception as e:
                            print(f"    (read error: {e})")

        # Blend
        if blend_state and len(blend_state.blends) > 0:
            b0 = blend_state.blends[0]
            if b0.enabled:
                print(f"  Blend: src={b0.colorBlend.source} dst={b0.colorBlend.destination} op={b0.colorBlend.operation}")
            else:
                print(f"  Blend: disabled")

        # Depth
        print(f"  Depth: test={depth_state.depthEnabled} write={depth_state.depthWrites}")

        print()

    print("DONE")


# Entry point - works with RenderDoc's pyrenderdoc
def run(ctx):
    ctx.Replay().BlockInvoke(lambda c: analyze(ctx, c))

pyrenderdoc.Replay().BlockInvoke(lambda c: analyze(pyrenderdoc, c))
