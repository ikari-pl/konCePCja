/* Classic 2x pixel-scaling kernels — clean-room first-party code (see the .cpp
 * for the algorithms and provenance). 16bpp RGB565 in, 2x-scaled 16bpp out. */

#pragma once

#include <SDL3/SDL.h>

void filter_supereagle(Uint8* srcPtr, Uint32 srcPitch, Uint8* dstPtr,
                       Uint32 dstPitch, int width, int height);
void filter_scale2x(const Uint8* srcPtr, Uint32 srcPitch, Uint8* dstPtr,
                    Uint32 dstPitch, int width, int height);
void filter_ascale2x(Uint8* srcPtr, Uint32 srcPitch, Uint8* dstPtr,
                     Uint32 dstPitch, int width, int height);
void filter_tv2x(const Uint8* srcPtr, Uint32 srcPitch, Uint8* dstPtr,
                 Uint32 dstPitch, int width, int height);
void filter_bilinear(const Uint8* srcPtr, Uint32 srcPitch, Uint8* dstPtr,
                     Uint32 dstPitch, int width, int height);
void filter_bicubic(Uint8* srcPtr, Uint32 srcPitch, Uint8* dstPtr,
                    Uint32 dstPitch, int width, int height);
void filter_dotmatrix(const Uint8* srcPtr, Uint32 srcPitch, Uint8* dstPtr,
                      Uint32 dstPitch, int width, int height);
