// Tests for src/vr/rear_jiggle (included by mathtest.cpp).
#include "../vr/physics/rear_jiggle.h"

static void test_rear_jiggle()
{
    using namespace trlvr;
    JiggleParams p;
    p.hz = 3.0f;
    p.damping = 0.3f;
    p.max_offset = 5.0f;
    p.reset_distance = 50.0f;
    // At rest: no offset, ever.
    {
        JiggleState s;
        const float t[3] = { 1, 2, 3 };
        float off[3];
        bool still = true;
        for (int i = 0; i < 200; ++i)
        {
            jiggle_step(s, t, 1.0f / 90.0f, p, off);
            still = still && off[0] == 0.0f && off[1] == 0.0f && off[2] == 0.0f;
        }
        CHECK(still);
    }
    // Steady motion leaves no lag; a stop swings the mass on, then it settles.
    {
        JiggleState s;
        float t[3] = { 0, 0, 0 }, off[3] = {};
        const float dt = 1.0f / 90.0f;
        for (int i = 0; i < 270; ++i)               // 3 s at 30 units/s
        {
            t[0] += 30.0f * dt;
            jiggle_step(s, t, dt, p, off);
        }
        CHECK(std::fabs(off[0]) < 0.01f);
        float peak = 0.0f;
        for (int i = 0; i < 90; ++i)                // stopped
        {
            jiggle_step(s, t, dt, p, off);
            peak = off[0] > peak ? off[0] : peak;
        }
        CHECK(peak > 0.3f && peak <= 5.0f);         // it carried on forward
        for (int i = 0; i < 450; ++i)
            jiggle_step(s, t, dt, p, off);
        CHECK(std::fabs(off[0]) < 0.01f);           // settled
    }
    // The limit holds; a jump beyond reset_distance, or a bad dt, resets.
    {
        JiggleState s;
        float t[3] = { 0, 0, 0 }, off[3];
        jiggle_step(s, t, 0.01f, p, off);
        t[2] = 40.0f;                               // under the reset distance
        jiggle_step(s, t, 0.01f, p, off);
        CHECK(std::sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]) <= 5.0f + 1e-4f);
        CHECK(off[2] < -1.0f);                      // left behind, downward
        t[2] = 200.0f;
        jiggle_step(s, t, 0.01f, p, off);
        CHECK(off[0] == 0.0f && off[1] == 0.0f && off[2] == 0.0f);
        t[2] = 210.0f;
        jiggle_step(s, t, 0.5f, p, off);            // a hitch: reset, not a fling
        CHECK(off[2] == 0.0f);
    }
    // Contact: a press moves the mass with the hand; a grip holds it there
    // closely; on release it springs back; a kick sets it swinging.
    {
        JiggleParams q = p;
        q.touch_offset = 5.0f;
        JiggleState s;
        const float t[3] = { 0, 0, 0 };
        float off[3];
        const float dt = 1.0f / 90.0f;
        jiggle_step(s, t, dt, q, off);
        JiggleContact c;
        c.touched = true;
        c.shift[1] = -4.0f;                         // pressed 4 units in
        for (int i = 0; i < 90; ++i)
            jiggle_step(s, t, dt, q, off, &c);
        CHECK(off[1] < -3.0f && off[1] > -4.5f);    // follows the press (limit loosened)
        c.gripped = true;
        c.shift[1] = 0.0f;
        c.shift[2] = 6.0f;                          // gripped and lifted
        float worst = 0.0f;
        for (int i = 0; i < 90; ++i)
        {
            jiggle_step(s, t, dt, q, off, &c);
            if (i > 45)
                worst = std::fabs(off[2] - 6.0f * std::tanh(6.0f / 10.0f) * 10.0f / 6.0f) > worst
                            ? std::fabs(off[2] - 6.0f * std::tanh(6.0f / 10.0f) * 10.0f / 6.0f) : worst;
        }
        CHECK(off[2] > 4.0f && worst < 0.2f);       // held steadily (no ringing)
        float peak = 0.0f;
        for (int i = 0; i < 90; ++i)                // released
        {
            jiggle_step(s, t, dt, q, off);
            peak = off[2] < peak ? off[2] : peak;
        }
        CHECK(peak < -0.5f);                        // swung past rest on the way back
        for (int i = 0; i < 360; ++i)
            jiggle_step(s, t, dt, q, off);
        CHECK(std::fabs(off[2]) < 0.05f && s.boost < 0.05f);
        JiggleContact k;
        k.kick[0] = 60.0f;
        jiggle_step(s, t, dt, q, off, &k);
        k.kick[0] = 0.0f;
        float swing = 0.0f;
        for (int i = 0; i < 30; ++i)
        {
            jiggle_step(s, t, dt, q, off, &k);
            swing = off[0] > swing ? off[0] : swing;
        }
        CHECK(swing > 1.0f && swing < 5.0f + 5.0f);
    }
    // A hard stop far beyond the limit: the shown offset rounds off toward
    // the limit (no frame-to-frame jump near the peak), the mass is slowed
    // by the stiffening rather than parked on the safety wall.
    {
        JiggleParams q = p;
        q.max_offset = 2.0f;
        JiggleState s;
        float t[3] = { 0, 0, 0 }, off[3] = {};
        const float dt = 1.0f / 90.0f;
        for (int i = 0; i < 90; ++i)
        {
            t[0] += 300.0f * dt;
            jiggle_step(s, t, dt, q, off);
        }
        float peak = 0.0f, last = off[0], worst_step = 0.0f, mass_peak = 0.0f;
        for (int i = 0; i < 60; ++i)
        {
            if (i < 9)                              // stopping over 0.1 s
                t[0] += 300.0f * (1.0f - (float)(i + 1) / 9.0f) * dt;
            jiggle_step(s, t, dt, q, off);
            peak = off[0] > peak ? off[0] : peak;
            mass_peak = s.pos[0] - t[0] > mass_peak ? s.pos[0] - t[0] : mass_peak;
            if (off[0] > 1.5f && last > 1.5f && std::fabs(off[0] - last) > worst_step)
                worst_step = std::fabs(off[0] - last);
            last = off[0];
        }
        CHECK(peak > 1.5f && peak < 2.0f);
        CHECK(worst_step < 0.15f);
        CHECK(mass_peak < 3.0f * 2.0f - 0.1f);
    }
    // Frame rate: the same motion at 30 and 90 fps gives the same swing.
    {
        auto run = [&](int fps, float at, float out[3])
        {
            JiggleState s;
            const float dt = 1.0f / (float)fps;
            const int frames = (int)std::lround(at * fps);
            for (int i = 0; i <= frames; ++i)
            {
                const float time = i * dt;
                const float t[3] = { 0, 0, 8.0f * std::sin(6.0f * time) * (time < 1.0f ? 1.0f : 0.0f) +
                                               (time >= 1.0f ? 8.0f * std::sin(6.0f) : 0.0f) };
                jiggle_step(s, t, dt, p, out);
            }
        };
        float a[3], b[3];
        run(30, 1.2f, a);
        run(90, 1.2f, b);
        CHECK(std::fabs(a[2]) > 0.2f);
        CHECK(near(a[2], b[2], 0.05f * std::fabs(b[2]) + 0.02f));
    }
}
