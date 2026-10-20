// Tests for src/vr/menu_panel (included by mathtest.cpp).
#include "../vr/menu_panel.h"

static void test_menu_panel()
{
    using namespace trlvr;
    // Straight ahead (-z), no turn: 1 m out, width +x, height +y.
    const float head[3] = { 0.5f, 1.6f, -0.2f };
    MenuPanelPose p{};
    CHECK(menu_panel_place(head, 0.0f, -2.0f, 1.0f, 0.0f, 0.0f, &p));
    CHECK(near(p.centre[0], 0.5f) && near(p.centre[1], 1.6f) && near(p.centre[2], -1.2f));
    CHECK(near(p.right[0], 1.0f) && near(p.right[1], 0.0f) && near(p.right[2], 0.0f));
    CHECK(near(p.up[0], 0.0f) && near(p.up[1], 1.0f) && near(p.up[2], 0.0f));
    // 90 degrees left: out along -x, width now toward -z (still the
    // viewer's right as they face it).
    CHECK(menu_panel_place(head, 0.0f, -1.0f, 2.0f, 90.0f, 0.0f, &p));
    CHECK(near(p.centre[0], -1.5f) && near(p.centre[1], 1.6f) && near(p.centre[2], -0.2f));
    CHECK(near(p.right[0], 0.0f) && near(p.right[2], -1.0f));
    // Facing +x, whose right is +z: 90 degrees right (-90 left) goes there.
    CHECK(menu_panel_place(head, 1.0f, 0.0f, 1.0f, -90.0f, 0.0f, &p));
    CHECK(near(p.centre[0], 0.5f) && near(p.centre[2], 0.8f));
    // Pitched up 30: up 0.5 m at 1 m, top edge leaning back toward the head,
    // axes orthonormal and the panel square to the line of sight.
    CHECK(menu_panel_place(head, 0.0f, -1.0f, 1.0f, 0.0f, 30.0f, &p));
    CHECK(near(p.centre[1], 2.1f) && near(p.centre[2], -0.2f - 0.8660254f));
    CHECK(p.up[2] > 0.0f && near(p.up[1], 0.8660254f));
    {
        const float d[3] = { p.centre[0] - head[0], p.centre[1] - head[1], p.centre[2] - head[2] };
        const float du = d[0] * p.up[0] + d[1] * p.up[1] + d[2] * p.up[2];
        const float dr = d[0] * p.right[0] + d[1] * p.right[1] + d[2] * p.right[2];
        const float ur = p.up[0] * p.right[0] + p.up[1] * p.right[1] + p.up[2] * p.right[2];
        CHECK(near(du, 0.0f) && near(dr, 0.0f) && near(ur, 0.0f));
    }
    CHECK(!menu_panel_place(head, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, &p));
    // Corners: strip order TL, TR, BL, BR.
    CHECK(menu_panel_place(head, 0.0f, -1.0f, 1.0f, 0.0f, 0.0f, &p));
    float c[4][3];
    menu_panel_corners(p, 0.25f, c);
    CHECK(near(c[0][0], 0.25f) && near(c[0][1], 1.85f));
    CHECK(near(c[1][0], 0.75f) && near(c[1][1], 1.85f));
    CHECK(near(c[2][0], 0.25f) && near(c[2][1], 1.35f));
    CHECK(near(c[3][0], 0.75f) && near(c[3][1], 1.35f) && near(c[3][2], -1.2f));
    // 90 degrees wide at 1 m is 1 m each way.
    CHECK(near(menu_panel_half_size(1.0f, 90.0f), 1.0f));
    CHECK(near(menu_panel_half_size(2.0f, 38.0f), 2.0f * std::tan(19.0f * 0.01745329f)));

    // Hold-to-open: a tap is a pause, a hold opens and never reaches the game.
    MenuHold h;
    bool game = true;
    CHECK(menu_hold_update(h, false, 1000, 0.6f, false, &game) == MenuHoldNone && !game);
    CHECK(menu_hold_update(h, true, 1000, 0.6f, false, &game) == MenuHoldNone && !game);
    CHECK(menu_hold_update(h, true, 1300, 0.6f, false, &game) == MenuHoldNone && !game);
    CHECK(menu_hold_update(h, false, 1400, 0.6f, false, &game) == MenuHoldTap && !game);
    CHECK(menu_hold_update(h, false, 1500, 0.6f, false, &game) == MenuHoldNone);
    CHECK(menu_hold_update(h, true, 2000, 0.6f, false, &game) == MenuHoldNone);
    CHECK(menu_hold_update(h, true, 2599, 0.6f, false, &game) == MenuHoldNone);
    CHECK(menu_hold_update(h, true, 2600, 0.6f, false, &game) == MenuHoldOpen && !game);
    // Now open: still down, then released, then pressed to close -- the
    // overlay's, never the game's, and no tap when it closes with it down.
    CHECK(menu_hold_update(h, true, 2700, 0.6f, true, &game) == MenuHoldNone && !game);
    CHECK(menu_hold_update(h, false, 2800, 0.6f, true, &game) == MenuHoldNone);
    CHECK(menu_hold_update(h, true, 3000, 0.6f, true, &game) == MenuHoldNone);
    CHECK(menu_hold_update(h, true, 3100, 0.6f, false, &game) == MenuHoldNone && !game);
    CHECK(menu_hold_update(h, true, 4000, 0.6f, false, &game) == MenuHoldNone && !game);
    CHECK(menu_hold_update(h, false, 4100, 0.6f, false, &game) == MenuHoldNone && !game);
    // Off: straight through, held as held.
    CHECK(menu_hold_update(h, true, 5000, 0.0f, false, &game) == MenuHoldNone && game);
    CHECK(menu_hold_update(h, true, 9000, 0.0f, false, &game) == MenuHoldNone && game);
    CHECK(menu_hold_update(h, false, 9100, 0.0f, false, &game) == MenuHoldNone && !game);
}
