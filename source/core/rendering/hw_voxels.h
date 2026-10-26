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
bool RT_VoxelTooFar(int vox, const DVector3& pos, const DVector3& view);

void voxInit();
void voxClear();
int voxDefine(int voxindex, const char* filename);
