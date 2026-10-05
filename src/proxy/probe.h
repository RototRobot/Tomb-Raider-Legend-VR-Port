#pragma once
// Finding the view-projection matrix among the vertex shader constants.
//
// Stereo means rendering the world twice with a different projection each
// time, so the first thing the mod has to know is which constant register the
// engine puts the view-projection matrix in. The engine reaches D3D9 through
// cdc::PCStateManager::SetVertexConstantMatrix4x4(reg, Matrix), which lands
// here as SetVertexShaderConstantF(reg, data, 4).
//
// Two properties separate it from everything else uploaded:
//
//   * A world or view matrix is affine -- its last column is (0,0,0,1). A
//     matrix carrying a projection is not. Engines upload either the matrix or
//     its transpose, so both layouts are tested and the log says which it is.
//
//   * A world-view-projection is uploaded once per object, hundreds of times a
//     frame. A view-projection is uploaded once per view -- a handful of times
//     a frame. Counting writes per frame separates them.

#include <windows.h>

struct _D3DMATRIX;

namespace trlvr
{
    void probe_constants(UINT start_register, const float* data, UINT vector4_count);

    // Every (register, count) pair the engine actually uses. Guessing which
    // shapes to look for was wrong twice; this just records what happens.
    void probe_call(UINT start_register, UINT vector4_count);

    // The fixed-function path. If the engine sets a view or projection here
    // rather than through constants, this is where it shows up.
    void probe_transform(unsigned state, const _D3DMATRIX* matrix);

    // Where the scene is actually being drawn. The projection implies an 8/7
    // aspect while the back buffer is 16:9, so the surface being projected
    // onto is not the one we assumed.
    void probe_surface(const char* what, unsigned width, unsigned height);

    void probe_end_frame();
    void probe_report(const char* reason);
}
