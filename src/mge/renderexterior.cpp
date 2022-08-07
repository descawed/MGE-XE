
#include "distantland.h"
#include "distantshader.h"
#include "configuration.h"
#include "mwbridge.h"
#include "proxydx/d3d8header.h"

#include <algorithm>



// renderSky - Render atmosphere scattering sky layer and other recorded draw calls on top
void DistantLand::renderSky() {
    // Recorded renders
    const auto& recordSky_const = recordSky;
    for (const auto& i : recordSky_const) {
        // Set variables in main effect; variables are shared via effect pool
        effect->SetTexture(ehTex0, i.texture);
        if (i.texture) {
            // Textured object; draw as normal in shader,
            // except moon shadow (prevents stars shining through moons) which
            // requires colour to be replaced with atmosphere scattering colour
            bool isMoonShadow = i.destBlend == D3DBLEND_INVSRCALPHA && !i.useLighting;

            effect->SetBool(ehHasAlpha, true);
            effect->SetBool(ehHasBones, isMoonShadow);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, 1);
            device->SetRenderState(D3DRS_SRCBLEND, i.srcBlend);
            device->SetRenderState(D3DRS_DESTBLEND, i.destBlend);
        } else {
            // Sky; perform atmosphere scattering in shader
            effect->SetBool(ehHasAlpha, false);
            effect->SetBool(ehHasBones, true);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, 0);
        }

        effect->SetMatrix(ehWorld, &i.worldTransforms[0]);
        effect->CommitChanges();

        device->SetStreamSource(0, i.vb, i.vbOffset, i.vbStride);
        device->SetIndices(i.ib);
        device->SetFVF(i.fvf);
        device->DrawIndexedPrimitive(i.primType, i.baseIndex, i.minIndex, i.vertCount, i.startIndex, i.primCount);
    }
}

void DistantLand::renderDistantLand(ID3DXEffect* e, const D3DXMATRIX* view, const D3DXMATRIX* proj) {
    DistantLandParameters params {
        *view, *proj, eyePos, Configuration.DL.DrawDist,
    };

    // Cull and draw
    DWORD command = 1, unused;
    WriteFile(memHostPipe, &command, sizeof(command), &unused, 0);
    WriteFile(memHostPipe, &params, sizeof(params), &unused, 0);

    D3DXMATRIX world;

    D3DXMatrixIdentity(&world);
    effect->SetMatrix(ehWorld, &world);

    effect->SetTexture(ehTex0, texWorldColour);
    effect->SetTexture(ehTex1, texWorldNormals);
    effect->SetTexture(ehTex2, texWorldDetail);
    e->CommitChanges();

    device->SetVertexDeclaration(LandDecl);

    // wait for search to finish
    ReadFile(memHostPipe, &command, sizeof(command), &unused, 0);

    visibleDistant->Render(LAND, device, 0, 0, 0, 0, 0, SIZEOFLANDVERT);
}

void DistantLand::renderDistantLandZ() {
    D3DXMATRIX world;

    D3DXMatrixIdentity(&world);
    effect->SetMatrix(DistantLand::ehWorld, &world);
    effectDepth->CommitChanges();

    // Draw with cached vis set
    device->SetVertexDeclaration(LandDecl);
    visibleDistant->Render(LAND, device, 0, 0, 0, 0, 0, SIZEOFLANDVERT);
}

void DistantLand::cullDistantStatics(const D3DXMATRIX* view, const D3DXMATRIX* proj) {
    DistantStaticParameters params {
        *view, *proj, eyePos, nearViewRange, fogEnd, Configuration.DL.NearStaticEnd, Configuration.DL.FarStaticEnd, Configuration.DL.VeryFarStaticEnd, { 0, },
    };
    strcpy(params.worldspace, cellname.c_str());

    DWORD command = 2, unused;
    WriteFile(memHostPipe, &command, sizeof(command), &unused, 0);
    WriteFile(memHostPipe, &params, sizeof(params), &unused, 0);
}

void DistantLand::renderDistantStatics() {
    if (!MWBridge::get()->IsExterior()) {
        // Set clipping to stop large architectural meshes (that don't match exactly)
        // from visible overdrawing and causing z-buffer occlusion
        float clipAt = nearViewRange - 768.0f;
        D3DXPLANE clipPlane(0, 0, clipAt, -(mwProj._33 * clipAt + mwProj._43));
        device->SetClipPlane(0, clipPlane);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 1);
    }

    device->SetVertexDeclaration(StaticDecl);

    DWORD result, unused;
    ReadFile(memHostPipe, &result, sizeof(result), &unused, 0);
    visibleDistant->Render(STATIC, device, effect, effect, &ehTex0, 0, &ehWorld, SIZEOFSTATICVERT);

    device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
}
