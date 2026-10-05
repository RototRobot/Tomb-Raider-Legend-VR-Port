// How many game units are there in a metre?
//
// Everything that converts between the player's body and the game world goes
// through `world_scale`: how far leaning moves you, and how far apart the eyes
// are. It has been an unverified 100 since the first build. Rotation does not
// depend on it, which is why turning your head has always felt right while
// leaning has not -- and why it survived this long without being caught.
//
// It is not a matter of taste. The game has a unit system and the answer is a
// property of it, so it can be measured instead of judged. The trick is finding
// something inside the game whose real size is known, and a human body is the
// obvious candidate: character models are built to human scale, and "life-size"
// in a headset means exactly "Lara is as tall as a person".
//
// So: find Lara's model, measure it in game units, and divide.
//
// The measurement is of the mesh the game actually draws, not of a tuning
// constant that might mean something else. Model vertices are stored per-bone,
// each in its own segment's local space, so the model's extent is the union of
// every segment's box placed at its pivot -- which is the rest pose, and is
// what the bounding data describes.
//
// One number, logged once, and then this file has done its job.
#include "measure.h"

#include "../common/log.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace trlvr
{
    namespace
    {
        bool g_done = false;

        // BaseInstance / Instance, from the PDB's type records.
        const unsigned kInstNext     = 0x08;
        const unsigned kInstPosition = 0x10;   // cdc::Vector3, 16 bytes
        const unsigned kInstScale    = 0x50;
        const unsigned kInstObject   = 0x94;   // Object*
        const unsigned kInstData     = 0x1C0;  // void*, the object's own state

        // PlayerData: Player work is at +0x70, and inside it collideRadius is
        // at +0x364 and dropOffHeight at +0x284.
        const unsigned kPlayerCollideRadius = 0x70 + 0x364;
        const unsigned kPlayerDropOffHeight = 0x70 + 0x284;

        // Object.
        const unsigned kObjNumModels = 0x18;   // short
        const unsigned kObjModelList = 0x20;   // Model**
        const unsigned kObjName      = 0x48;   // char*

        // Model.
        const unsigned kModNumSegments = 0x04;
        const unsigned kModSegmentList = 0x0C;
        const unsigned kModScale       = 0x10;   // cdc::Vector3
        const unsigned kModMaxRad      = 0x3C;

        // Segment -- 64 bytes: min, max, pivot as 16-byte vectors, then flags.
        const unsigned kSegSize   = 0x40;
        const unsigned kSegMin    = 0x00;
        const unsigned kSegMax    = 0x10;
        const unsigned kSegPivot  = 0x20;
        const unsigned kSegParent = 0x38;   // int, negative at the root

        // Nothing here is allowed to crash the game. These are pointers read
        // out of engine structures at a moment nobody chose, and a probe that
        // takes the process down with it is worse than no measurement.
        bool readable(const void* p, size_t n)
        {
            if (!p || (uintptr_t)p < 0x10000)
                return false;
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(p, &mbi, sizeof(mbi)))
                return false;
            if (mbi.State != MEM_COMMIT)
                return false;
            const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
                           | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                           | PAGE_EXECUTE_WRITECOPY;
            if (!(mbi.Protect & ok) || (mbi.Protect & PAGE_GUARD))
                return false;
            const uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            return (uintptr_t)p + n <= end;
        }

        template <typename T>
        T at(const void* base, unsigned off)
        {
            T v{};
            memcpy(&v, (const unsigned char*)base + off, sizeof(T));
            return v;
        }

        // Camera::focusInstance. CameraCore is Camera's first member, so this
        // offset is into the outer struct -- valid because CalcAutoCenterData
        // is typed as taking a Camera*, unlike CalculateWCTransform.
        const unsigned kCamFocusInstance = 0x488;

        // The model's extent in game units, from the rest pose.
        //
        // Each segment's box is in its own space, so a segment has to be placed
        // by walking its pivots up to the root before its box means anything.
        // Segments are stored parents-first, so one forward pass does it.
        bool model_extent(const void* model, float* size, float* bone_size,
                          float* maxrad, int* segments)
        {
            const int nseg = at<int>(model, kModNumSegments);
            const void* segs = at<void*>(model, kModSegmentList);
            if (nseg < 1 || nseg > 512 || !readable(segs, (size_t)nseg * kSegSize))
                return false;

            float lo[3] = { 1e30f, 1e30f, 1e30f };
            float hi[3] = { -1e30f, -1e30f, -1e30f };

            // The skeleton's own extent, separately. The mesh box includes hair,
            // boots and whatever else hangs off her; the bones do not. Reporting
            // both says how much of the height is body and how much is trimming,
            // which is the difference between measuring a person and measuring a
            // person's ponytail.
            float blo[3] = { 1e30f, 1e30f, 1e30f };
            float bhi[3] = { -1e30f, -1e30f, -1e30f };

            // Accumulated pivots, one per segment.
            static float world[512][3];

            for (int i = 0; i < nseg; ++i)
            {
                const unsigned char* s = (const unsigned char*)segs + (size_t)i * kSegSize;
                const int parent = at<int>(s, kSegParent);

                for (int a = 0; a < 3; ++a)
                {
                    const float pivot = at<float>(s, kSegPivot + 4u * a);
                    world[i][a] = (parent >= 0 && parent < i)
                                ? world[parent][a] + pivot
                                : pivot;

                    if (world[i][a] < blo[a]) blo[a] = world[i][a];
                    if (world[i][a] > bhi[a]) bhi[a] = world[i][a];

                    const float mn = world[i][a] + at<float>(s, kSegMin + 4u * a);
                    const float mx = world[i][a] + at<float>(s, kSegMax + 4u * a);
                    if (mn < lo[a]) lo[a] = mn;
                    if (mx > hi[a]) hi[a] = mx;
                }
            }

            for (int a = 0; a < 3; ++a)
            {
                if (hi[a] < lo[a])
                    return false;
                size[a] = hi[a] - lo[a];
                bone_size[a] = bhi[a] - blo[a];
            }
            *maxrad = at<float>(model, kModMaxRad);
            *segments = nseg;
            return true;
        }

        void measure(const void* inst, const char* name, const void* object)
        {
            const short nmodels = at<short>(object, kObjNumModels);
            const void* list = at<void*>(object, kObjModelList);
            if (nmodels < 1 || !readable(list, sizeof(void*)))
                return;

            const void* model = at<void*>(list, 0);
            if (!readable(model, 0x94))
                return;

            float size[3]{}, bone[3]{}, maxrad = 0.0f;
            int segments = 0;
            if (!model_extent(model, size, bone, &maxrad, &segments))
                return;

            // Instance and model scale both multiply the mesh. They are almost
            // always 1, but "almost always" is not a thing to assume when the
            // whole point is a number nobody has checked.
            float iscale[3], mscale[3];
            for (int a = 0; a < 3; ++a)
            {
                iscale[a] = at<float>(inst, kInstScale + 4u * a);
                mscale[a] = at<float>(model, kModScale + 4u * a);
            }

            log("measure: found \"%s\"  (%d models, %d bones)",
                name, (int)nmodels, segments);
            log("  mesh extent    %.2f x %.2f x %.2f game units", size[0], size[1], size[2]);
            log("  bone extent    %.2f x %.2f x %.2f", bone[0], bone[1], bone[2]);
            log("  bounding rad   %.2f", maxrad);
            log("  instance scale %.3f %.3f %.3f   model scale %.3f %.3f %.3f",
                iscale[0], iscale[1], iscale[2],
                mscale[0], mscale[1], mscale[2]);

            // The tallest axis is the one to divide, and saying which it was
            // keeps the answer honest -- a model lying down its Y axis would
            // otherwise be measured across the shoulders and never noticed.
            int tall = 0;
            for (int a = 1; a < 3; ++a)
                if (size[a] > size[tall])
                    tall = a;

            const char* axis = (tall == 0) ? "X" : (tall == 1) ? "Y" : "Z";
            log("  tallest axis   %s at %.2f units", axis, size[tall]);
            log("  ---> world_scale = %.1f  if Lara is 1.75 m", size[tall] / 1.75f);
            log("       world_scale = %.1f  if Lara is 1.70 m", size[tall] / 1.70f);
            log("       world_scale = %.1f  if Lara is 1.80 m", size[tall] / 1.80f);

            // An independent check, from gameplay rather than from art.
            //
            // Instance::data is the object's own state; for the player that is
            // a PlayerData, whose Player::collideRadius is the radius the game
            // pushes her around the world with. A person is about 0.3 m across,
            // so that number alone separates a world of 100 units per metre
            // from one of 300 -- and it comes from a different department than
            // the mesh, which is what makes it worth having.
            const void* data = at<void*>(inst, kInstData);
            if (readable(data, kPlayerCollideRadius + 4))
            {
                const int radius = at<int>(data, kPlayerCollideRadius);
                const short dropoff = at<short>(data, kPlayerDropOffHeight);
                log("  collide radius %d units, drop-off height %d "
                    "(if Instance::data is a PlayerData)", radius, dropoff);
                if (radius > 0)
                    log("       a 0.30 m radius would put world_scale at %.1f",
                        radius / 0.30f);
            }
        }

        // Every object in the level, measured.
        //
        // Lara alone is one measurement, and one measurement of a thing whose
        // real size is assumed rather than known is not a calibration. A door,
        // a crate, a vehicle -- anything here with an obvious real-world size
        // checks her, from art made by different people for a different purpose.
        //
        // It began as a name dump and earned its place immediately: the first
        // run matched "lara" against "co_lara_pda", her PDA, and measured that.
        void dump_names(void* inst)
        {
            char seen[96][32];
            int nseen = 0;

            void* node = inst;
            for (int i = 0; i < 8192 && readable(node, 0x98); ++i)
            {
                const void* object = at<void*>(node, kInstObject);
                if (readable(object, 0x4C))
                {
                    const char* name = at<char*>(object, kObjName);
                    if (readable(name, 32))
                    {
                        char safe[32]{};
                        memcpy(safe, name, 31);

                        bool dup = false;
                        for (int j = 0; j < nseen; ++j)
                            if (strcmp(seen[j], safe) == 0) { dup = true; break; }

                        if (!dup && nseen < 96)
                        {
                            memcpy(seen[nseen++], safe, 32);

                            // The same measurement Lara got, on everything.
                            const short nm = at<short>(object, kObjNumModels);
                            const void* list = at<void*>(object, kObjModelList);
                            if (nm >= 1 && readable(list, sizeof(void*)))
                            {
                                const void* model = at<void*>(list, 0);
                                float sz[3]{}, bn[3]{}, mr = 0.0f;
                                int nb = 0;
                                if (readable(model, 0x94) &&
                                    model_extent(model, sz, bn, &mr, &nb))
                                {
                                    log("    %-28s %8.1f x %8.1f x %8.1f  "
                                        "(%.2f m tall at 291/m)",
                                        safe, sz[0], sz[1], sz[2], sz[2] / 291.0f);
                                }
                            }
                        }
                    }
                }
                node = at<void*>(node, kInstNext);
            }

            log("measure: %d distinct objects measured above.", nseen);
        }

    }

    void measure_note_camera(void* camera)
    {
        if (g_done || !camera)
            return;

        void* inst = at<void*>(camera, kCamFocusInstance);
        if (!readable(inst, 0x98))
            return;

        const void* object = at<void*>(inst, kInstObject);
        if (!readable(object, 0x4C))
            return;

        const char* name = at<char*>(object, kObjName);
        char safe[32]{};
        if (readable(name, 32))
            memcpy(safe, name, 31);
        else
            memcpy(safe, "<unnamed>", 10);

        // Whatever the camera is following is what gets measured, and the name
        // goes in the log so the answer can be checked rather than trusted.
        // If it says anything but Lara, the number below is not hers.
        g_done = true;
        log("measure: the camera is following \"%s\"", safe);
        measure(inst, safe, object);

        // And everything else, walked from her. Finding Lara is itself the
        // proof that a level is up, which is a better trigger than counting
        // frames and hoping -- the first attempt waited on 20,000 calls to a
        // per-object process function and never fired in the tutorial, which
        // does not have enough objects to reach it.
        dump_names(inst);
    }

    void measure_init()
    {
        // Nothing to install. This used to hook GenericProcess to get hold of
        // any instance at all; the camera's focusInstance is both a better
        // starting point and one hook fewer in a per-object path.
    }
}
