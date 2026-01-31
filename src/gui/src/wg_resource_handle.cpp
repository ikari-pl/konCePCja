// wg_resource_handle.cpp
//
// Resource Handle implementation
//
//
// Copyright (c) 2002-2004 Rob Wiskow
// rob-dev@boxedchaos.com
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
//

#include "wg_resource_handle.h"
#include "wg_error.h"
#include "wg_application.h"
#include <map>
#include <string>
#include "log.h"
#include <png.h>

namespace wGui
{

static SDL_Surface* LoadPngSurface(const std::string& filename)
{
	FILE* fp = fopen(filename.c_str(), "rb");
	if (!fp) return nullptr;

	png_byte header[8];
	if (fread(header, 1, 8, fp) != 8 || png_sig_cmp(header, 0, 8)) {
		fclose(fp);
		return nullptr;
	}

	png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (!png_ptr) { fclose(fp); return nullptr; }
	png_infop info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr) { png_destroy_read_struct(&png_ptr, nullptr, nullptr); fclose(fp); return nullptr; }
	if (setjmp(png_jmpbuf(png_ptr))) {
		png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
		fclose(fp);
		return nullptr;
	}

	png_init_io(png_ptr, fp);
	png_set_sig_bytes(png_ptr, 8);
	png_read_info(png_ptr, info_ptr);

	png_uint_32 width = 0, height = 0;
	int bit_depth = 0, color_type = 0;
	png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type, nullptr, nullptr, nullptr);

	if (bit_depth == 16) png_set_strip_16(png_ptr);
	if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png_ptr);
	if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png_ptr);
	if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png_ptr);
	if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png_ptr);
	if (!(color_type & PNG_COLOR_MASK_ALPHA)) png_set_add_alpha(png_ptr, 0xFF, PNG_FILLER_AFTER);

	png_read_update_info(png_ptr, info_ptr);

	SDL_Surface* surface = SDL_CreateSurface((int)width, (int)height, SDL_PIXELFORMAT_RGBA32);
	if (!surface) {
		png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
		fclose(fp);
		return nullptr;
	}

	png_bytep* rows = (png_bytep*)malloc(sizeof(png_bytep) * height);
	if (!rows) {
		SDL_DestroySurface(surface);
		png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
		fclose(fp);
		return nullptr;
	}

	for (png_uint_32 y = 0; y < height; ++y) {
		rows[y] = (png_bytep)surface->pixels + y * surface->pitch;
	}

	png_read_image(png_ptr, rows);
	png_read_end(png_ptr, nullptr);

	free(rows);
	png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
	fclose(fp);
	return surface;
}

std::map<TResourceId, unsigned int> CResourceHandle::m_RefCountMap;
std::map<TResourceId, SDL_Surface*> CBitmapResourceHandle::m_BitmapMap;
std::map<TResourceId, std::string> CStringResourceHandle::m_StringMap;
std::map<TResourceId, SDL_Cursor*> CCursorResourceHandle::m_SDLCursorMap;
TResourceId CResourceHandle::m_NextUnusedResourceId = 10000;
static bool g_resources_shutting_down = false;

void CResourceHandle::BeginShutdown()
{
	g_resources_shutting_down = true;
}


CResourceHandle::CResourceHandle(TResourceId resId) :
	m_ResourceId(resId)
{
	if (m_ResourceId == AUTO_CREATE_RESOURCE_ID)
	{
		while (m_RefCountMap.find(m_NextUnusedResourceId) != m_RefCountMap.end())
		{
			++m_NextUnusedResourceId;
		}
		m_ResourceId = m_NextUnusedResourceId;
		++m_NextUnusedResourceId;
	}
	if (m_RefCountMap.find(m_ResourceId) == m_RefCountMap.end() || m_RefCountMap[m_ResourceId] == 0)
	{
		m_RefCountMap[m_ResourceId] = 0;
	}
	++m_RefCountMap[m_ResourceId];
}


CResourceHandle::CResourceHandle(const CResourceHandle& resHandle)
{
	m_ResourceId = resHandle.m_ResourceId;
	++m_RefCountMap[m_ResourceId];
}


CResourceHandle::~CResourceHandle()
{
	if (g_resources_shutting_down) return;
	if (GetRefCount() > 0)
	{
		--m_RefCountMap[m_ResourceId];
	}
	else
	{
    LOG_ERROR("CResourceHandle::~CResourceHandle : Trying to decrement refcount of zero!");
	}
}


CBitmapResourceHandle::~CBitmapResourceHandle()
{
	if (g_resources_shutting_down) return;
	if (SDL_WasInit(SDL_INIT_VIDEO) == 0) return;
	if (GetRefCount() == 1 && m_BitmapMap.find(m_ResourceId) != m_BitmapMap.end())
	{
		SDL_DestroySurface(m_BitmapMap[m_ResourceId]);
		m_BitmapMap.erase(m_ResourceId);
	}
}


SDL_Surface* CBitmapResourceHandle::Bitmap() const
{
	return (m_BitmapMap.find(m_ResourceId) != m_BitmapMap.end()) ? m_BitmapMap[m_ResourceId] : nullptr;
}

void CBitmapResourceHandle::CleanupAll()
{
	for (auto& it : m_BitmapMap) {
		if (it.second) SDL_DestroySurface(it.second);
	}
	m_BitmapMap.clear();
}


CBitmapFileResourceHandle::CBitmapFileResourceHandle(std::string sFilename) :
	CBitmapResourceHandle(AUTO_CREATE_RESOURCE_ID),
	m_sFilename(std::move(sFilename))
{
	if (m_BitmapMap.find(m_ResourceId) == m_BitmapMap.end())
	{
		SDL_Surface* pSurface = nullptr;
		// Load PNG with libpng (for alpha), otherwise fallback to BMP
		if (m_sFilename.size() >= 4 && m_sFilename.substr(m_sFilename.size() - 4) == ".png") {
			pSurface = LoadPngSurface(m_sFilename);
		}
		if (!pSurface)
		{
			pSurface = SDL_LoadBMP(m_sFilename.c_str());
		}
		if (!pSurface)
		{
			throw(Wg_Ex_App("Unable to load bitmap: " + m_sFilename, "CBitmapFileResourceHandle::CBitmapFileResourceHandle"));
		}
		m_BitmapMap[m_ResourceId] = pSurface;
	}
}


CStringResourceHandle::~CStringResourceHandle()
{
	if (g_resources_shutting_down) return;
	if (GetRefCount() == 1 && m_StringMap.find(m_ResourceId) != m_StringMap.end())
	{
		m_StringMap.erase(m_ResourceId);
	}
}


std::string CStringResourceHandle::String() const
{
	return (m_StringMap.find(m_ResourceId) != m_StringMap.end()) ? m_StringMap[m_ResourceId] : "";
}

void CStringResourceHandle::CleanupAll()
{
	m_StringMap.clear();
}


CCursorResourceHandle::~CCursorResourceHandle()
{
	if (g_resources_shutting_down) return;
	if (SDL_WasInit(SDL_INIT_VIDEO) == 0) return;
	if (GetRefCount() == 1 && m_SDLCursorMap.find(m_ResourceId) != m_SDLCursorMap.end())
	{
		SDL_DestroyCursor(m_SDLCursorMap[m_ResourceId]);
		m_SDLCursorMap.erase(m_ResourceId);
	}
}


SDL_Cursor* CCursorResourceHandle::Cursor() const
{
	return (m_SDLCursorMap.find(m_ResourceId) != m_SDLCursorMap.end()) ? m_SDLCursorMap[m_ResourceId] : nullptr;
}

void CCursorResourceHandle::CleanupAll()
{
	for (auto& it : m_SDLCursorMap) {
		if (it.second) SDL_DestroyCursor(it.second);
	}
	m_SDLCursorMap.clear();
}

}

