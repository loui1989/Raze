// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2021 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** hw_voxels.cpp
**
** voxel handling.
**
**/ 

#include "build.h"
#include "voxels.h"
#include "hw_voxels.h"
#include "tiletexture.h"
#include "gamecontrol.h"
#include "hw_models.h"
#include "v_video.h"

static int voxlumps[MAXVOXELS];
float voxscale[MAXVOXELS];
voxmodel_t* voxmodels[MAXVOXELS];
FixedBitArray<MAXVOXELS> voxrotate;
FixedBitArray<MAXVOXELS> voxdefined;


void voxInit()
{
	for (auto& v : voxscale) v = 1.f;
	voxrotate.Zero();
	voxdefined.Zero();
}

// ray tracing: the voxel models' coarser meshes (their second mip level), made when a
// model is first wanted that way
static voxmodel_t* voxlods[MAXVOXELS];

voxmodel_t* RT_VoxelLod(int vox)
{
	if (vox < 0 || vox >= MAXVOXELS || !voxmodels[vox] || !voxmodels[vox]->model) return nullptr;
	if (!voxlods[vox])
	{
		FVoxel* voxel = voxmodels[vox]->model->GetVoxel();
		if (!voxel || voxel->NumMips < 2 || voxel->Mips[1].SizeX <= 0 || !voxel->Mips[1].OffsetX) return nullptr;
		auto vm = new voxmodel_t;
		*vm = *voxmodels[vox];
		vm->model = new FVoxelModel(voxel, 1, voxmodels[vox]->model->GetPaletteTexture());
		FHWModelRenderer mr(*screen->RenderState(), 0);
		vm->model->BuildVertexBuffer(&mr);
		voxlods[vox] = vm;
	}
	return voxlods[vox];
}

void voxClear()
{
	for (auto& vox : voxlods)
	{
		if (vox)
		{
			delete vox->model;
			delete vox;
		}
		vox = nullptr;
	}
	for (auto& vox : voxmodels)
	{
		if (vox)
		{
			delete vox->model;
			delete vox;
		}
		vox = nullptr;
	}
}

int voxDefine(int voxindex, const char* filename)
{
	if ((unsigned)voxindex >= MAXVOXELS)
		return -1;

	int i = fileSystem.FindFile(filename);
	voxlumps[voxindex] = i;
	if (i >= 0) voxdefined.Set(voxindex);
	return i < 0 ? -1 : 0;
}

static voxmodel_t* voxload(int lumpnum)
{
	FVoxel* voxel = R_LoadKVX(lumpnum);
	if (voxel != nullptr)
	{
		voxmodel_t* vm = new voxmodel_t;
		*vm = {};
		auto pivot = voxel->Mips[0].Pivot;
		//vm->mdnum = 1; //VOXel model id
		vm->scale = vm->bscale = 1.f;
		vm->piv.X = float(pivot.X);
		vm->piv.Y = float(pivot.Y);
		vm->piv.Z = float(pivot.Z);
		vm->siz.X = voxel->Mips[0].SizeX;
		vm->siz.Y = voxel->Mips[0].SizeY;
		vm->siz.Z = voxel->Mips[0].SizeZ;
		vm->is8bit = true;
		voxel->Mips[0].Pivot.Zero();  // Needs to be taken out of the voxel data because it gets baked into the vertex buffer which we cannot use here.
		vm->model = new FVoxelModel(voxel, true);
		return vm;
	}
	return nullptr;
}

void LoadVoxelModels()
{
	for (int i = 0; i < MAXVOXELS; i++)
	{
		int lumpnum = voxlumps[i];
		if (lumpnum > 0)
		{
			voxmodels[i] = voxload(lumpnum);
			if (voxmodels[i])
				voxmodels[i]->scale = voxscale[i];
			else
				Printf("Unable to load voxel from %s\n", fileSystem.GetFileFullPath(lumpnum).c_str());
		}
		else 
		{
			auto index = fileSystem.FindResource(i, "KVX");
			if (index >= 0)
			{
				voxmodels[i] = voxload(index);
			}
		}
	}
}

