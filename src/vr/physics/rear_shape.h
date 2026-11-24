#pragma once
// The buttocks' shape for the rear soft body: which of Lara's source
// vertices move, by how much, and where a fingertip dents them. The
// falloff is the Underworld mod's shader maths (skin_deform: a smoothstep
// ellipsoid with a full-weight core, a seam ramp across her midline, a
// cleft band, fingertip dent spheres), run here on the CPU over the mesh's
// int16 source vertices, which Legend skins on every draw. Also the fit of
// the cheeks to her measured mesh, and the touch zones god mode tests.
//
// Space: "pelvis-local bind" -- model units (about 291 per metre), axes as
// the model's (z up, +x her left, +y back, -y front), origin at the pelvis
// bone's bind joint. Legend's bind frames are not rotated, so an offset in
// this space added to a vertex's local shorts is an offset before skinning.
//
// Pure maths -- no config(), no game memory, no Windows calls -- so
// mathtest links it.

namespace trlvr
{
namespace rear
{
    // One region: an ellipsoid whose weight is 1 inside `core` (a fraction
    // of the radius) and eases to 0 at its surface with a smoothstep.
    // Teardrop options (the cheeks): above the centre the height radius is
    // `up` (0 = radius[2]) and the axis bends toward her front by `lean`
    // at the top (lean x (dz / up)^2, following her lower back); with
    // `sharp_bottom` the fall below the centre is quartic, so the weight
    // holds down to near the bottom and then drops fast.
    struct RearRegion
    {
        float centre[3] = {};
        float radius[3] = { 1.0f, 1.0f, 1.0f };   // [2]: below the centre
        float core = 0.0f;              // 0 .. 0.95
        float up = 0.0f;
        float lean = 0.0f;
        bool sharp_bottom = false;
    };

    // The point's place in the region's own units: t = (p - axis) / r with
    // the teardrop's radii and bent axis (|t| = 1 on the surface of a
    // plain ellipsoid).
    void region_local(const RearRegion& r, const float p[3], float t[3]);

    // u = saturate((1 - d^2) / (1 - core^2)), d^2 = tx^2 + ty^2 + tz^2
    // (tz^4 below the centre with sharp_bottom); returns u^2 (3 - 2u). 1
    // at the centre and inside the core, 0 at and beyond the surface, no
    // crease at either edge.
    float region_weight(const RearRegion& r, const float p[3]);

    // A point on the region's zero-weight surface scaled by `scale` (1 =
    // the surface, core = the core), in the plane of axes u, v (0..2) at
    // angle a: for the debug lines.
    void region_outline(const RearRegion& r, int u, int v, float a, float scale, float out[3]);

    // The seam cut: side 0 (her -x cheek) keeps x < seam_x, side 1 keeps
    // x > seam_x; 0.5 on the seam, 0 / 1 at seam_soft beyond it (a
    // smoothstep in between). seam_soft <= 0: no cut (1 everywhere).
    // inset >= 0 moves the ramp that far out from the seam on each side.
    float seam_weight(float x, float seam_x, float seam_soft, int side, float inset = 0.0f);

    // A fingertip dent: a point inside the sphere is pushed out along the
    // line from its centre to its surface (P += normalize(P - c) x
    // max(r - |P - c|, 0)). Returns whether it moved.
    bool dent_push(float p[3], const float centre[3], float radius);

    // Both cheeks (0: -x, 1: +x), the cleft between them and the seam.
    struct RearShape
    {
        RearRegion cheek[2];
        RearRegion cleft;
        float seam_x = 0.0f;
        float seam_soft = 0.0f;         // 0 = no cut
        float seam_inset = 0.0f;        // the ramp moved out from the seam
        // The rest-shape fields (rear_flare, rear_cleft_close), placed from
        // the fit's landmarks. Flare: across her body it rises from
        // flare_x0 to flare_x1 (distances out from the seam, per side) and
        // stays full beyond (over the hip side); it is full from flare_zlo
        // to flare_zhi and fades over flare_fade above and below; it fades
        // toward her front from flare_y1 (full) to flare_y0 (none).
        float flare_x0[2] = { 0.0f, 0.0f };
        float flare_x1[2] = { 1.0f, 1.0f };
        float flare_zlo = 0.0f, flare_zhi = 0.0f, flare_fade = 1.0f;
        float flare_y0 = 0.0f, flare_y1 = 1.0f;
        // Cleft close: full from close_x0 to close_x1 out from the seam,
        // gone by close_x2; only on her back (from close_y0 to close_y1).
        float close_x1 = 1.0f, close_x2 = 2.0f;
        float close_y0 = 0.0f, close_y1 = 1.0f;
    };

    struct RearWeights
    {
        float cheek[2] = { 0.0f, 0.0f };
        float cleft = 0.0f;
    };

    // Per vertex (bind position): each cheek's region weight times its
    // seam ramp; the cleft is never cut.
    RearWeights rear_weights(const RearShape& s, const float p[3]);

    struct RearDentSphere
    {
        bool on = false;
        float centre[3] = {};
        float radius = 0.0f;
    };

    // The moved bind position of one vertex: bind + w0 x offset0 + w1 x
    // offset1 + wc x cleft_offset, then each dent that is on.
    void rear_deform(const float bind[3], const RearWeights& w, const float offset[2][3],
                     const float cleft_offset[3], const RearDentSphere* dents, int dent_count,
                     float out[3]);

    // The baseline fullness: how far a vertex goes out (amount, model units;
    // negative = in) from the cheeks' shape alone, at rest. Each cheek's
    // region weight (seam ramp included, so it fades at her midline; the
    // cleft is left alone) times the amount, along the direction from the
    // region's centre through the vertex in the region's own (radius-
    // normalised) space, biased toward her back (+y) so a vertex at the
    // centre goes straight back. Adds to `delta`.
    void rear_inflate(const RearShape& s, const RearWeights& w, const float p[3], float amount,
                      float delta[3]);

    // The hip flare: a vertex goes out from her midline (x) by amount x
    // the flare field (RearShape) -- nothing inside flare_x0 (the cleft
    // keeps its width), rising across the glute to full at its outer edge
    // and staying full over the hip side, fading up to the waist, down the
    // thigh and toward her front. A smooth field of position only, so
    // layered gear moves with the skin under it. Minus = in. Adds to
    // delta[0]. (w is unused: the field is not the cheeks' motion region.)
    void rear_flare(const RearShape& s, const RearWeights& w, const float p[3], float amount,
                    float delta[3]);

    // The cleft closed: each side moved toward her midline by amount x a
    // band that rises over the first close_x1 from the seam, holds, and is
    // gone by close_x2 -- never across the midline (the band rises over at
    // least twice the amount), never the outer hips. Back of her only, the
    // flare's height. Adds to delta[0].
    void rear_cleft_close(const RearShape& s, const float p[3], float amount, float delta[3]);

    // The whole baseline (rest) shape: fullness, flare and cleft close, all
    // in model units. Adds to delta.
    void rear_baseline(const RearShape& s, const RearWeights& w, const float p[3], float fullness,
                       float flare, float cleft_close, float delta[3]);

    // A smooth 0..1 step (smoothstep of (v - lo) / (hi - lo)).
    float smooth_between(float v, float lo, float hi);

    // The absolute most a point may be moved from where it rests: a vector's
    // length becomes limit x tanh(length / limit) -- one to one when small,
    // easing in smoothly, never reaching the limit -- so no push or pull,
    // whatever the gain, goes beyond it and none stops dead at it. A
    // limit <= 0 leaves it unlimited.
    void soft_limit_vector(float v[3], float limit);

    // A short, rounded to nearest and clamped to int16.
    short round_short(float v);

    // --- fitting the cheeks to her mesh --------------------------------------

    // What the fit found, in pelvis-local bind units. peak: the rearmost
    // part of each cheek (mean of its few rearmost points); cap_lo / cap_hi:
    // the x and z extent of the skin within kCapDepth of that peak; centre /
    // radius: the region the defaults put there. fallback: too few points
    // (or a peak outside where a cheek can be), so the nominal shape is used.
    struct RearFit
    {
        bool ok = false;
        bool fallback = false;
        int samples[2] = { 0, 0 };      // points considered per side
        float peak[2][3] = {};
        float cap_lo[2][2] = {};        // x, z (x signed, as the mesh has it)
        float cap_hi[2][2] = {};
        float centre[2][3] = {};
        float radius[2][3] = {};        // [2]: down from the centre
        float up[2] = {};               // height radius above the centre
        float lean[2] = {};             // the upper half's bend to her front
        float seam_x = 0.0f;
        // Landmarks (signed x, as the mesh has it): the glute's outer edge
        // (where the back outline at peak height has fallen kEdgeDrop below
        // the peak), the hip side (fallen kHipDrop), the crease under the
        // glute (the column's back-most point fallen kFoldDrop below the
        // peak going down) and its top (above the peak by half the peak-
        // to-crease height: the peak sits in the glute's upper third).
        float edge_x[2] = {};
        float hip_x[2] = {};
        float fold_z[2] = {};
        float top_z[2] = {};
        bool landmarks[2] = { false, false };   // measured (else estimated)
    };

    // Points: pelvis-local bind positions (x, y, z triples), any vertices
    // of her hips and thighs; metre: model units per metre. Looks at the
    // back (+y) skin below the belt and above the thigh (z -0.20 .. -0.02 m,
    // |x| 0.015 .. 0.17 m) on each side of x = 0, then measures the
    // landmarks. Each region covers its whole glute with a soft margin:
    // centred between the midline and the outer edge and between the top
    // and the crease, radii kRegionMargin x those half extents, 5 cm
    // inside the peak with a 12 cm depth radius. Always fills `out` (the
    // nominal shape when the mesh does not fit).
    bool rear_fit(const float* points, int count, float metre, RearFit& out);

    // The sliders, relative to the fitted shape: spread_cm moves each cheek
    // out (+) / in (-) from its fitted place by (spread_cm - 8) cm (8, the
    // default, = as fitted); lift_cm up / down; size x (width, 1, height)
    // scales the radii; core and seam_cm as they are; cleft band of half
    // width cleft_width_cm, pushed in by cleft_cm (rear_deform's cleft
    // offset comes from cleft_offset_y).
    struct RearAdjust
    {
        float size = 1.0f;
        float width = 1.0f;
        float height = 1.0f;
        float spread_cm = 8.0f;
        float lift_cm = 0.0f;
        float core = 0.5f;
        float seam_cm = 1.0f;
        float cleft_width_cm = 2.0f;
        float seam_inset_cm = 0.0f;
        float flare_start_cm = 3.9f;
    };
    const float kSpreadDefaultCm = 8.0f;

    void rear_shape_from_fit(const RearFit& fit, const RearAdjust& adj, float metre, RearShape& out);

    // God mode: cheek `side`'s touch zone, an ellipsoid round its fitted
    // region with its back edge 1 cm outside the peak and 2 cm beyond the
    // cap at the sides, moved with spread/lift and grown by touch_cm.
    void rear_touch_zone(const RearFit& fit, const RearAdjust& adj, int side, float metre,
                         float touch_cm, float centre[3], float semi[3]);

    // Is p inside the axis-aligned ellipsoid (centre, semi)? depth: first-
    // order distance to its surface; normal: outward, unit (the gradient).
    // margin grows every semi-axis.
    bool ellipsoid_contact(const float p[3], const float centre[3], const float semi[3],
                           float margin, float* depth, float normal[3]);

    // --- the pelvis frame --------------------------------------------------

    // m: the pelvis bone's world matrix (row-vector 4x4, translation in
    // [12..14]); a uniform scale divides out. A world direction / point into
    // pelvis-local, and a pelvis-local point / direction into the world.
    // The *_to_local ones return false on a degenerate matrix.
    bool world_dir_to_local(const float m[16], const float v[3], float out[3]);
    bool world_point_to_local(const float m[16], const float p[3], float out[3]);
    void local_point_to_world(const float m[16], const float p[3], float out[3]);
    void local_dir_to_world(const float m[16], const float v[3], float out[3]);
    // The mean length of its rows (its scale); 0 when degenerate.
    float matrix_scale(const float m[16]);

    // A cheek's shown offset in pelvis-local units from the spring's lag
    // (already pelvis-local): each axis x its gain, the gains eased toward 1
    // by `boost` (a hand on it), gains below 1 then eased toward 1 by
    // `event` (0..1, a big motion: see event_ramp), motion into her body
    // (-y) also x inward, then test_push added along +y (back).
    void cheek_offset(const float lag_local[3], const float gain[3], float boost, float inward,
                      float test_push, float out[3], float event = 0.0f);

    // How big a motion this is (Underworld's big motion ramp): 0 .. 1, a
    // smoothstep of the lag's length over min(ramp, 0.6 x max_lag) (the
    // shown lag never passes max_lag, so a longer ramp would never reach
    // full). ramp <= 0: 0.
    float event_ramp(const float lag[3], float ramp, float max_lag);
}
}
