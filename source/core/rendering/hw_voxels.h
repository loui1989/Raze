#pragma once

#include <stdint.h>
#include "model.h"
#include "model_kvx.h"
#include "intvec.h"

constexpr int MAXVOXELS = 2048;

struct voxmodel_t // : public mdmodel_t
{
    FVoxelModel* model = nullptr;
    float scale, bscale, zadd, yoffset;
    vec3_t siz;
    FVector3 piv;
    int32_t is8bit;
};



extern float voxscale[];
extern voxmodel_t* voxmodels[MAXVOXELS];
extern FixedBitArray<MAXVOXELS> voxrotate;
extern FixedBitArray<MAXVOXELS> voxdefined;	// from a definitions file (a voxel pack), not the game's own

// ray tracing: a voxel model this far from the view is drawn as its sprite (rt_voxel_distance)
struct tspritetype;
bool RT_VoxelTooFar(int vox, const tspritetype* spr, const DVector3& view);
// ray tracing: a voxel model's coarser mesh (its second mip level: a quarter of the faces or
// so), for the models over the frame's budget; nullptr if the voxel has none
voxmodel_t* RT_VoxelLod(int vox);

// where the top of the voxel model a sprite is drawn as will be (as HWSprite::ProcessVoxel
// places it); false if the sprite is drawn as a sprite
struct tspritetype;
bool VoxelTopZ(tspritetype* spr, double& top);

void voxInit();
void voxClear();
int voxDefine(int voxindex, const char* filename);
