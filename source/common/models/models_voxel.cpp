// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2010-2016 Christoph Oelckers
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
** gl_voxels.cpp
**
** Voxel management
**
**/

#include "filesystem.h"
#include "colormatcher.h"
#include "bitmap.h"
#include "model_kvx.h"
#include "image.h"
#include "texturemanager.h"
#include "modelrenderer.h"
#include "voxels.h"
#include "texturemanager.h"
#include "palettecontainer.h"
#include "textures.h"
#include "imagehelpers.h"
#include "c_cvars.h"

#include <algorithm>
#include <vector>

CVARD(Bool, r_voxel_merge, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "voxel models: only the faces that can be seen, neighbours of one colour merged (next start)")

#ifdef _MSC_VER
#pragma warning(disable:4244) // warning C4244: conversion from 'double' to 'float', possible loss of data
#endif

//===========================================================================
//
// Creates a 16x16 texture from the palette so that we can
// use the existing palette manipulation code to render the voxel
// Otherwise all shaders had to be duplicated and the non-shader code
// would be a lot less efficient.
//
//===========================================================================

class FVoxelTexture : public FImageSource
{
public:
	FVoxelTexture(FVoxel *voxel);

	int CopyPixels(FBitmap *bmp, int conversion, int frame = 0) override;
	PalettedPixels CreatePalettedPixels(int conversion, int frame = 0) override;
	const uint8_t *GetVoxelPalette() const { return SourceVox->Palette.Size() >= 768 ? SourceVox->Palette.Data() : nullptr; }

protected:
	FVoxel *SourceVox;
};

// ray tracing: is 'img' a voxel model's palette texture (16 x 16 texels, texel y * 16 + x
// is colour index y * 16 + x)? 'palette': the voxel's own 6 bit palette, null for the game's
bool IsVoxelPaletteTexture(FImageSource *img, const uint8_t **palette)
{
	auto vt = dynamic_cast<FVoxelTexture *>(img);
	if (!vt) return false;
	if (palette) *palette = vt->GetVoxelPalette();
	return true;
}

//===========================================================================
//
// 
//
//===========================================================================

FVoxelTexture::FVoxelTexture(FVoxel *vox)
{
	SourceVox = vox;
	Width = 16;
	Height = 16;
	//bNoCompress = true;
}

//===========================================================================
//
// 
//
//===========================================================================

PalettedPixels FVoxelTexture::CreatePalettedPixels(int conversion, int frame)
{
	// GetPixels gets called when a translated palette is used so we still need to implement it here.
	PalettedPixels Pixels(256);
	uint8_t *pp = SourceVox->Palette.Data();

	if(pp != NULL)
	{
		for(int i=0;i<256;i++, pp+=3)
		{
			PalEntry pe;
			pe.r = (pp[0] << 2) | (pp[0] >> 4);
			pe.g = (pp[1] << 2) | (pp[1] >> 4);
			pe.b = (pp[2] << 2) | (pp[2] >> 4);
			// Alphatexture handling is just for completeness, but rather unlikely to be used ever.
			Pixels[i] = conversion == luminance ? pe.r : ColorMatcher.Pick(pe);

		}
	}
	else 
	{
		for(int i=0;i<256;i++, pp+=3)
		{
			Pixels[i] = (uint8_t)i;
		}
	}  
	ImageHelpers::FlipSquareBlock(Pixels.Data(), Width);
	return Pixels;
}

//===========================================================================
//
// FVoxelTexture::CopyPixels
//
// This creates a dummy 16x16 paletted bitmap and converts that using the
// voxel palette
//
//===========================================================================

int FVoxelTexture::CopyPixels(FBitmap *bmp, int conversion, int frame)
{
	PalEntry pe[256];
	uint8_t bitmap[256];
	uint8_t *pp = SourceVox->Palette.Data();

	if(pp != nullptr)
	{
		for(int i=0;i<256;i++, pp+=3)
		{
			bitmap[i] = (uint8_t)i;
			pe[i].r = (pp[0] << 2) | (pp[0] >> 4);
			pe[i].g = (pp[1] << 2) | (pp[1] >> 4);
			pe[i].b = (pp[2] << 2) | (pp[2] >> 4);
			pe[i].a = 255;
		}
	}
	else 
	{
		for(int i=0;i<256;i++, pp+=3)
		{
			bitmap[i] = (uint8_t)i;
			pe[i] = GPalette.BaseColors[i];
			pe[i].a = 255;
		}
	}    
	bmp->CopyPixelData(0, 0, bitmap, Width, Height, 1, 16, 0, pe);
	return 0;
}	

//===========================================================================
//
// 
//
//===========================================================================

FVoxelModel::FVoxelModel(FVoxel *voxel, bool owned)
{
	mVoxel = voxel;
	mOwningVoxel = owned;
	mPalette = TexMan.AddGameTexture(MakeGameTexture(new FImageTexture(new FVoxelTexture(voxel)), nullptr, ETextureType::Override));
}

//===========================================================================
//
// 
//
//===========================================================================

FVoxelModel::~FVoxelModel()
{
	if (mOwningVoxel) delete mVoxel;
}


//===========================================================================
//
// 
//
//===========================================================================

unsigned int FVoxelModel::AddVertex(FModelVertex &vert, FVoxelMap &check)
{
	unsigned int index = check[vert];
	if (index == 0xffffffff)
	{
		index = check[vert] =mVertices.Push(vert);
	}
	return index;
}

//===========================================================================
//
// 
//
//===========================================================================

void FVoxelModel::AddFace(int x1, int y1, int z1, int x2, int y2, int z2, int x3, int y3, int z3, int x4, int y4, int z4, uint8_t col, FVoxelMap &check)
{
	float PivotX = mVoxel->Mips[0].Pivot.X;
	float PivotY = mVoxel->Mips[0].Pivot.Y;
	float PivotZ = mVoxel->Mips[0].Pivot.Z;
	FModelVertex vert;
	unsigned int indx[4];

	vert.packedNormal = 0;	// currently this is not being used for voxels.
	vert.u = (((col & 15) + 0.5f) / 16.f);
	vert.v = (((col / 16) + 0.5f) / 16.f);

	vert.x =  x1 - PivotX;
	vert.z = -y1 + PivotY;
	vert.y = -z1 + PivotZ;
	indx[0] = AddVertex(vert, check);

	vert.x =  x2 - PivotX;
	vert.z = -y2 + PivotY;
	vert.y = -z2 + PivotZ;
	indx[1] = AddVertex(vert, check);

	vert.x =  x4 - PivotX;
	vert.z = -y4 + PivotY;
	vert.y = -z4 + PivotZ;
	indx[2] = AddVertex(vert, check);

	vert.x =  x3 - PivotX;
	vert.z = -y3 + PivotY;
	vert.y = -z3 + PivotZ;
	indx[3] = AddVertex(vert, check);


	mIndices.Push(indx[0]);
	mIndices.Push(indx[1]);
	mIndices.Push(indx[3]);
	mIndices.Push(indx[1]);
	mIndices.Push(indx[2]);
	mIndices.Push(indx[3]);
}

//===========================================================================
//
// 
//
//===========================================================================

void FVoxelModel::MakeSlabPolys(int x, int y, kvxslab_t *voxptr, FVoxelMap &check)
{
	const uint8_t *col = voxptr->col;
	int zleng = voxptr->zleng;
	int ztop = voxptr->ztop;
	int cull = voxptr->backfacecull;

	if (cull & 16)
	{
		AddFace(x, y, ztop, x+1, y, ztop, x, y+1, ztop, x+1, y+1, ztop, *col, check);
	}
	int z = ztop;
	while (z < ztop+zleng)
	{
		int c = 0;
		while (z+c < ztop+zleng && col[c] == col[0]) c++;

		if (cull & 1)
		{
			AddFace(x, y, z, x, y+1, z, x, y, z+c, x, y+1, z+c, *col, check);
		}
		if (cull & 2)
		{
			AddFace(x+1, y+1, z, x+1, y, z, x+1, y+1, z+c, x+1, y, z+c, *col, check);
		}
		if (cull & 4)
		{
			AddFace(x+1, y, z, x, y, z, x+1, y, z+c, x, y, z+c, *col, check);
		}
		if (cull & 8)
		{
			AddFace(x, y+1, z, x+1, y+1, z, x, y+1, z+c, x+1, y+1, z+c, *col, check);
		}	
		z+=c;
		col+=c;
	}
	if (cull & 32)
	{
		int zz = ztop+zleng-1;
		AddFace(x+1, y, zz+1, x, y, zz+1, x+1, y+1, zz+1, x, y+1, zz+1, voxptr->col[zleng-1], check);
	}
}

//===========================================================================
//
// 
//
//===========================================================================

//===========================================================================
//
// The voxel's faces, as few as can show it.
//
// MakeSlabPolys gives each voxel of a column's slab the side faces the slab's
// backface bits ask for -- the bits are the slab's, so every voxel of a long slab
// gets a face where one of them shows. High resolution voxels (Duke's voxel pack)
// came out at up to two million faces, and a path tracer rebuilds a moving mesh
// each frame: a single big voxel model on screen cost most of the frame.
//
// Here a face is only made where the voxel's neighbour is empty, and the faces in
// one plane are merged into rectangles of one colour: a tenth of the triangles or
// less, the same picture.
//
//===========================================================================

bool FVoxelModel::MakeMergedPolys(FVoxelMipLevel *mip, FVoxelMap &check)
{
	const int sx = mip->SizeX, sy = mip->SizeY, sz = mip->SizeZ;
	if (sx <= 0 || sy <= 0 || sz <= 0 || sx > 1024 || sy > 1024 || sz > 1024) return false;

	const uint8_t *slabs = mip->GetSlabData(false);
	auto forEachSlab = [&](auto &&fn)
	{
		for (int x = 0; x < sx; x++)
		{
			const uint8_t *slabxoffs = &slabs[mip->OffsetX[x]];
			const short *xyoffs = &mip->OffsetXY[x * (sy + 1)];
			for (int y = 0; y < sy; y++)
			{
				auto voxptr = (const kvxslab_t *)(slabxoffs + xyoffs[y]);
				auto voxend = (const kvxslab_t *)(slabxoffs + xyoffs[y + 1]);
				for (; voxptr < voxend; voxptr = (const kvxslab_t *)((const uint8_t *)voxptr + voxptr->zleng + 3))
				{
					fn(x, y, voxptr);
				}
			}
		}
	};

	// which cells hold a voxel (one bit each)
	std::vector<uint8_t> solid((size_t(sx) * sy * sz + 7) / 8, 0);
	auto cell = [&](int x, int y, int z) { return (size_t(x) * sy + y) * sz + z; };
	auto isSolid = [&](int x, int y, int z)
	{
		if (x < 0 || y < 0 || z < 0 || x >= sx || y >= sy || z >= sz) return false;
		const size_t i = cell(x, y, z);
		return ((solid[i >> 3] >> (i & 7)) & 1) != 0;
	};
	forEachSlab([&](int x, int y, const kvxslab_t *s)
	{
		for (int k = 0; k < s->zleng && s->ztop + k < sz; k++)
		{
			const size_t i = cell(x, y, s->ztop + k);
			solid[i >> 3] |= uint8_t(1 << (i & 7));
		}
	});

	// the faces that show, per direction: plane, row, column, colour
	// (directions as the backface bits: -x, +x, -y, +y, top (-z), bottom (+z))
	static const int dirs[6][3] = { { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 } };
	std::vector<uint64_t> faces[6];
	auto key = [](int plane, int v, int u, int col) { return (uint64_t(plane) << 48) | (uint64_t(v) << 32) | (uint64_t(u) << 16) | uint64_t(col); };
	forEachSlab([&](int x, int y, const kvxslab_t *s)
	{
		for (int k = 0; k < s->zleng && s->ztop + k < sz; k++)
		{
			const int z = s->ztop + k;
			const int col = s->col[k];
			for (int d = 0; d < 6; d++)
			{
				if (!(s->backfacecull & (1 << d))) continue;
				if (d == 4 && k != 0) continue;
				if (d == 5 && k != s->zleng - 1) continue;
				if (isSolid(x + dirs[d][0], y + dirs[d][1], z + dirs[d][2])) continue;
				switch (d)
				{
				case 0: case 1: faces[d].push_back(key(x, z, y, col)); break;
				case 2: case 3: faces[d].push_back(key(y, z, x, col)); break;
				default:        faces[d].push_back(key(z, y, x, col)); break;
				}
			}
		}
	});

	// greedy rectangles of one colour in each plane
	std::vector<uint16_t> mask(std::max({ size_t(sy) * sz, size_t(sx) * sz, size_t(sx) * sy }), 0);
	for (int d = 0; d < 6; d++)
	{
		auto &list = faces[d];
		std::sort(list.begin(), list.end());
		const int W = d < 2 ? sy : sx;
		const int H = d < 4 ? sz : sy;
		size_t first = 0;
		while (first < list.size())
		{
			const int plane = int(list[first] >> 48);
			size_t last = first;
			while (last < list.size() && int(list[last] >> 48) == plane)
			{
				const int v = int((list[last] >> 32) & 0xffff), u = int((list[last] >> 16) & 0xffff);
				mask[size_t(v) * W + u] = uint16_t((list[last] & 0xffff) + 1);
				last++;
			}
			for (size_t f = first; f < last; f++)
			{
				const int v0 = int((list[f] >> 32) & 0xffff), u0 = int((list[f] >> 16) & 0xffff);
				const uint16_t c = mask[size_t(v0) * W + u0];
				if (c == 0) continue;
				int w = 1, h = 1;
				while (u0 + w < W && mask[size_t(v0) * W + u0 + w] == c) w++;
				for (; v0 + h < H; h++)
				{
					const size_t row = size_t(v0 + h) * W;
					int i = 0;
					while (i < w && mask[row + u0 + i] == c) i++;
					if (i < w) break;
				}
				for (int j = 0; j < h; j++)
				{
					for (int i = 0; i < w; i++) mask[size_t(v0 + j) * W + u0 + i] = 0;
				}

				const int u1 = u0 + w, v1 = v0 + h;
				const uint8_t col = uint8_t(c - 1);
				switch (d)
				{
				case 0: AddFace(plane, u0, v0, plane, u1, v0, plane, u0, v1, plane, u1, v1, col, check); break;
				case 1: AddFace(plane + 1, u1, v0, plane + 1, u0, v0, plane + 1, u1, v1, plane + 1, u0, v1, col, check); break;
				case 2: AddFace(u1, plane, v0, u0, plane, v0, u1, plane, v1, u0, plane, v1, col, check); break;
				case 3: AddFace(u0, plane + 1, v0, u1, plane + 1, v0, u0, plane + 1, v1, u1, plane + 1, v1, col, check); break;
				case 4: AddFace(u0, v0, plane, u1, v0, plane, u0, v1, plane, u1, v1, plane, col, check); break;
				default: AddFace(u1, v0, plane + 1, u0, v0, plane + 1, u1, v1, plane + 1, u0, v1, plane + 1, col, check); break;
				}
			}
			first = last;
		}
	}
	return true;
}

void FVoxelModel::Initialize()
{
	FVoxelMap check;
	FVoxelMipLevel *mip = &mVoxel->Mips[0];
	if (r_voxel_merge && MakeMergedPolys(mip, check))
	{
		return;
	}
	for (int x = 0; x < mip->SizeX; x++)
	{
		uint8_t *slabxoffs = &mip->GetSlabData(false)[mip->OffsetX[x]];
		short *xyoffs = &mip->OffsetXY[x * (mip->SizeY + 1)];
		for (int y = 0; y < mip->SizeY; y++)
		{
			kvxslab_t *voxptr = (kvxslab_t *)(slabxoffs + xyoffs[y]);
			kvxslab_t *voxend = (kvxslab_t *)(slabxoffs + xyoffs[y+1]);
			for (; voxptr < voxend; voxptr = (kvxslab_t *)((uint8_t *)voxptr + voxptr->zleng + 3))
			{
				MakeSlabPolys(x, y, voxptr, check);
			}
		}
	}
}

//===========================================================================
//
// 
//
//===========================================================================

void FVoxelModel::BuildVertexBuffer(FModelRenderer *renderer)
{
	if (!GetVertexBuffer(renderer->GetType()))
	{
		Initialize();

		auto vbuf = renderer->CreateVertexBuffer(true, true);
		SetVertexBuffer(renderer->GetType(), vbuf);

		FModelVertex *vertptr = vbuf->LockVertexBuffer(mVertices.Size());
		unsigned int *indxptr = vbuf->LockIndexBuffer(mIndices.Size());

		memcpy(vertptr, &mVertices[0], sizeof(FModelVertex)* mVertices.Size());
		memcpy(indxptr, &mIndices[0], sizeof(unsigned int)* mIndices.Size());

		vbuf->UnlockVertexBuffer();
		vbuf->UnlockIndexBuffer();
		mNumIndices = mIndices.Size();

		// delete our temporary buffers
		mVertices.Clear();
		mIndices.Clear();
		mVertices.ShrinkToFit();
		mIndices.ShrinkToFit();
	}
}


//===========================================================================
//
// for skin precaching
//
//===========================================================================

void FVoxelModel::AddSkins(uint8_t *hitlist, const FTextureID*)
{
	hitlist[mPalette.GetIndex()] |= FTextureManager::HIT_Flat;
}

//===========================================================================
//
// 
//
//===========================================================================

bool FVoxelModel::Load(const char * fn, int lumpnum, const char * buffer, int length)
{
	return false;	// not needed
}

//===========================================================================
//
// Voxels don't have frames so always return 0
//
//===========================================================================

int FVoxelModel::FindFrame(const char* name, bool nodefault)
{
	return nodefault ? FErr_Voxel : 0; // -2, not -1 because voxels are special.
}

//===========================================================================
//
// Voxels need aspect ratio correction according to the current map's setting
//
//===========================================================================

float FVoxelModel::getAspectFactor(float stretch)
{
	return stretch;
}

//===========================================================================
//
// Voxels never interpolate between frames, they only have one.
//
//===========================================================================

void FVoxelModel::RenderFrame(FModelRenderer *renderer, FGameTexture * skin, int frame, int frame2, double inter, FTranslationID translation, const FTextureID*, const TArray<VSMatrix>& boneData, int boneStartPosition)
{
	renderer->SetMaterial(skin, true, translation);
	renderer->SetupFrame(this, 0, 0, 0, {}, -1);
	renderer->DrawElements(mNumIndices, 0);
}
