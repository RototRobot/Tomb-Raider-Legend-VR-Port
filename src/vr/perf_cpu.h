#pragma once
// CPU time per frame by part (2026-10-05). The F3 GPU profile showed the
// game's drawing at about 5 ms of GPU time while the GPU waited up to
// 21 ms per frame for the first draw: the frame is CPU-bound. These timers
// split the CPU frame: the game's update before its first draw, the
// rendering after it, the draw calls themselves, and the mod's main hooks
// (inclusive of the retail function each one wraps). Reported with the
// 5 s "perf:" line as "perf cpu: ...".

namespace trlvr
{
    enum PerfBucket
    {
        PerfDrawCalls,      // proxy dispatch_draw (mod + DXVK, both eyes)
        PerfDrawMath,       // the mod's per-eye matrix work inside it
        PerfSubmit,         // vr_submit_frame
        PerfInput,          // tune/pose/controller update at Present
        PerfCamera,         // CAMERA_CalculateWCTransform hook
        PerfDrawInstance,   // DRAW_DrawInstance hook, other instances
        PerfDrawLara,       // DRAW_DrawInstance hook, Lara (hands/body)
        PerfVisibility,     // PIPE3D_CheckInstanceVisibility hook
        PerfTerrain,        // DRAW_DrawTerrainGroup hook
        PerfLineProbe,      // MULTIBODY_LegacyLineProbe hook
        PerfAimTarget,      // playerUpdateTargetPos hook
        PerfMovement,       // FilteredInput::Update + ProcessMovement hooks
        PerfUiDraw,         // BasicDrawable::Draw hook (HUD classification)
        PerfBucketCount
    };

    void perf_cpu_begin(PerfBucket bucket);
    void perf_cpu_end(PerfBucket bucket);

    struct PerfCpuScope
    {
        PerfBucket bucket;
        explicit PerfCpuScope(PerfBucket b) : bucket(b) { perf_cpu_begin(b); }
        ~PerfCpuScope() { perf_cpu_end(bucket); }
        PerfCpuScope(const PerfCpuScope&) = delete;
        PerfCpuScope& operator=(const PerfCpuScope&) = delete;
    };

    // Every proxy draw (the first one of a frame ends the update phase).
    void perf_cpu_draw();
    // At the swap chain's Present: start and end of the mod's frame work.
    void perf_cpu_present_begin();
    void perf_cpu_present_end();
    // Logs the averages over the frames since the last report and resets.
    void perf_cpu_report(unsigned frames);
}
