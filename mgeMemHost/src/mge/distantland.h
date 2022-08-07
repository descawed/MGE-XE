#pragma once

#include "quadtree.h"
#include "ffeshader.h"
#include "specificrender.h"

#include <string>
#include <unordered_map>
#include <functional>
#include <memory>


#pragma pack(push, 4)
struct DistantStaticParameters {
    D3DXMATRIX view;
    D3DXMATRIX proj;
    D3DXVECTOR4 eyePos;
    float nearViewRange;
    float fogEnd;
    float nearStaticEnd;
    float farStaticEnd;
    float veryFarStaticEnd;
    char worldspace[64];
};

struct DistantReflectionParameters {
    D3DXMATRIX view;
    D3DXMATRIX proj;
    D3DXVECTOR4 eyePos;
    float fogEnd;
    float nearStaticEnd;
    char worldspace[64];
};

struct DistantLandParameters {
    D3DXMATRIX view;
    D3DXMATRIX proj;
    D3DXVECTOR4 eyePos;
    float drawDistance;
};

struct DistantGrassParameters {
    D3DXMATRIX view;
    D3DXMATRIX proj;
    float nearViewRange;
    float fogEnd;
    bool expFog;
    char worldspace[64];
};

struct DistantShadowParameters {
    D3DXMATRIX viewproj;
    char worldspace[64];
};

struct VisibleDistantMeshes {
    DWORD numStatics;
    RenderMesh staticMeshes[100000];
    DWORD numLand;
    RenderMesh landMeshes[100000];
    DWORD numGrass;
    RenderMesh grassMeshes[100000];
    DWORD numReflections;
    RenderMesh reflectionMeshes[100000];
    DWORD numShadow;
    RenderMesh shadowMeshes[100000];
};
#pragma pack(pop)


struct MGEShader;

class DistantLand {
public:
    struct WorldSpace {
        std::unique_ptr<QuadTree> NearStatics;
        std::unique_ptr<QuadTree> FarStatics;
        std::unique_ptr<QuadTree> VeryFarStatics;
        std::unique_ptr<QuadTree> GrassStatics;
    };

    struct RecordedState : RenderedState {
        RecordedState(const RenderedState&);
        ~RecordedState();
        RecordedState(const RecordedState&) = delete;
        RecordedState(RecordedState&&) noexcept;
    };

    static constexpr DWORD fvfWave = D3DFVF_XYZRHW | D3DFVF_TEX2;
    static constexpr int waveTexResolution = 512;
    static constexpr float waveTexWorldRes = 2.5f;
    static constexpr int GrassInstStride = 48;
    static constexpr int MaxGrassElements = 8192;
    static constexpr float kCellSize = 8192.0f;
    static constexpr float kDistantZBias = 5e-6f;
    static constexpr float kDistantNearPlane = 4.0f;
    static constexpr float kMoonTag = 88888.0f;

    static std::unordered_map<std::string, WorldSpace> mapWorldSpaces;
    static QuadTree LandQuadTree;
    static VisibleSet visLand;
    static VisibleSet visDistant;
    static VisibleSet visGrass;

    static D3DXMATRIX smView[2], smProj[2], smViewproj[2];

    static bool initLandscape(HANDLE pipe);
    static bool initDistantStatics(HANDLE pipe);
    static bool loadDistantStatics(HANDLE pipe);
    static bool initDistantStaticsBVH(HANDLE pipe);
    static void getDistantStatics(HANDLE pipe, VisibleDistantMeshes& meshes);
    static void getDistantLand(HANDLE pipe, VisibleDistantMeshes& meshes);
    static void getDistantGrass(HANDLE pipe, VisibleDistantMeshes& meshes);
    static void getDistantReflections(HANDLE pipe, VisibleDistantMeshes& meshes);
    static void getDistantShadows(HANDLE pipe, VisibleDistantMeshes& meshes);

    static void editProjectionZ(D3DMATRIX* m, float zn, float zf);
};