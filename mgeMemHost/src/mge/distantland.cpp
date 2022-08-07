
#include "distantland.h"



void DistantLand::getDistantStatics(HANDLE pipe, VisibleDistantMeshes& meshes) {
	DWORD unused;
	DistantStaticParameters params;

	ReadFile(pipe, &params, sizeof(DistantStaticParameters), &unused, 0);

    D3DXMATRIX ds_proj = params.proj, ds_viewproj;
    D3DXVECTOR4 viewsphere(params.eyePos.x, params.eyePos.y, params.eyePos.z, 0);
    float zn = params.nearViewRange - 768.0f, zf = zn;
    float cullDist = params.fogEnd;

    VisibleSet visDistant;
    WorldSpace* currentWorldSpace = &mapWorldSpaces[params.worldspace];

    zf = std::min(params.nearStaticEnd * kCellSize, cullDist);
    if (zn < zf) {
        editProjectionZ(&ds_proj, zn, zf);
        ds_viewproj = params.view * ds_proj;
        ViewFrustum range_frustum(&ds_viewproj);
        viewsphere.w = zf;
        currentWorldSpace->NearStatics->GetVisibleMeshes(range_frustum, viewsphere, visDistant);
    }

    zf = std::min(params.farStaticEnd * kCellSize, cullDist);
    if (zn < zf) {
        editProjectionZ(&ds_proj, zn, zf);
        ds_viewproj = params.view * ds_proj;
        ViewFrustum range_frustum(&ds_viewproj);
        viewsphere.w = zf;
        currentWorldSpace->FarStatics->GetVisibleMeshes(range_frustum, viewsphere, visDistant);
    }

    zf = std::min(params.veryFarStaticEnd * kCellSize, cullDist);
    if (zn < zf) {
        editProjectionZ(&ds_proj, zn, zf);
        ds_viewproj = params.view * ds_proj;
        ViewFrustum range_frustum(&ds_viewproj);
        viewsphere.w = zf;
        currentWorldSpace->VeryFarStatics->GetVisibleMeshes(range_frustum, viewsphere, visDistant);
    }

    visDistant.SortByState();

    meshes.numStatics = visDistant.size();
    visDistant.Copy(meshes.staticMeshes);
}

void DistantLand::getDistantLand(HANDLE pipe, VisibleDistantMeshes& meshes) {
    DWORD unused;
    DistantLandParameters params;

    ReadFile(pipe, &params, sizeof(DistantLandParameters), &unused, 0);

    D3DXMATRIX viewproj = params.view * params.proj;
    D3DXVECTOR4 viewsphere(params.eyePos.x, params.eyePos.y, params.eyePos.z, params.drawDistance * kCellSize);

    ViewFrustum frustum(&viewproj);
    VisibleSet visLand;
    LandQuadTree.GetVisibleMeshes(frustum, viewsphere, visLand);

    meshes.numLand = visLand.size();
    visLand.Copy(meshes.landMeshes);
}

void DistantLand::getDistantGrass(HANDLE pipe, VisibleDistantMeshes& meshes) {
    DWORD unused;
    DistantGrassParameters params;

    ReadFile(pipe, &params, sizeof(DistantGrassParameters), &unused, 0);

    D3DXMATRIX ds_proj = params.proj, ds_viewproj;
    float zn = 4.0f, zf = params.nearViewRange;

    // Don't draw beyond fully fogged distance; early out if frustum is empty
    if (params.expFog) {
        zf = std::min(params.fogEnd, zf);
    }
    if (zf <= zn) {
        return;
    }

    // Create a clipping frustum for visibility determination
    editProjectionZ(&ds_proj, zn, zf);
    ds_viewproj = params.view * ds_proj;

    // Cull and sort
    ViewFrustum range_frustum(&ds_viewproj);
    VisibleSet visGrass;
    WorldSpace* currentWorldSpace = &mapWorldSpaces[params.worldspace];

    currentWorldSpace->GrassStatics->GetVisibleMeshesCoarse(range_frustum, visGrass);
    visGrass.SortByState();

    meshes.numGrass = visGrass.size();
    visGrass.Copy(meshes.grassMeshes);
}

void DistantLand::getDistantReflections(HANDLE pipe, VisibleDistantMeshes& meshes) {
    DWORD unused;
    DistantReflectionParameters params;

    ReadFile(pipe, &params, sizeof(DistantReflectionParameters), &unused, 0);

    // Select appropriate static clipping distance
    D3DXMATRIX ds_proj = params.proj, ds_viewproj;
    float zn = 4.0f, zf = params.nearStaticEnd * kCellSize;

    // Don't draw beyond fully fogged distance; early out if frustum is empty
    zf = std::min(params.fogEnd, zf);
    if (zf <= zn) {
        return;
    }

    // Create a clipping frustum for visibility determination
    editProjectionZ(&ds_proj, zn, zf);
    ds_viewproj = params.view * ds_proj;

    // Cull sort and draw
    VisibleSet visReflected;
    ViewFrustum range_frustum(&ds_viewproj);
    WorldSpace* currentWorldSpace = &mapWorldSpaces[params.worldspace];
    D3DXVECTOR4 viewsphere(params.eyePos.x, params.eyePos.y, params.eyePos.z, zf);

    currentWorldSpace->NearStatics->GetVisibleMeshes(range_frustum, viewsphere, visReflected);
    currentWorldSpace->FarStatics->GetVisibleMeshes(range_frustum, viewsphere, visReflected);
    currentWorldSpace->VeryFarStatics->GetVisibleMeshes(range_frustum, viewsphere, visReflected);
    visReflected.SortByState();

    meshes.numReflections = visReflected.size();
    visReflected.Copy(meshes.reflectionMeshes);
}

void DistantLand::getDistantShadows(HANDLE pipe, VisibleDistantMeshes& meshes) {
    DWORD unused;
    DistantShadowParameters params;

    ReadFile(pipe, &params, sizeof(DistantShadowParameters), &unused, 0);

    VisibleSet visible_set;
    ViewFrustum range_frustum(&params.viewproj);
    WorldSpace* currentWorldSpace = &mapWorldSpaces[params.worldspace];

    currentWorldSpace->NearStatics->GetVisibleMeshesCoarse(range_frustum, visible_set);
    currentWorldSpace->FarStatics->GetVisibleMeshesCoarse(range_frustum, visible_set);
    currentWorldSpace->VeryFarStatics->GetVisibleMeshesCoarse(range_frustum, visible_set);

    meshes.numShadow = visible_set.size();
    visible_set.Copy(meshes.shadowMeshes);
}



// editProjectionZ - Alter the near and far clip planes of a projection matrix
void DistantLand::editProjectionZ(D3DMATRIX* m, float zn, float zf) {
	// Override near and far clip planes
	m->_33 = zf / (zf - zn);
	m->_43 = -zn * zf / (zf - zn);
}