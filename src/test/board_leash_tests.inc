// Tests for src/vr/board_leash (included by mathtest.cpp).
#include "../vr/godmode/board_leash.h"

static void test_board_leash()
{
    using namespace trlvr;
    // Inside the radius x/y stay; outside the anchor is pulled to exactly
    // the radius, direction kept; height always follows.
    float a[3] = { 0.0f, 0.0f, 0.0f };
    const float l1[3] = { 30.0f, 40.0f, 7.0f };         // 50 away
    board_leash(a, l1, 80.0f);
    CHECK(near(a[0], 0.0f) && near(a[1], 0.0f) && near(a[2], 7.0f));
    board_leash(a, l1, 20.0f);                          // pulled 30 along (0.6, 0.8)
    CHECK(near(a[0], 18.0f) && near(a[1], 24.0f) && near(a[2], 7.0f));
    CHECK(near(std::sqrt((30.0f - a[0]) * (30.0f - a[0]) + (40.0f - a[1]) * (40.0f - a[1])), 20.0f));
    const float l2[3] = { 30.0f, 40.0f, -3.0f };
    board_leash(a, l2, 80.0f);
    CHECK(near(a[0], 18.0f) && near(a[1], 24.0f) && near(a[2], -3.0f));
    // Radius 0 puts the anchor on her; a height band pulls only past its edge.
    board_leash(a, l1, 0.0f);
    CHECK(near(a[0], 30.0f) && near(a[1], 40.0f));
    float b[3] = { 0.0f, 0.0f, 0.0f };
    const float l3[3] = { 0.0f, 0.0f, 10.0f };
    board_leash(b, l3, 5.0f, 4.0f);
    CHECK(near(b[2], 6.0f));
    board_leash(b, l3, 5.0f, 40.0f);
    CHECK(near(b[2], 6.0f));

    // The board stick's dead zone: 0 inside 0.2, its edge 0, full tilt 1.
    CHECK(near(board_stick_axis(0.1f), 0.0f));
    CHECK(near(board_stick_axis(-0.19f), 0.0f));
    CHECK(near(board_stick_axis(0.2f), 0.0f));
    CHECK(near(board_stick_axis(0.6f), 0.5f));
    CHECK(near(board_stick_axis(-0.6f), -0.5f));
    CHECK(near(board_stick_axis(1.0f), 1.0f));
    CHECK(near(board_stick_axis(1.5f), 1.0f));
    CHECK(near(board_stick_axis(std::nanf("")), 0.0f));

    // Zoom: stick up (in > 0) brings the view closer, exp(-in*speed*dt).
    {
        float zoom = 1.0f, scale = 10.0f;
        CHECK(board_stick_step(&zoom, &scale, 1.0f, 0.0f, 1.0f, 0.7f, 0.1f,
                               0.3f, 3.5f, 1.25f, 25.0f));
        CHECK(near(zoom, std::exp(-0.1f)));
        CHECK(near(scale, 10.0f));                      // no scale input
        // Down zooms out by the same ratio: back to 1.
        board_stick_step(&zoom, &scale, -1.0f, 0.0f, 1.0f, 0.7f, 0.1f,
                         0.3f, 3.5f, 1.25f, 25.0f);
        CHECK(near(zoom, 1.0f));
        // Clamped at both ends.
        for (int i = 0; i < 100; ++i)
            board_stick_step(&zoom, &scale, 1.0f, 0.0f, 5.0f, 0.7f, 0.1f,
                             0.3f, 3.5f, 1.25f, 25.0f);
        CHECK(near(zoom, 0.3f));
        CHECK(!board_stick_step(&zoom, &scale, 1.0f, 0.0f, 5.0f, 0.7f, 0.1f,
                                0.3f, 3.5f, 1.25f, 25.0f));
        for (int i = 0; i < 100; ++i)
            board_stick_step(&zoom, &scale, -1.0f, 0.0f, 5.0f, 0.7f, 0.1f,
                             0.3f, 3.5f, 1.25f, 25.0f);
        CHECK(near(zoom, 3.5f));
    }
    // Scale: stick up makes Lara bigger (a smaller shrink factor); clamped
    // to 1.25 .. 25 (Lara 140 .. 7 cm).
    {
        float zoom = 1.0f, scale = 10.0f;
        board_stick_step(&zoom, &scale, 0.0f, 1.0f, 1.0f, 0.7f, 0.2f,
                         0.3f, 3.5f, 1.25f, 25.0f);
        CHECK(near(scale, 10.0f * std::exp(-0.14f)));
        CHECK(near(zoom, 1.0f));
        for (int i = 0; i < 200; ++i)
            board_stick_step(&zoom, &scale, 0.0f, 1.0f, 5.0f, 0.7f, 0.1f,
                             0.3f, 3.5f, 1.25f, 25.0f);
        CHECK(near(scale, 1.25f));
        for (int i = 0; i < 200; ++i)
            board_stick_step(&zoom, &scale, 0.0f, -1.0f, 5.0f, 0.7f, 0.1f,
                             0.3f, 3.5f, 1.25f, 25.0f);
        CHECK(near(scale, 25.0f));
    }
    // A hitch (dt 0.5 or more), no time, or a zero speed changes nothing.
    {
        float zoom = 1.0f, scale = 10.0f;
        CHECK(!board_stick_step(&zoom, &scale, 1.0f, 1.0f, 1.0f, 0.7f, 0.5f,
                                0.3f, 3.5f, 1.25f, 25.0f));
        CHECK(!board_stick_step(&zoom, &scale, 1.0f, 1.0f, 1.0f, 0.7f, 0.0f,
                                0.3f, 3.5f, 1.25f, 25.0f));
        CHECK(!board_stick_step(&zoom, &scale, 1.0f, 1.0f, 0.0f, 0.0f, 0.1f,
                                0.3f, 3.5f, 1.25f, 25.0f));
        CHECK(near(zoom, 1.0f) && near(scale, 10.0f));
        // No input on an axis leaves it as it was, even outside the range.
        float wide = 1.0f;
        board_stick_step(&zoom, &wide, 1.0f, 0.0f, 1.0f, 0.7f, 0.1f,
                         0.3f, 3.5f, 1.25f, 25.0f);
        CHECK(near(wide, 1.0f));
    }
}
