
#include "distantland.h"
#include "dlformat.h"
#include <memory>
#include <iostream>



using std::string;
using std::string_view;
using std::vector;
using std::unordered_map;

unordered_map<std::string, DistantLand::WorldSpace> DistantLand::mapWorldSpaces;
QuadTree DistantLand::LandQuadTree;
VisibleSet DistantLand::visLand;
VisibleSet DistantLand::visDistant;
VisibleSet DistantLand::visGrass;


static vector<DistantStatic> DistantStatics;
static unordered_map< string, vector<UsedDistantStatic> > UsedDistantStatics;

bool DistantLand::initDistantStatics(HANDLE pipe) {
    if (!loadDistantStatics(pipe)) {
        return false;
    }

    std::cout << "initDistantStaticsBVH" << std::endl;
    if (!initDistantStaticsBVH(pipe)) {
        return false;
    }

    // Remove UsedDistantStatic, DistantStatic, and DistantSubset objects
    UsedDistantStatics.clear();
    DistantStatics.clear();

    return true;
}

class membuf_reader {
    char* ptr;

public:
    membuf_reader(char* buf) : ptr(buf) {}

    template <typename T>
    inline void read(T* dest, size_t size) {
        memcpy((char*)dest, ptr, size);
        ptr += size;
    }

    inline char* get() {
        return ptr;
    }

    inline void advance(size_t size) {
        ptr += size;
    }
};

bool DistantLand::loadDistantStatics(HANDLE pipe) {
    DWORD unused;

    size_t DistantStaticCount = 0;
    ReadFile(pipe, &DistantStaticCount, 4, &unused, 0);
    DistantStatics.resize(DistantStaticCount);

    std::cout << "Num distant statics: " << DistantStaticCount << std::endl;

    for (auto& i : DistantStatics) {
        int numSubsets;
        ReadFile(pipe, &numSubsets, 4, &unused, 0);
        ReadFile(pipe, &i.sphere.radius, 4, &unused, 0);
        ReadFile(pipe, &i.sphere.center, 12, &unused, 0);
        ReadFile(pipe, &i.type, 1, &unused, 0);

        i.subsets.resize(numSubsets);
        i.aabbMin = D3DXVECTOR3(FLT_MAX, FLT_MAX, FLT_MAX);
        i.aabbMax = D3DXVECTOR3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

        std::cout << "\tNum subsets: " << numSubsets << std::endl;
        for (auto& subset : i.subsets) {
            ReadFile(pipe, &subset, sizeof(DistantSubset), &unused, 0);

            // Update parent AABB
            i.aabbMin.x = std::min(i.aabbMin.x, subset.aabbMin.x);
            i.aabbMin.y = std::min(i.aabbMin.y, subset.aabbMin.y);
            i.aabbMin.z = std::min(i.aabbMin.z, subset.aabbMin.z);
            i.aabbMax.x = std::max(i.aabbMax.x, subset.aabbMax.x);
            i.aabbMax.y = std::max(i.aabbMax.y, subset.aabbMax.y);
            i.aabbMax.z = std::max(i.aabbMax.z, subset.aabbMax.z);
        }
    }

    // Load statics references
    mapWorldSpaces.clear();
    for (size_t nWorldSpace = 0; true; ++nWorldSpace) {
        size_t UsedDistantStaticCount = 0;
        decltype(UsedDistantStatics)::iterator iCell;

        ReadFile(pipe, &UsedDistantStaticCount, 4, &unused, 0);
        if (nWorldSpace != 0 && UsedDistantStaticCount == 0) {
            break;
        }

        if (nWorldSpace == 0) {
            mapWorldSpaces.insert(make_pair(string(), WorldSpace()));
            iCell = UsedDistantStatics.insert(make_pair(string(), vector<UsedDistantStatic>())).first;
            if (UsedDistantStaticCount == 0) {
                continue;
            }
            std::cout << "Main worldspace contains " << UsedDistantStaticCount << " statics" << std::endl;
        } else {
            char cellname[64];
            ReadFile(pipe, &cellname, 64, &unused, 0);
            iCell = UsedDistantStatics.insert(make_pair(string(cellname), vector<UsedDistantStatic>())).first;
            mapWorldSpaces.insert(make_pair(string(cellname), WorldSpace()));
            std::cout << "Worldspace " << cellname << " contains " << UsedDistantStaticCount << " statics" << std::endl;
        }

        size_t UsedDistantStaticDataSize = UsedDistantStaticCount * 32;
        auto UsedDistantStaticData = std::make_unique<char[]>(UsedDistantStaticDataSize);
        ReadFile(pipe, UsedDistantStaticData.get(), UsedDistantStaticDataSize, &unused, 0);
        membuf_reader udsReader(UsedDistantStaticData.get());

        vector<UsedDistantStatic>& ThisWorldStatics = iCell->second;
        ThisWorldStatics.reserve(UsedDistantStaticCount);

        for (size_t i = 0; i < UsedDistantStaticCount; ++i) {
            UsedDistantStatic NewUsedStatic;
            float yaw, pitch, roll, scale;

            udsReader.read(&NewUsedStatic.staticRef, 4);
            udsReader.read(&NewUsedStatic.pos, 12);
            udsReader.read(&yaw, 4);
            udsReader.read(&pitch, 4);
            udsReader.read(&roll, 4);
            udsReader.read(&scale, 4);

            DistantStatic* stat = &DistantStatics[NewUsedStatic.staticRef];
            if (scale == 0.0f) {
                scale = 1.0f;
            }
            NewUsedStatic.scale = scale;

            D3DXMATRIX transmat, rotmatx, rotmaty, rotmatz, scalemat;
            D3DXMatrixTranslation(&transmat, NewUsedStatic.pos.x, NewUsedStatic.pos.y, NewUsedStatic.pos.z);
            D3DXMatrixRotationX(&rotmatx, -yaw);
            D3DXMatrixRotationY(&rotmaty, -pitch);
            D3DXMatrixRotationZ(&rotmatz, -roll);
            D3DXMatrixScaling(&scalemat, scale, scale, scale);
            NewUsedStatic.transform = scalemat * rotmatz * rotmaty * rotmatx * transmat;
            NewUsedStatic.sphere = NewUsedStatic.GetBoundingSphere(stat->sphere);
            NewUsedStatic.box = NewUsedStatic.GetBoundingBox(stat->aabbMin, stat->aabbMax);

            ThisWorldStatics.push_back(NewUsedStatic);
        }
    }

    return true;
}

bool DistantLand::initDistantStaticsBVH(HANDLE pipe) {
    DWORD unused;
    float farStaticMinSize, veryFarStaticMinSize;

    ReadFile(pipe, &farStaticMinSize, sizeof(farStaticMinSize), &unused, 0);
    ReadFile(pipe, &veryFarStaticMinSize, sizeof(veryFarStaticMinSize), &unused, 0);

    std::cout << "farStaticMinSize: " << farStaticMinSize << ", veryFarStaticMinSize: " << veryFarStaticMinSize << std::endl;

    for (auto& iWS : mapWorldSpaces) {
        auto it = UsedDistantStatics.find(iWS.first);
        std::cout << "initDistantStaticsBVH: " << it->first << std::endl;
        vector<UsedDistantStatic>& uds = it->second;

        // Initialize quadtrees
        iWS.second.NearStatics = std::make_unique<QuadTree>();
        iWS.second.FarStatics = std::make_unique<QuadTree>();
        iWS.second.VeryFarStatics = std::make_unique<QuadTree>();
        iWS.second.GrassStatics = std::make_unique<QuadTree>();
        QuadTree* NQTR = iWS.second.NearStatics.get();
        QuadTree* FQTR = iWS.second.FarStatics.get();
        QuadTree* VFQTR = iWS.second.VeryFarStatics.get();
        QuadTree* GQTR = iWS.second.GrassStatics.get();

        // Calclulate optimal initial quadtree size
        D3DXVECTOR2 aabbMax = D3DXVECTOR2(-FLT_MAX, -FLT_MAX);
        D3DXVECTOR2 aabbMin = D3DXVECTOR2(FLT_MAX, FLT_MAX);

        // Find xyz bounds
        std::cout << "\tFinding xyz bounds" << std::endl;
        for (const auto& i : uds) {
            float x = i.pos.x, y = i.pos.y, r = i.sphere.radius;

            aabbMax.x = std::max(x + r, aabbMax.x);
            aabbMax.y = std::max(y + r, aabbMax.y);
            aabbMin.x = std::min(aabbMin.x, x - r);
            aabbMin.y = std::min(aabbMin.y, y - r);
        }

        float box_size = std::max(aabbMax.x - aabbMin.x, aabbMax.y - aabbMin.y);
        D3DXVECTOR2 box_center = 0.5 * (aabbMax + aabbMin);

        NQTR->SetBox(box_size, box_center);
        FQTR->SetBox(box_size, box_center);
        VFQTR->SetBox(box_size, box_center);
        GQTR->SetBox(box_size, box_center);

        std::cout << "\tIterating " << uds.size() << " statics" << std::endl;
        for (const auto& i : uds) {
            DistantStatic* stat = &DistantStatics[i.staticRef];
            QuadTree* targetQTR;

            // Use transformed radius
            float radius = i.sphere.radius;

            // Buildings are treated as larger objects, as they are typically
            // smaller component meshes combined to make a single building
            if (stat->type == STATIC_BUILDING) {
                radius *= 2.0f;
            }

            // Select quadtree to place object in
            switch (stat->type) {
            case STATIC_AUTO:
            case STATIC_TREE:
            case STATIC_BUILDING:
                if (radius <= farStaticMinSize) {
                    targetQTR = NQTR;
                } else if (radius <= veryFarStaticMinSize) {
                    targetQTR = FQTR;
                } else {
                    targetQTR = VFQTR;
                }
                break;

            case STATIC_GRASS:
                targetQTR = GQTR;
                break;

            case STATIC_NEAR:
                targetQTR = NQTR;
                break;

            case STATIC_FAR:
                targetQTR = FQTR;
                break;

            case STATIC_VERY_FAR:
                targetQTR = VFQTR;
                break;

            default:
                continue;
            }

            // Add sub-meshes to appropriate quadtree
            if (stat->type == STATIC_BUILDING) {
                std::cout << "\tAdding " << stat->subsets.size() << " building mesh subsets" << std::endl;
                // Use model bound so that all building parts have coherent visibility
                for (auto& s : stat->subsets) {
                    targetQTR->AddMesh(
                        i.sphere,
                        i.box,
                        i.transform,
                        s.tex,
                        s.verts,
                        s.vbuffer,
                        s.faces,
                        s.ibuffer,
                        s.hasalpha
                    );
                }
            } else {
                std::cout << "\tAdding " << stat->subsets.size() << " individual mesh subsets" << std::endl;
                // Use individual mesh bounds
                for (auto& s : stat->subsets) {
                    targetQTR->AddMesh(
                        i.GetBoundingSphere(s.sphere),
                        i.GetBoundingBox(s.aabbMin, s.aabbMax),
                        i.transform,
                        s.tex,
                        s.verts,
                        s.vbuffer,
                        s.faces,
                        s.ibuffer,
                        s.hasalpha
                    );
                }
            }
        }

        std::cout << "Optimizing and calculating volumes" << std::endl;

        NQTR->Optimize();
        NQTR->CalcVolume();
        FQTR->Optimize();
        FQTR->CalcVolume();
        VFQTR->Optimize();
        VFQTR->CalcVolume();
        GQTR->Optimize();
        GQTR->CalcVolume();

        uds.clear();
    }

    return true;
}

bool DistantLand::initLandscape(HANDLE pipe) {
    // read mesh count
    DWORD mesh_count, unused;
    ptr32 texWorldColour;
    ReadFile(pipe, &mesh_count, 4, &unused, 0);
    ReadFile(pipe, &texWorldColour, 4, &unused, 0);

    vector<LandMesh> meshesLand;
    meshesLand.resize(mesh_count);

    if (!meshesLand.empty()) {
        D3DXVECTOR2 qtmin(FLT_MAX, FLT_MAX), qtmax(-FLT_MAX, -FLT_MAX);
        D3DXMATRIX world;
        D3DXMatrixIdentity(&world);

        // Load meshes and calculate max size of quadtree
        for (auto& i : meshesLand) {
            ReadFile(pipe, &i, sizeof(LandMesh), &unused, 0);

            qtmin.x = std::min(qtmin.x, i.sphere.center.x - i.sphere.radius);
            qtmin.y = std::min(qtmin.y, i.sphere.center.y - i.sphere.radius);
            qtmax.x = std::max(qtmax.x, i.sphere.center.x + i.sphere.radius);
            qtmax.y = std::max(qtmax.y, i.sphere.center.y + i.sphere.radius);
        }

        LandQuadTree.SetBox(std::max(qtmax.x - qtmin.x, qtmax.y - qtmin.y), 0.5 * (qtmax + qtmin));

        // Add meshes to the quadtree
        for (auto& i : meshesLand) {
            LandQuadTree.AddMesh(i.sphere, i.box, world, texWorldColour, i.verts, i.vbuffer, i.faces, i.ibuffer, false);
        }
    }

    LandQuadTree.CalcVolume();

    return true;
}