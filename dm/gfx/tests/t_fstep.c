// Frame step controller (nvn_fstep.h), simulated in vsyncs: a frame whose work w fits its hold k stays on screen k vsyncs, a longer one takes w.
// Game time is the sum of the steps, real time the sum of the frame times.
#include "../nvn/nvn_fstep.h"
#include <stdio.h>
#include <string.h>

static int fail, checks;
static void check(int ok, const char *what) { checks++; fail += !ok; printf("%-78s %s\n", what, ok ? "✓" : "✗"); }

typedef struct { nvn_fstep s; double real, game; int next, slow; } sim;
// n frames of work w (vsyncs); slow: the CPU reported slowed for reasons other than heat (probes)
static void run(sim *m, int n, double w, int kmin, int kmax, int fixed) {
    for (int i = 0; i < n; i++) {
        int k = m->s.k ? m->s.k : kmin, step = m->next ? m->next : kmin;
        double shown = w > k ? w : k;
        m->real += shown; m->game += step;
        m->next = nvn_fstep_next(&m->s, shown, w, kmin, kmax, fixed, m->slow);
    }
}

// seven frames of 1.6 vsyncs: 4.2 vsyncs behind in the last 30 frames, 3.6 without the longest, over FSTEP_LATE a frame
static void hitch(sim *m) { run(m, 7, 1.6, 1, 3, 0); }

int main(void) {
    {   // 120 fps (fps_high): kmin 1, kmax 2, in ticks of 1/120 s
        sim m = {0}; m.s.per_vsync = 2; m.s.no_payback = 1;
        run(&m, 120, 0.7, 1, 2, 0); check(m.s.k == 1, "120 fps: frames of 5.8 ms hold 120");
        check(m.s.hold == FSTEP_HOLD0 * 2, "120 fps: the wait before a trial is the same real time");
        run(&m, 10, 1.6, 1, 2, 0); check(m.s.k == 2, "120 fps: frames of 13 ms step down to 60");
        run(&m, 600, 1.6, 1, 2, 0); check(m.s.k == 2, "120 fps: they stay at 60 while they do not fit");
        run(&m, 60, 4.0, 1, 2, 0); check(m.s.k == 2, "120 fps: 33 ms frames stop at 60, never a longer hold");
        run(&m, 1200, 0.7, 1, 2, 0); check(m.s.k == 1, "120 fps: frames that fit return to 120");
        sim c = {0}; c.s.per_vsync = 2; c.s.no_payback = 1;
        run(&c, 120, 0.7, 1, 2, 0); run(&c, 30, 0.7, 2, 2, 0); check(c.s.k == 2, "120 fps: a cap of 60 (a movie, a 60 Hz screen) holds two ticks");
        run(&c, 120, 0.7, 1, 2, 0); check(c.s.k == 1, "120 fps: the cap's end returns to 120");
    }
    char b[160];
    {   sim m = {0}; run(&m, 600, 0.5, 1, 3, 0);
        check(m.s.k == 1 && m.s.ups == 0 && m.s.paid == 0 && m.game == m.real, "steady 60: hold 1, step 1, game time = real time");
    }
    {   sim m = {0}; run(&m, 120, 0.5, 1, 3, 0); run(&m, 600, 1.32, 1, 3, 0);
        snprintf(b, sizeof b, "steady 22 ms frames: 30 fps after three late frames, no flapping (k %d, ups %llu, downs %llu)", m.s.k, (unsigned long long)m.s.ups, (unsigned long long)m.s.downs);
        check(m.s.k == 2 && m.s.ups == 1 && m.s.downs == 0, b);
        snprintf(b, sizeof b, "  game time %.0f against real time %.2f (debt %.2f)", m.game, m.real, m.s.debt);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.debt < 1, b);
    }
    {   // three in a row late by 4 ms every 20 frames: the step pays them back, so hold 1 stays
        sim m = {0}; run(&m, 120, 0.5, 1, 3, 0);
        for (int i = 0; i < 3000; i++) run(&m, 1, i % 20 < 3 ? 1.25 : 0.9, 1, 3, 0);
        snprintf(b, sizeof b, "near-misses, 3 of every 20 frames late by 4 ms: hold 1 (k %d, ups %llu, late %llu)", m.s.k, (unsigned long long)m.s.ups, (unsigned long long)m.s.late);
        check(m.s.k == 1 && m.s.ups == 0 && m.s.late >= 400, b);
        snprintf(b, sizeof b, "  game time %.0f against real time %.2f (debt %.2f, paid %llu)", m.game, m.real, m.s.debt, (unsigned long long)m.s.paid);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.paid > 0, b);
        sim h = {0}; run(&h, 120, 0.5, 1, 3, 0); run(&h, 600, 0.9, 1, 3, 0); run(&h, 3, 1.6, 1, 3, 0); run(&h, 60, 0.9, 1, 3, 0);
        check(h.s.k == 1 && h.s.ups == 0, "  a hitch of three 27 ms frames among frames on time: hold 1");
    }
    {   // lone stalls among near-misses: each long frame counts FSTEP_SHORT_MAX (2) of the 30 frames' limit of 3, and the step pays it back
        static const struct { int at; double ms; } st[] = {{1913, 67.7}, {10267, 57.7}, {10286, 43.1}, {10454, 54.9}, {11396, 65.4}};
        nvn_fstep s = {0}; int next = 1, at2 = 0; double real = 0, game = 0;
        for (int p = 0; p < 12000; p++) {
            int k = s.k ? s.k : 1; double shown = k == 1 && p % 20 >= 5 && p % 20 < 8 ? 1.3 : k;
            for (unsigned i = 0; i < sizeof st / sizeof *st; i++) if (p == st[i].at) shown = st[i].ms / (1000.0 / 60);
            real += shown; game += next; next = nvn_fstep_next(&s, shown, 0.45, 1, 3, 0, 0); at2 += s.k > 1;
        }
        snprintf(b, sizeof b, "lone stalls of 43-68 ms among near-misses (MKW, M4 Max): hold 1 (ups %llu, %d frames at hold 2)", (unsigned long long)s.ups, at2);
        check(s.ups == 0 && at2 == 0, b);
        snprintf(b, sizeof b, "  game time %.0f against real time %.2f (debt %.2f)", game, real, s.debt);
        check(real - game - s.debt > -1.01 && real - game - s.debt < 1.01, b);
    }
    {   // the step goes to the cheapest of the last three late frames (2), not to the hitch (3)
        sim m = {0}; run(&m, 120, 0.5, 1, 3, 0); run(&m, 11, 1.3, 1, 3, 0);
        check(m.s.k == 1, "scene at 1.3 vsyncs: eleven late frames are 3 vsyncs behind without the longest, not yet short of hold 1");
        run(&m, 1, 3.0, 1, 3, 0);
        snprintf(b, sizeof b, "  a 3-vsync frame then: hold 2, not 3 (k %d)", m.s.k);
        check(m.s.k == 2 && m.s.ups == 1, b);
    }
    {   // 120 fps has no payback (the game steps by its target): a late frame is slow motion
        sim m = {0}; m.s.per_vsync = 2; m.s.no_payback = 1; run(&m, 120, 0.7, 1, 2, 0); run(&m, 3, 1.25, 1, 2, 0);
        check(m.s.k == 2, "120 fps, no payback: three frames late by 2 ms step down to 60");
    }
    {   sim m = {0}; run(&m, 120, 0.5, 1, 3, 0); run(&m, 300, 1.12, 1, 3, 0);
        check(m.s.k == 2 && m.s.ups == 1, "steady 18.7 ms frames (53 fps): 30 fps, not 53 with a payback every 8 frames");
    }
    {   sim m = {0}; run(&m, 120, 0.5, 1, 3, 0);
        for (int i = 0; i < 20; i++) { m.s.screen = 1; m.next = nvn_fstep_next(&m.s, 1.0, 1.3, 1, 3, 0, 0); }
        check(m.s.k == 1 && m.s.ups == 0, "snapshots: costly frames the screen shows on time keep hold 1");
        for (int i = 0; i < 3; i++) { m.s.screen = 2; m.next = nvn_fstep_next(&m.s, 1.0, 0.8, 1, 3, 0, 0); }
        check(m.s.k == 2 && m.s.ups == 1, "  three frames late on screen step up, whatever their cost");
        m.next = nvn_fstep_next(&m.s, 2.0, 0.5, 1, 3, 0, 0);
        check(m.s.screen == 0, "  the screen verdict holds for one call");
    }
    {   sim m = {0}; run(&m, 120, 0.5, 1, 3, 0);
        for (int i = 0; i < 600; i++) { m.s.screen = 1; m.next = nvn_fstep_next(&m.s, 1.0, 1.12, 1, 3, 0, 0); }
        for (int i = 0; i < 3; i++) { m.s.screen = 2; m.next = nvn_fstep_next(&m.s, 1.0, 1.12, 1, 3, 0, 0); }
        for (int i = 0; i < 600; i++) { m.s.screen = 1; m.next = nvn_fstep_next(&m.s, 2.0, 1.12, 1, 3, 0, 0); }
        snprintf(b, sizeof b, "snapshots, 18.7 ms work shown on time at hold 1, then late: hold 2 stays, no probes (k %d, probes %llu)", m.s.k, (unsigned long long)m.s.probes);
        check(m.s.k == 2 && m.s.probes == 0 && m.s.downs == 0, b);
    }
    {   sim m = {0}; run(&m, 60, 0.5, 1, 3, 0);
        m.real += 6; m.game += m.next; m.next = nvn_fstep_next(&m.s, 6, 6, 1, 3, 0, 0);
        check(m.next == 2 && m.s.k == 1, "100 ms hitch: the next step pays back one vsync, hold stays 1");
        int two = 0; for (int i = 0; i < 20; i++) { int st = m.next; run(&m, 1, 0.5, 1, 3, 0); two += st == 2; }
        snprintf(b, sizeof b, "  five steps of 2, then 1 (%d steps of 2); game time %.0f = real time %.0f", two, m.game, m.real);
        check(two == 5 && m.game == m.real && m.s.k == 1, b);
    }
    {   sim m = {0}; run(&m, 60, 0.5, 1, 3, 0);
        m.real += 30; m.game += m.next; m.next = nvn_fstep_next(&m.s, 30, 30, 1, 3, 0, 0);
        run(&m, 60, 0.5, 1, 3, 0);
        snprintf(b, sizeof b, "0.5 s stall: 15 vsyncs paid back, %.0f dropped, not replayed (paid %llu)", m.s.dropped, (unsigned long long)m.s.paid);
        check(m.s.dropped == 14 && m.s.paid == 15 && m.real - m.game == 14 && m.s.k == 1, b);
    }
    {   sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0);
        check(m.s.k == 2, "heavy scene: hold 2");
        run(&m, 29, 0.6, 1, 3, 0);
        check(m.s.k == 2, "  cost drops: 29 frames that fit 1 are not enough");
        run(&m, 1, 0.6, 1, 3, 0);
        check(m.s.k == 1 && m.s.trial == FSTEP_RUN, "  the 30th starts a trial at hold 1");
        run(&m, FSTEP_RUN + 10, 0.6, 1, 3, 0);
        check(m.s.k == 1 && m.s.trial == 0 && m.s.hold == FSTEP_HOLD0 && m.s.failed == 0, "  the trial holds: 60 fps again");
    }
    {   // the cost misses something: each trial fails, and the trials spread out 1 → 2 → 4 → 8 s
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 30, 1.5, 1, 3, 0);
        check(m.s.k == 2, "frames at hold 1 take 1.5 vsyncs: hold 2");
        double last = m.real, gaps[4]; int ng = 0; uint64_t downs = m.s.downs;
        for (int i = 0; i < 4000 && ng < 4; i++) {
            double w = m.s.k == 1 ? 1.5 : 0.6;
            run(&m, 1, w, 1, 3, 0);
            if (m.s.downs != downs) { downs = m.s.downs; gaps[ng++] = m.real - last; last = m.real; }
        }
        snprintf(b, sizeof b, "  trials %.0f, %.0f, %.0f, %.0f vsyncs apart; %llu failed", gaps[0], gaps[1], gaps[2], gaps[3], (unsigned long long)m.s.failed);
        check(ng == 4 && gaps[0] >= 60 && gaps[1] >= 120 && gaps[2] >= 240 && gaps[3] >= 480 && gaps[3] < 560 && m.s.failed >= 3, b);
    }
    {   sim m = {0}; run(&m, 300, 0.5, 2, 2, 0);
        check(m.s.k == 2 && m.next == 2 && m.game == m.real, "30 fps cap: hold 2, step 2");
        m.real += 6; m.game += m.next; m.next = nvn_fstep_next(&m.s, 6, 6, 2, 2, 0, 0);
        int three = 0; for (int i = 0; i < 10; i++) { int st = m.next; run(&m, 1, 0.5, 2, 2, 0); three += st == 3; }
        snprintf(b, sizeof b, "  a hitch at the 30 cap is paid back with steps of 3 (%d), hold stays 2", three);
        check(three == 4 && m.s.k == 2 && m.game == m.real, b);
        run(&m, 300, 3.5, 2, 2, 0);
        check(m.s.k == 2, "  frames longer than the hold at the 30 cap: hold stays 2 (the 30 Hz link holds only even counts)");
    }
    {   sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 1, 0.5, 2, 3, 0);
        check(m.s.k == 2, "movie flag (kmin 2): hold 2 at once");
        run(&m, 120, 0.5, 2, 3, 0); run(&m, 1, 0.5, 1, 3, 0);
        check(m.s.k == 1 && m.s.ups == 0 && m.s.late == 0, "  movie ends (kmin 1): hold 1 at once");
    }
    {   sim m = {0}; run(&m, 600, 0.86, 1, 3, 0); run(&m, 600, 1.5, 2, 2, 0); run(&m, 1, 1.5, 1, 3, 0);
        check(m.s.k == 1 && m.s.grace == FSTEP_GRACE && m.s.trial == FSTEP_RUN && m.s.probes == 1,
              "cap release: count a probe, with all grace frames left for hold 1");
        int frames = 0;
        while (m.s.k == 1 && frames < 100) { run(&m, 1, 1.3, 1, 3, 0); frames++; }
        check(frames == FSTEP_GRACE + 3 && m.s.k == 2 && m.s.failed == 1 && m.s.hold == 2 * FSTEP_HOLD0,
              "  recovery fails after 30 grace frames plus 3 late frames, then doubles the hold-off");
    }
    {   sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 120, 0.5, 1, 3, 2);
        check(m.s.k == 2 && m.next == 2 && m.s.debt == 0, "the game fixes its step at 2 (BotW's VFR override): hold 2, step 2");
        run(&m, 2, 0.5, 1, 3, 0);
        check(m.s.k == 1 && m.next == 1 && m.s.late == 0, "  the override ends: hold 1, step 1, no late frame counted");
    }
    {
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0); run(&m, 2000, 1.2, 1, 3, 0);
        check(m.s.k == 2 && m.s.downs == 0 && m.s.probes == 0, "not slowed: 1.2-vsync frames at hold 2 stay at hold 2, no probe");
    }
    {
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0); m.slow = 1;
        int at1 = 0, up_during_grace = 0; uint64_t ups = m.s.ups;
        for (int i = 0; i < 400; i++) {
            double w = m.s.k == 1 ? (at1++ < 20 ? 1.5 : 0.6) : 1.2;
            run(&m, 1, w, 1, 3, 0);
            if (m.s.grace > 0 && m.s.ups != ups) up_during_grace = 1;
        }
        snprintf(b, sizeof b, "slowed, the clock rises: one probe to hold 1 that holds (k %d, probes %llu, failed %llu)", m.s.k, (unsigned long long)m.s.probes, (unsigned long long)m.s.failed);
        check(m.s.k == 1 && m.s.probes == 1 && m.s.failed == 0 && m.s.ups == ups && !up_during_grace && m.s.trial == 0 && m.s.probe == 2 && m.s.hold == FSTEP_HOLD0, b);
        snprintf(b, sizeof b, "  20 late frames in the grace are paid back: game time %.0f against real time %.2f (debt %.2f)", m.game, m.real, m.s.debt);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.dropped == 0, b);
    }
    {
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0); m.slow = 1;
        double last = m.real, gaps[5]; int ng = 0, at1 = 0, first_len = -1; uint64_t probes = m.s.probes;
        for (int i = 0; i < 6000 && ng < 5; i++) {
            run(&m, 1, m.s.k == 1 ? 1.5 : 1.2, 1, 3, 0);
            if (m.s.k == 1) at1++; else if (at1 && first_len < 0) first_len = at1;
            if (m.s.probes != probes) { probes = m.s.probes; gaps[ng++] = m.real - last; last = m.real; }
        }
        snprintf(b, sizeof b, "slowed, hold 1 does not fit: the first probe lasts its grace + 3 late frames (%d frames at hold 1)", first_len);
        check(first_len == FSTEP_GRACE + 3, b);
        snprintf(b, sizeof b, "  probes %.0f, %.0f, %.0f, %.0f vsyncs apart; %llu failed", gaps[1], gaps[2], gaps[3], gaps[4], (unsigned long long)m.s.failed);
        check(ng == 5 && gaps[1] >= 120 && gaps[2] >= 240 && gaps[3] >= 480 && gaps[4] >= 480 && gaps[4] < 600 && m.s.failed >= 4, b);
        snprintf(b, sizeof b, "  game time %.0f against real time %.2f (debt %.2f, dropped %.0f)", m.game, m.real, m.s.debt, m.s.dropped);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.dropped == 0, b);
    }
    {   // a probe whose run at hold 1 ends within 8 s counts as failed
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0); m.slow = 1;
        for (int i = 0; i < 1000 && m.s.probe != 2; i++) run(&m, 1, m.s.k == 1 ? 0.6 : 1.2, 1, 3, 0);
        double hold0 = m.s.hold; uint64_t failed = m.s.failed;
        run(&m, 120, 0.6, 1, 3, 0); hitch(&m);
        snprintf(b, sizeof b, "slowed, hold 1 lasts 2 s after the probe: counted as failed (failed %llu, hold-off %.0f → %.0f vsyncs)", (unsigned long long)m.s.failed, hold0, m.s.hold);
        check(m.s.k == 2 && m.s.failed == failed + 1 && m.s.hold == 2 * hold0 && m.s.probe == 0, b);
        for (int i = 0; i < 2000 && m.s.probe != 2; i++) run(&m, 1, m.s.k == 1 ? 0.6 : 1.2, 1, 3, 0);
        hold0 = m.s.hold; failed = m.s.failed;
        run(&m, 600, 0.6, 1, 3, 0); hitch(&m);
        snprintf(b, sizeof b, "  hold 1 lasts 10 s after the next probe: the hold-off returns to 1 s (failed %llu, hold-off %.0f → %.0f vsyncs)", (unsigned long long)m.s.failed, hold0, m.s.hold);
        check(m.s.k == 2 && m.s.failed == failed && hold0 == 120 && m.s.hold == FSTEP_HOLD0, b);
    }
    {   sim m = {0}; m.slow = 1; run(&m, 600, 0.5, 1, 3, 0);
        check(m.s.k == 1 && m.s.probes == 0 && m.s.downs == 0 && m.game == m.real, "slowed at hold 1 already: nothing to probe");
        sim c = {0}; c.slow = 1; run(&c, 300, 0.5, 2, 2, 0);
        check(c.s.k == 2 && c.s.probes == 0, "  slowed at the 30 cap (kmin 2): nothing to probe");
    }
    {
        sim m = {0}; nvn_fgpu g = {0}; int at1 = 0, stepped = 0;
        for (int i = 0; i < 2000; i++) {
            double w = i < 600 ? 0.86 : i < 603 ? 1.25 : m.s.k >= 2 ? 1.5 : at1++ < 20 ? 1.5 : 0.86;
            stepped |= i >= 603 && m.s.k == 2;
            m.slow = nvn_fstep_gpu_slowed(&g, w, &m.s, 1);
            run(&m, 1, w, 1, 3, 0);
        }
        snprintf(b, sizeof b, "GPU clock follows the hold (BotW): one probe back to hold 1 that holds (k %d, probes %llu, failed %llu)", m.s.k, (unsigned long long)m.s.probes, (unsigned long long)m.s.failed);
        check(stepped && m.s.k == 1 && m.s.probes == 1 && m.s.failed == 0, b);
        snprintf(b, sizeof b, "  game time %.0f against real time %.2f (debt %.2f, dropped %.0f)", m.game, m.real, m.s.debt, m.s.dropped);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.dropped == 0, b);
    }
    {
        sim m = {0}; nvn_fgpu g = {0};
        for (int i = 0; i < 2000; i++) { double w = i < 600 ? 0.86 : 1.12; m.slow = nvn_fstep_gpu_slowed(&g, w, &m.s, 1); run(&m, 1, w, 1, 3, 0); }
        check(m.s.k == 2 && m.s.probes == 0, "heavier scene, GPU 0.86 → 1.12 at hold 2: not slowed, no probe");
    }
    {   // the CPU holds the frame back, not the GPU (5.7 times the GPU time but 3 ms)
        sim m = {0}; nvn_fgpu g = {0}; int slowed = 0;
        for (int i = 0; i < 1200; i++) { double gp = i < 600 || m.s.k == 1 ? 0.03 : 0.17, w = i < 600 ? 0.5 : 1.3; m.slow = nvn_fstep_gpu_slowed(&g, gp, &m.s, 1); slowed |= m.slow; run(&m, 1, w, 1, 3, 0); }
        check(m.s.k == 2 && !slowed && m.s.probes == 0, "GPU 0.03 → 0.17 under CPU lag: a GPU at 3 ms is not called slowed, no probe");
    }
    {
        sim m = {0}; nvn_fgpu g = {0}; int at1 = 0, at3 = 0, at2 = 0;
        for (int i = 0; i < 3000; i++) {
            int k = m.s.k ? m.s.k : 1;
            double w = i < 600 ? 0.75 : i < 603 ? 3.4 : k == 3 ? (i % 12 ? 1.3 : 2.0) : k == 2 ? 1.0 : at1++ < 20 ? 1.0 : 0.75;
            at3 += i >= 603 && k == 3; at2 += i >= 603 && k == 2;
            m.slow = nvn_fstep_gpu_slowed(&g, i >= 600 && i < 603 ? 0.75 : w, &m.s, 1);
            run(&m, 1, w, 1, 3, 0);
        }
        snprintf(b, sizeof b, "frames over the fit at hold 3 (OoT): a trial to 2, a probe to 1 (k %d, failed %llu, %d frames at hold 3, %d at hold 2)", m.s.k, (unsigned long long)m.s.failed, at3, at2);
        check(m.s.k == 1 && m.s.probes == 1 && m.s.failed == 0 && at3 <= 40 && at2 <= 70, b);
        sim c = {0};
        run(&c, 600, 0.75, 1, 3, 0); run(&c, 3, 3.4, 1, 3, 0);
        for (int i = 0; i < 2400; i++) run(&c, 1, i % 12 ? 1.92 : 2.0, 1, 3, 0);
        check(c.s.k == 3 && c.s.downs == 0, "  a mean of 1.93 at hold 3 does not fit hold 2: no trial");
    }
    {
        sim m = {0}; int at2 = 0;
        for (int i = 0; i < 3000; i++) {
            double w = i >= 300 && i < 310 ? 1.4 : i % 10 ? 0.8 : 1.05;
            at2 += i >= 310 && m.s.k == 2;
            run(&m, 1, w, 1, 3, 0);
        }
        snprintf(b, sizeof b, "edge scene after a hitch (OoT): frames of 1.05 among 0.8 do not keep hold 2 (k %d, ups %llu, failed %llu, %d frames at hold 2)", m.s.k, (unsigned long long)m.s.ups, (unsigned long long)m.s.failed, at2);
        check(m.s.k == 1 && m.s.ups == 1 && m.s.failed == 0 && at2 <= 70, b);
    }
    {
        for (int v = 0; v < 2; v++) {
            sim m = {0}; int at3 = 0; run(&m, 60, 0.5, 1, 3, 0); run(&m, 3, 3.5, 1, 3, 0);
            for (int i = 0; i < 600; i++) { at3 += m.s.k == 3; run(&m, 1, i % 6 == 5 ? (v ? 1.9 : 1.8) : 1.56, 1, 3, 0); }
            snprintf(b, sizeof b, "1.56 at hold 3, every sixth %.1f: hold 2 and it holds (k %d, %d frames at hold 3, failed %llu)", v ? 1.9 : 1.8, m.s.k, at3, (unsigned long long)m.s.failed);
            check(m.s.k == 2 && at3 <= 120 && m.s.failed == 0, b);
        }
    }
    {
        // a fit of 0.85 would stay at hold 2 for good: 0.90 is not 1.3 times the light scene's 0.76
        for (int v = 0; v < 2; v++) {
            sim m = {0}; nvn_fgpu g = {0}; uint32_t r = 3; int at1 = 0; double c = v ? 1.14 : 0.90;
            for (int i = 0; i < 6700; i++) {
                r = r * 1664525u + 1013904223u; double j = ((r >> 8) % 1000) / 1000.0 * 0.06 - 0.03;
                double w = i < 600 ? 0.5 + j / 3 : i < 700 ? 1.2 + j : c + j;
                at1 += i >= 700 && m.s.k == 1;
                m.slow = nvn_fstep_gpu_slowed(&g, w, &m.s, 1);
                run(&m, 1, w, 1, 3, 0);
            }
            snprintf(b, sizeof b, "%s at either hold after a heavier moment: %s (frames at hold 1 %d of 6000, failed %llu)", v ? "  1.14" : "0.90", v ? "30 fps" : "back to 60",
                     at1, (unsigned long long)m.s.failed);
            check(v ? at1 <= 200 && m.s.k == 2 : at1 >= 5900 && m.s.k == 1 && m.s.failed <= 1, b);
        }
    }
    {   // a scene at the edge whose mean fits hold 1 flaps between holds 1 and 2 about once per FSTEP_HOLD_MAX; this pins that rate
        sim m = {0}; uint32_t r = 1; run(&m, 10, 1.6, 1, 3, 0);
        double t0 = m.real;
        for (int i = 0; i < 36000; i++) { r = r * 1664525u + 1013904223u; run(&m, 1, (r >> 8) % 4 ? 0.7 : 1.3, 1, 3, 0); }
        double cap = (m.real - t0) / FSTEP_HOLD_MAX;
        snprintf(b, sizeof b, "  edge scene with a heavy tail: steps down at most once per 8 s (downs %llu, at most %.0f, failed %llu)", (unsigned long long)m.s.downs, cap, (unsigned long long)m.s.failed);
        check(m.s.downs <= cap, b);
    }
    {   // the GPU equally slow at both holds: failed probes give the ratio 1, so no probe after the second while the scene costs the same
        sim m = {0}; nvn_fgpu g = {0}; uint64_t p1 = 0;
        for (int i = 0; i < 9000; i++) {
            double w = i < 600 ? 0.86 : i < 603 ? 1.25 : i < 6000 ? 1.5 : 0.95;
            m.slow = nvn_fstep_gpu_slowed(&g, w, &m.s, 1);
            run(&m, 1, w, 1, 3, 0);
            if (i == 5999) p1 = m.s.probes;
        }
        snprintf(b, sizeof b, "GPU slow at hold 1 too: two probes fail, none after them (probes %llu, failed %llu, ratio %.2f)", (unsigned long long)p1, (unsigned long long)m.s.failed, g.ratio_at[1][2]);
        check(p1 == FSTEP_GPU_FAILS && m.s.failed == FSTEP_GPU_FAILS && g.ratio_at[1][2] == 1, b);
        snprintf(b, sizeof b, "  a lighter scene that fits hold 1 by the ratio: its probe holds (k %d, probes %llu, failed %llu)", m.s.k, (unsigned long long)m.s.probes, (unsigned long long)m.s.failed);
        check(m.s.k == 1 && m.s.probes == FSTEP_GPU_FAILS + 1 && m.s.failed == FSTEP_GPU_FAILS, b);
        snprintf(b, sizeof b, "  game time %.0f against real time %.2f (debt %.2f)", m.game, m.real, m.s.debt);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01, b);
    }
    {   // two failed probes give a ratio under 1, so the estimate at hold 1 is over it and no probe follows (with ratio 1 the probes would go on failing)
        sim m = {0}; nvn_fgpu g = {0}; uint64_t p1 = 0, f1 = 0; uint32_t r = 7;
        for (int i = 0; i < 9600; i++) {
            int k = m.s.k ? m.s.k : 1; r = r * 1664525u + 1013904223u; double j = ((r >> 8) % 1000) / 1000.0 * 0.06 - 0.03;
            double w = i < 600 ? 0.25 : i < 603 ? 3.0 : i < 6600 ? (k == 1 ? 1.12 : 1.00) + j : (k == 1 ? 0.94 : 0.90) + j / 3;
            m.slow = nvn_fstep_gpu_slowed(&g, w, &m.s, 1);
            run(&m, 1, w, 1, 3, 0);
            if (i == 6599) { p1 = m.s.probes; f1 = m.s.failed; }
        }
        snprintf(b, sizeof b, "GPU a little slower at hold 1 (OoT, M4 Pro): %llu probes, %llu failed, then none (ratio %.2f)", (unsigned long long)p1, (unsigned long long)f1, g.ratio_at[1][2]);
        check(p1 == FSTEP_GPU_FAILS && f1 == FSTEP_GPU_FAILS && g.ratio_at[1][2] < 1, b);
        snprintf(b, sizeof b, "  a lighter scene after it: a trial back to hold 1 that holds (k %d, probes %llu, failed %llu)", m.s.k, (unsigned long long)m.s.probes, (unsigned long long)m.s.failed);
        check(m.s.k == 1 && m.s.probes == p1 && m.s.failed == f1, b);
    }
    {   // probes failed by a stall give no ratio
        sim m = {0}; nvn_fgpu g = {0}; int stalls = 0, at1 = 0, post = 0;
        for (int i = 0; i < 4000; i++) {
            int k = m.s.k ? m.s.k : 1; at1 = k == 1 ? at1 + 1 : 0;
            post = m.s.probe == 1 && k == 1 && m.s.grace == 0 ? post + 1 : 0;
            double w = i < 600 ? 0.25 : i < 603 ? 3.0 : k > 1 ? 1.34 : stalls < 2 && m.s.probe == 1 && post >= 1 && post <= 10 ? 5.0 : at1 <= 20 ? 1.34 : 0.84;
            uint64_t f = m.s.failed;
            m.slow = nvn_fstep_gpu_slowed(&g, w, &m.s, 1);
            run(&m, 1, w, 1, 3, 0);
            stalls += m.s.failed != f;
        }
        snprintf(b, sizeof b, "two probes failed by a stall give no ratio, the next probe holds (k %d, probes %llu, failed %llu, ratios %d)", m.s.k, (unsigned long long)m.s.probes, (unsigned long long)m.s.failed, g.ratio_n[1][2] + g.ratio_n[1][3]);
        check(m.s.k == 1 && m.s.failed == 2 && m.s.probes == 3 && g.ratio_n[1][2] + g.ratio_n[1][3] == 0, b);
    }
    // GPU clock following the hold (dvfs): w is the GPU time at hold 1's clock, drop times as long at a longer hold, and the first 20 frames back at hold 1 still drop
    for (int c = 0; c < 4; c++)
        for (int d = 0; d < 3; d++) {
            static const double drops[] = {1.6, 2.0, 3.0};
            sim m = {0}; nvn_fgpu g = {0}; int ramp = 0, k0 = 1, at1 = 0, n = c == 3 ? 12000 : 7800, from = c == 3 ? 6000 : 600; uint64_t p0 = 0, f0 = 0;
            for (int i = 0; i < n; i++) {
                int k = m.s.k ? m.s.k : 1;
                if (k == 1 && k0 > 1) ramp = 20;
                k0 = k;
                double w = c == 2 ? (i < 1800 ? 1.25 : 0.6) : i < 600 ? 0.25 : c == 1 ? 1.12 : c == 3 && i >= 6000 ? 0.6 : i % 400 >= 340 ? 2.05 : 1.25;
                if (c == 2 && i == 0) from = 1800;
                double gt = k > 1 || ramp-- > 0 ? w * drops[d] : w;
                if (i == from) { p0 = m.s.probes; f0 = m.s.failed; }
                at1 += i >= from && k == 1;
                m.slow = nvn_fstep_gpu_slowed(&g, gt, &m.s, 1);
                run(&m, 1, gt, 1, 3, 0);
            }
            unsigned long long pr = m.s.probes - p0, fl = m.s.failed - f0;
            if (c < 2) {
                snprintf(b, sizeof b, "%s, GPU %.1fx at a longer hold: %llu probes, all failed (frames at hold 1: %d)", c ? "  Hyrule Field 1.12 (OoT, M2 Pro)" : "Kokiri Forest 1.25 / 2.05 (OoT)", drops[d], pr, at1);
                check(pr <= FSTEP_GPU_FAILS + 1 && fl == pr && at1 <= 120, b);   // two failed probes give a hold its ratio
            } else {
                snprintf(b, sizeof b, "  %s, GPU %.1fx: back to hold 1 (k %d, frames at hold 1 %d of %d, probes %llu, failed %llu)", c == 2 ? "1.25 from the start, then 0.6" : "Kokiri Forest, then 0.6", drops[d], m.s.k, at1, n - from, pr, fl);
                check(m.s.k == 1 && at1 >= n - from - 600 && fl == 0, b);
            }
        }
    {
        // the probe starts before 30 fitting frames in a row
        sim m = {0};
        for (int i = 0; i < 1200; i++) run(&m, 1, i % 2 ? 0.92 : 0.80, 1, 3, 0);
        hitch(&m);
        int stepped = m.s.k == 2; double ref = m.s.cref;
        for (int i = 0; i < 600; i++) run(&m, 1, i % 2 ? 0.92 : 0.80, 1, 3, 0);
        snprintf(b, sizeof b, "hitch after 20 s at hold 1, same cost at hold 2 (BotW): one probe back that holds (k %d, probes %llu, failed %llu)", m.s.k, (unsigned long long)m.s.probes, (unsigned long long)m.s.failed);
        check(stepped && ref > 0.84 && ref < 0.88 && m.s.k == 1 && m.s.probes == 1 && m.s.failed == 0 && m.s.probe == 2 && m.s.downs == 1, b);
        snprintf(b, sizeof b, "  game time %.0f against real time %.2f (debt %.2f, dropped %.0f)", m.game, m.real, m.s.debt, m.s.dropped);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.dropped == 0, b);
    }
    {
        sim m = {0};
        for (int i = 0; i < 1200; i++) run(&m, 1, i % 2 ? 0.92 : 0.80, 1, 3, 0);
        for (int i = 0; i < 1200; i++) run(&m, 1, i % 2 ? 1.25 : 1.15, 1, 3, 0);
        check(m.s.k == 2 && m.s.probes == 0 && m.s.downs == 0, "heavier scene, cost 1.4 times what held hold 1: hold 2, no probe");
        for (int i = 0; i < 600; i++) run(&m, 1, i % 2 ? 0.92 : 0.80, 1, 3, 0);
        check(m.s.k == 1 && m.s.probes == 1 && m.s.failed == 0, "  the scene gets lighter again, back to the cost that held: one probe back that holds");
    }
    {   // a hold 1 run of only 5 s proves nothing: a device that steps up every few seconds keeps 30 rather than flap
        sim m = {0};
        for (int i = 0; i < 300; i++) run(&m, 1, i % 2 ? 1.02 : 0.92, 1, 3, 0);
        hitch(&m);
        for (int i = 0; i < 2000; i++) run(&m, 1, i % 2 ? 1.02 : 0.92, 1, 3, 0);
        check(m.s.k == 2 && m.s.probes == 0 && m.s.cref == 0, "hitch after 5 s at hold 1: no cost that held, no probe");
    }
    {
        sim m = {0}; double last = 0, gaps[5]; int ng = 0; uint64_t probes = 0;
        run(&m, 1200, 0.97, 1, 3, 0); hitch(&m);
        for (int i = 0; i < 6000 && ng < 5; i++) {
            run(&m, 1, m.s.k == 1 ? 1.3 : 0.97, 1, 3, 0);
            if (m.s.probes != probes) { probes = m.s.probes; gaps[ng++] = m.real - last; last = m.real; }
        }
        snprintf(b, sizeof b, "same cost at hold 2 but hold 1 does not fit: probes fail (%llu of %llu), cost that held kept at %.2f", (unsigned long long)m.s.failed, (unsigned long long)m.s.probes, m.s.cref);
        check(ng == 5 && m.s.failed >= 4 && m.s.cref > 0.96 && m.s.cref < 0.98, b);
        snprintf(b, sizeof b, "  probes %.0f, %.0f, %.0f, %.0f vsyncs apart; game time %.0f against real time %.2f (debt %.2f)", gaps[1], gaps[2], gaps[3], gaps[4], m.game, m.real, m.s.debt);
        check(gaps[1] >= 120 && gaps[2] >= 240 && gaps[3] >= 480 && gaps[4] >= 480 && gaps[4] < 600 && m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01, b);
    }
    for (int sustained = 0; sustained < 2; sustained++) {
        // only a run of at least 8 s proves that this cost held 60
        sim m = {0}; run(&m, sustained ? 1200 : 300, 0.97, 1, 3, 0);
        run(&m, 600, 0.5, 2, 2, 0);
        check(sustained ? m.s.cref_at[1] > 0.96 && m.s.cref_at[1] < 0.98 : m.s.cref_at[1] == 0,
              sustained ? "first movie after 20 s at 60: save the sustained run without an earlier hitch"
                        : "first movie after only 5 s at 60: no proven cost to save");
        run(&m, 60, 0.97, 1, 3, 0); uint64_t probes = m.s.probes; hitch(&m);
        for (int i = 0; i < 1200; i++) run(&m, 1, m.s.k == 1 ? 0.97 : 1.0, 1, 3, 0);
        check(sustained ? m.s.k == 1 && m.s.probes > probes : m.s.k == 2 && m.s.probes == probes,
              sustained ? "  hitch 1 s after the first movie: the saved run brings 60 back"
                        : "  hitch after a short unproven run: keep 30 without a same-cost probe");
    }
    {
        sim m = {0}; run(&m, 1200, 0.86, 1, 3, 0); run(&m, 120, 0.5, 2, 2, 0);
        run(&m, 120, 0.6, 1, 3, 0); run(&m, 120, 0.5, 2, 2, 0);
        check(m.s.cref_at[1] > 0.85 && m.s.cref_at[1] < 0.87, "second movie after only 2 s: keep the earlier proven cost");
        run(&m, 1200, 0.6, 1, 3, 0); run(&m, 120, 0.5, 2, 2, 0);
        check(m.s.cref_at[1] > 0.59 && m.s.cref_at[1] < 0.61, "  another sustained run before a movie: update the proven cost");
    }
    {   sim m = {0}; run(&m, 1200, 0.86, 1, 3, 0); hitch(&m); double held = m.s.cref; run(&m, 1, 0.5, 2, 3, 0);
        check(held > 0.85 && m.s.cref == 0 && m.s.run <= 2, "movie flag (kmin 2): the cost that held kmin 1 is set aside");
        run(&m, 120, 0.5, 2, 3, 0); run(&m, 1, 0.5, 1, 3, 0);
        check(m.s.cref == held, "  movie ends (kmin 1): the cost that held kmin 1 comes back");
    }
    {   // movies that set a 30 cap (kmin 2)
        sim m = {0}; run(&m, 1200, 0.86, 1, 3, 0); hitch(&m);
        for (int i = 0; i < 1200; i++) run(&m, 1, m.s.k == 1 ? 0.86 : 0.9, 1, 3, 0);
        double held = m.s.cref; int at1 = 0;
        run(&m, 600, 0.5, 2, 2, 0); run(&m, 60, 0.86, 1, 3, 0); hitch(&m);
        for (int i = 0; i < 1200; i++) { run(&m, 1, m.s.k == 1 ? 0.86 : 0.9, 1, 3, 0); at1 += i >= 600 && m.s.k == 1; }
        snprintf(b, sizeof b, "  a hitch 1 s after a movie: the cost that held hold 1 starts a probe (k %d, at hold 1 %d of the last 600)", m.s.k, at1);
        check(held > 0.85 && m.s.k == 1 && at1 == 600, b);
    }
    // thermal cap (thermal_capped): 30 fps from frame 600 for 30 s; the first 10 frames back at hold 1 take 1.5 until the clock rises
    for (int c = 0; c < 3; c++) {
        static const char *const what[] = {"thermal cap ends: the probe's grace covers the clock, hold 1 at once and it stays (k %d, %d steps up after the cap, at hold 1 %d of the last 600)",
                                           "  the clock takes 1 s: the probe fails, the GPU against hold 1's own reference starts another (k %d, %d steps up, at hold 1 %d of the last 600)",
                                           "  hold 1 no longer fits (1.3): a probe at most every 8 s, at hold 1 %d of the last 600; game time %.0f against real time %.2f (probes %llu)"};
        int rise = 600 + 1800, since1 = 0, at1 = 0, n = rise + 3600; uint64_t probes = 0, ups = 0; double last = 0, gap = 1e9;
        sim m = {0}; nvn_fgpu g = {0};
        for (int i = 0; i < n; i++) {
            int kmin = i >= 600 && i < rise ? 2 : 1, kmax = kmin == 2 ? 2 : 3, k = m.s.k ? m.s.k : kmin;
            since1 = k == 1 ? since1 + 1 : 0;
            double w = k >= 2 ? 1.5 : i < rise ? 0.86 : c == 2 ? 1.3 : since1 <= 10 || (c == 1 && i < rise + 60) ? 1.5 : 0.86;
            m.slow = nvn_fstep_gpu_slowed(&g, w, &m.s, kmin);
            run(&m, 1, w, kmin, kmax, 0);
            if (i == rise - 1) ups = m.s.ups;
            if (i >= n - 600) at1 += m.s.k == 1;
            if (i > rise + 1200 && m.s.probes != probes) { if (last) gap = gap < m.real - last ? gap : m.real - last; last = m.real; }
            probes = m.s.probes;
        }
        int up = (int)(m.s.ups - ups);
        if (c < 2) { snprintf(b, sizeof b, what[c], m.s.k, up, at1); check(m.s.k == 1 && at1 == 600 && (c || !up), b); }
        else {
            snprintf(b, sizeof b, what[c], at1, m.game, m.real, (unsigned long long)m.s.probes);
            check(at1 < 100 && gap >= FSTEP_HOLD_MAX && m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01, b);
        }
    }
    {   // 165 Hz with virtual ticks (link_range): 12.1 / 18.2 ms at hold 1, 30.3 / 36.4 at 2, 48.5 / 54.5 at 3
        for (int sl = 1; sl >= 0; sl--) {
            nvn_fstep s = {.k = 3, .step = 3, .kmin = 1, .hold = FSTEP_HOLD0, .shown_slack = sl ? 60.0 / 165 : 0};
            for (int i = 0; i < 360; i++) {
                double ms = s.k == 3 ? (i % 4 ? 48.485 : 54.545) : s.k == 2 ? (i % 2 ? 30.303 : 36.364) : (i % 4 ? 18.182 : 12.121);
                nvn_fstep_next(&s, ms * 60 / 1000, 0.6, 1, 3, 0, 0);
            }
            snprintf(b, sizeof b, sl ? "165 Hz, cheap frames at hold 3, one refresh of slack: trials back to 2 and 1 (k %d, downs %llu)"
                                     : "  without the slack the 54.5 ms frames count as late: hold 3 (20 fps) for good (k %d, downs %llu)", s.k, (unsigned long long)s.downs);
            check(sl ? s.k == 1 && s.downs == 2 : s.k == 3 && s.downs == 0, b);
        }
        sim m = {0}; m.s.shown_slack = 60.0 / 165; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.32, 1, 3, 0);
        check(m.s.k == 2, "  the slack does not hide work: 22 ms frames still step to hold 2");
    }
    {   // a clock that rises slowly: probe frames start at 1.5 vsyncs and get 0.6% cheaper each frame, fitting after about 52
        // the ramp rule (nvn_fstep.ramp) renews the grace while the frames keep getting cheaper
        for (int ramp = 0; ramp <= 1; ramp++) {
            sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0); m.slow = 1; m.s.ramp = ramp;
            int at1 = 0, first_len = -1;
            for (int i = 0; i < 400; i++) {
                double w = m.s.k == 1 ? fmax(0.6, 1.5 * pow(0.994, at1)) : 1.2;
                run(&m, 1, w, 1, 3, 0);
                if (m.s.k == 1) at1++; else if (at1 && first_len < 0) first_len = at1;
            }
            if (!ramp) {
                snprintf(b, sizeof b, "slowed, the clock rises slowly: without the ramp rule the probe fails after grace + 3 frames (%d frames at hold 1, failed %llu)", first_len, (unsigned long long)m.s.failed);
                check(first_len == FSTEP_GRACE + 3 && m.s.failed >= 1, b);
            } else {
                snprintf(b, sizeof b, "  with it the grace is renewed twice and the probe holds (k %d, probes %llu, failed %llu, grace frames %d)", m.s.k, (unsigned long long)m.s.probes, (unsigned long long)m.s.failed, m.s.ptotal);
                check(m.s.k == 1 && m.s.probes == 1 && m.s.failed == 0 && m.s.probe == 2 && m.s.trial == 0 && m.s.ptotal == 3 * FSTEP_GRACE && first_len < 0, b);
                snprintf(b, sizeof b, "  its late frames are paid back: game time %.0f against real time %.2f (debt %.2f)", m.game, m.real, m.s.debt);
                check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.dropped == 0, b);
            }
        }
    }
    {   // costs that do not fall, stop short of the hold, or never reach it: the probe lasts grace + 3, two windows + 3, FSTEP_PROBE_MAX + 3 frames
        static const char *const what[] = {"ramp rule, the cost does not fall: the probe lasts its grace + 3 late frames as before (%d frames at hold 1)",
                                           "  the cost falls, then holds at 1.3: one more window, then the probe fails (%d frames at hold 1)",
                                           "  the cost keeps falling without reaching the hold: the grace ends at FSTEP_PROBE_MAX (%d frames at hold 1, failed %llu)"};
        static const int want[] = {FSTEP_GRACE + 3, 2 * FSTEP_GRACE + 3, FSTEP_PROBE_MAX + 3};
        for (int c = 0; c < 3; c++) {
            sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0); m.slow = 1; m.s.ramp = 1;
            int at1 = 0, first_len = -1;
            for (int i = 0; i < 600 && first_len < 0; i++) {
                double w = m.s.k != 1 ? 1.2 : c == 0 ? 1.5 : c == 1 ? fmax(1.3, 1.6 * pow(0.99, at1)) : 3.0 * pow(0.995, at1);
                run(&m, 1, w, 1, 3, 0);
                if (m.s.k == 1) at1++; else if (at1 && first_len < 0) first_len = at1;
            }
            snprintf(b, sizeof b, what[c], first_len, (unsigned long long)m.s.failed);
            check(first_len == want[c] && m.s.k == 2 && m.s.failed == 1, b);
        }
    }
    {   // the frame time a game steps by (nvn_fdelta_next)
        nvn_fdelta f = {0}; uint64_t t = 1000; float v = 1;
        check(nvn_fdelta_next(&f, t) == 0, "frame time: the first present has none");
        for (int i = 0; i < 20; i++) v = nvn_fdelta_next(&f, t += 16666667);
        check(v == 0, "frame time: 0 at 60 fps, the game's own step");
        for (int i = 0; i < 20; i++) v = nvn_fdelta_next(&f, t += 50000000);
        check(fabsf(v - 0.05f) < 0.0001f, "frame time: 50 ms at 20 fps");
        v = nvn_fdelta_next(&f, t += 2000000000);
        check(v > 0.05f && v < 0.0625f, "frame time: a 2 s stall counts 0.1 s in the mean");
        for (int i = 0; i < 20; i++) v = nvn_fdelta_next(&f, t += 200000000);
        check(fabsf(v - 4.0f / 60) < 0.0001f, "frame time: stops at 4 frames of 60 fps");
    }
    {   // max(step, measured): lost time is made up once (nvn_fstep_measured)
        static const struct { double shown[20]; int n; uint64_t paid_min, paid_max; const char *name; } c[] = {
            {{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2}, 20, 0, 1, "measured step: one late frame in 20 on a 60 Hz screen is made up by the measured time, no whole vsync"},
            {{1.005}, 1, 20, 6000, "measured step: under 1.01 frames nothing is measured and whole vsyncs keep game time"},
            {{1.07}, 1, 0, 1, "measured step: at 56 fps every frame steps 1.07, no whole vsync"},
            {{1, 1, 1, 1.5}, 4, 0, 1, "measured step: a frame in four shown a 120 Hz refresh longer is spread over the frames around it"},
            {{0.97, 1.03}, 2, 0, 0, "measured step: 60 fps with uneven intervals keeps the game's own step"},
            {{1.2}, 1, 0, 1, "measured step: at 50 fps every frame steps 1.2"},
            {{1, 1, 1, 1, 1, 1, 1, 2, 0.3, 0.7}, 10, 0, 6000, "measured step: a late frame the pacing makes up does not run the game ahead"},
        };
        for (unsigned q = 0; q < sizeof c / sizeof *c; q++) {
            nvn_fstep s = {0}; nvn_fdelta f = {0}; uint64_t t = 1000; double real = 0, game = 0;
            nvn_fdelta_next(&f, t);
            for (int i = 0; i < 6000; i++) {
                double shown = c[q].shown[i % c[q].n];
                int n = nvn_fstep_next(&s, shown, 1.0, 1, 1, 0, 0);
                double vs = nvn_fstep_measured(&s, nvn_fdelta_next(&f, t += (uint64_t)(shown * 16666667)) * 60.0);
                n = s.step; game += vs > n ? vs : n;
                if (i) real += shown;
            }
            check(fabs(game - real) < 2.5 && s.paid >= c[q].paid_min && s.paid <= c[q].paid_max, c[q].name);
        }
    }
    {   // frame advance (nvn_fheld_judged): a frame of the game adds the step the present before it handed to its stub's count (state +0x30)
        nvn_fstep s = {0}; nvn_fheld h = {0}; uint64_t stepped = 0; int handed = 0, judged_all = 1, steps1 = 1;
        #define PRESENT(frame, shown, cost) do { if (frame) stepped += (uint64_t)handed; int j_ = nvn_fheld_judged(&h, stepped); judged_all &= j_; \
                                                 handed = j_ ? nvn_fstep_next(&s, shown, cost, 1, 3, 0, 0) : 1; } while (0)
        for (int i = 0; i < 300; i++) PRESENT(1, 2.0, 1.3);
        check(judged_all && s.k == 2, "held presents: a running game's every present is judged (a scene at 30 fps: hold 2)");
        int k = s.k; uint64_t ups = s.ups, downs = s.downs; double debt = s.debt;
        PRESENT(1, 2.0, 1.3);
        for (int t = 0; t < 10; t++) {
            for (int i = 0; i < 30; i++) PRESENT(0, 2.0, 1.0);
            uint64_t was = stepped; PRESENT(1, 2.0, 1.3); steps1 &= stepped - was == 1;
        }
        for (int i = 0; i < 6; i++) { for (int j = 0; j < 3; j++) PRESENT(0, 1.0, 1.0); uint64_t was = stepped; PRESENT(1, 1.0, 1.4); steps1 &= stepped - was == 1; }
        check(steps1, "held presents: each frame let through steps the cap's 1 vsync, not the hold's 2");
        check(s.k == k && s.ups == ups && s.downs == downs && s.debt == debt, "held presents: the frame step keeps its state while the game is held");
        uint64_t was = stepped; PRESENT(1, 2.0, 1.3); int first = (int)(stepped - was);
        was = stepped; for (int i = 0; i < 4; i++) PRESENT(1, 2.0, 1.3);
        check(first == 1 && stepped - was == 8 && s.k == 2, "held presents: run again, the first frame steps 1 and then the hold of 2 goes on");
        #undef PRESENT
    }
    {   // in ms against a 16.7 ms hold
        uint64_t pw = 20; check(nvn_fstep_drain_cut(33, 17, 16, 0, &pw) == 17 && pw == 4, "a drain of one tick: the frame shows its hold, the pacing wait loses the tick");
        pw = 10; check(nvn_fstep_drain_cut(17, 17, 6, 0, &pw) == 17 && pw == 10, "a drain inside the hold is pacing: nothing cut");
        pw = 10; check(nvn_fstep_drain_cut(30, 17, 6, 0, &pw) == 24 && pw == 4, "a late frame keeps its lateness beyond the drain");
        pw = 12; check(nvn_fstep_drain_cut(34, 33, 5, 0, &pw) == 33 && pw == 11, "only the part past the hold is cut (hold 2)");
        pw = 2; check(nvn_fstep_drain_cut(33, 17, 0, 16, &pw) == 17 && pw == 2, "a drain in a later acquire of the previous frame: the time, not the pacing wait");
        pw = 3; check(nvn_fstep_drain_cut(40, 17, 9, 0, &pw) == 37 && pw == 0, "a drain counted no larger than the pacing wait it was part of");
        pw = 0; check(nvn_fstep_drain_cut(0, 17, 0, 5, &pw) == 0, "the first frame (no previous start): nothing to cut");
    }
    {   // idle GPU: 10 late frames do not step up, FSTEP_RUN (30) in a row still do
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0);
        for (int i = 0; i < 10; i++) { m.s.gpu_on = 1; m.s.gpu = 0.02; run(&m, 1, 2.5, 1, 3, 0); }
        check(m.s.k == 1 && m.s.ups == 0, "idle GPU (0.02 vsyncs): 10 late frames on a progress screen keep hold 1");
        for (int i = 0; i < FSTEP_RUN; i++) { m.s.gpu_on = 1; m.s.gpu = 0.02; run(&m, 1, 2.5, 1, 3, 0); }
        check(m.s.k > 1 && m.s.ups == 1 && m.s.up_idle == 1, "idle GPU: FSTEP_RUN late frames in a row still step up (CPU-heavy scene)");
    }
    {   // a game that waits outside acquire shows cost == shown == hold; the probe brings it back to hold 1
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0);
        for (int i = 0; i < 10; i++) { m.s.gpu_on = 1; m.s.gpu = 2.5; run(&m, 1, 2.5, 1, 3, 0); }
        int at3 = m.s.k == 3;
        for (int i = 0; i < 300; i++) {
            double k = m.s.k; double w = k == 1 ? 0.75 : k;
            m.s.gpu_on = 1; m.s.gpu = 0.75; run(&m, 1, w, 1, 3, 0);
        }
        check(at3 && m.s.k == 1 && m.s.probes >= 1 && m.s.failed == 0, "unpaced hold at hold 3 with GPU 0.75: unpaced probe returns to hold 1");
    }
    {   // a CPU-bound game: uhold backs off up to FSTEP_UHOLD_MAX (60 s)
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0);
        for (int i = 0; i < 10; i++) { m.s.gpu_on = 1; m.s.gpu = 0.5; run(&m, 1, 1.8, 1, 3, 0); }
        int at2 = m.s.k == 2;
        for (int i = 0; i < 6000; i++) {
            double w = m.s.k == 1 ? 1.8 : 2.0;
            m.s.gpu_on = 1; m.s.gpu = 0.5; run(&m, 1, w, 1, 3, 0);
        }
        check(at2 && m.s.k == 2 && m.s.failed >= 2 && m.s.uhold > FSTEP_HOLD_MAX, "unpaced probe when CPU really needs hold 2: uhold doubles past 8 s toward 60 s");
    }
    {   // stalls (FSTEP_STALL) and host-named pauses / window changes (settle)
        // cold-GPU / link-recovery frames after a pause (over_sum - omax > 3.0) keep hold 1
        sim m = {0}; run(&m, 600, 0.9, 1, 3, 0); uint64_t late0 = m.s.late;
        run(&m, 1, 114, 1, 3, 0); run(&m, 1, 3.0, 1, 3, 0); run(&m, 1, 2.5, 1, 3, 0); run(&m, 1, 2.0, 1, 3, 0); run(&m, 60, 0.9, 1, 3, 0);
        snprintf(b, sizeof b, "stall: a pause and severe late frames keep hold 1 (k %d, ups %llu, stalls %llu)", m.s.k, (unsigned long long)m.s.ups, (unsigned long long)m.s.stalls);
        check(m.s.k == 1 && m.s.ups == 0 && m.s.stalls == 1 && m.s.late == late0 + 3, b);
        check(m.s.crun < 0.95, "  the stall's time stays out of the cost that held hold 1");
        run(&m, 2, 1.3, 1, 3, 0); run(&m, 1, 10, 1, 3, 0); run(&m, 3, 1.4, 1, 3, 0); run(&m, 1, 17, 1, 3, 0); run(&m, 6, 1.3, 1, 3, 0); run(&m, 120, 0.9, 1, 3, 0);
        check(m.s.k == 1 && m.s.ups == 0 && m.s.stalls == 3, "  a fullscreen switch (two stalls, late frames around them) keeps hold 1");
        sim h = {0}; run(&h, 600, 0.9, 1, 3, 0); run(&h, 1, 114, 1, 3, 0);
        int n = 0; while (h.s.k == 1 && n < 600) { run(&h, 1, 1.32, 1, 3, 0); n++; }
        snprintf(b, sizeof b, "  22 ms frames after a stall step up once its grace is over (%d frames)", n);
        check(h.s.k == 2 && n == FSTEP_GRACE + 3, b);
        sim l = {0}; run(&l, 600, 0.9, 1, 3, 0); run(&l, 3, 5.0, 1, 3, 0);
        check(l.s.k == 3 && l.s.ups == 1 && l.s.stalls == 0, "  three 83 ms frames in a row are not stalls: they step up as before");
        // 120 fps (per_vsync = 2): the same 133 ms threshold
        sim f = {0}; f.s.per_vsync = 2; f.s.no_payback = 1; run(&f, 240, 0.7, 1, 2, 0); run(&f, 3, 10, 1, 2, 0);
        check(f.s.k == 2 && f.s.stalls == 0, "  120 fps: 83 ms frames are not stalls");
        sim g = {0}; g.s.per_vsync = 2; g.s.no_payback = 1; run(&g, 240, 0.7, 1, 2, 0); run(&g, 1, 200, 1, 2, 0); run(&g, 3, 1.5, 1, 2, 0); run(&g, 60, 0.7, 1, 2, 0);
        check(g.s.k == 1 && g.s.ups == 0 && g.s.stalls == 1, "  120 fps: a pause and three late frames keep 120");
        // a host-named pause is not paid back; an unnamed stall pays back FSTEP_DEBT_MAX (15)
        sim p = {0}; run(&p, 600, 0.9, 1, 3, 0); uint64_t paid0 = p.s.paid;
        p.s.settle = 1; run(&p, 1, 114, 1, 3, 0); run(&p, 60, 0.9, 1, 3, 0);
        snprintf(b, sizeof b, "  a host-named pause is not paid back (paid %llu, debt %.2f)", (unsigned long long)(p.s.paid - paid0), p.s.debt);
        check(p.s.paid == paid0 && p.s.debt < 1 && p.s.k == 1 && p.s.settle == 0, b);
        // a named pause on a frame already stepping 2: no double payment
        sim pb = {0}; run(&pb, 60, 0.9, 1, 3, 0); run(&pb, 1, 2.2, 1, 3, 0); uint64_t paid_b = pb.s.paid;
        pb.s.settle = 1; run(&pb, 1, 114, 1, 3, 0); run(&pb, 30, 0.9, 1, 3, 0);
        check(pb.s.paid == paid_b && pb.s.debt < 1, "  named pause on a payback frame drops only the pause, no double-pay");
        sim q = {0}; run(&q, 600, 0.9, 1, 3, 0); paid0 = q.s.paid; run(&q, 1, 114, 1, 3, 0); run(&q, 60, 0.9, 1, 3, 0);
        check(q.s.paid - paid0 == 15, "  an unnamed stall pays back 15 vsyncs as before");
        sim w = {0}; run(&w, 600, 0.9, 1, 3, 0);
        w.s.settle = 1; run(&w, 20, 1.34, 1, 3, 0); w.s.settle = 1; run(&w, 20, 1.34, 1, 3, 0); run(&w, 120, 0.9, 1, 3, 0);
        check(w.s.k == 1 && w.s.ups == 0, "  a host-named window change: 40 mildly late frames keep hold 1");
        sim x = {0}; run(&x, 600, 0.9, 1, 3, 0); run(&x, 20, 1.34, 1, 3, 0);
        check(x.s.k == 2 && x.s.ups == 1, "  the same frames with no window change step up as before");
        // pauses do not inflate run / tsince; a stall during a trial does not let late frames pass it
        sim ti = {0}; run(&ti, 5, 0.9, 1, 3, 0); ti.s.settle = 1; run(&ti, 1, 600, 1, 3, 0);
        check(ti.s.run < 20 && ti.s.cref == 0, "  a 10 s pause does not inflate run into an 8 s proven hold");
        sim tr = {0}; run(&tr, 60, 0.5, 1, 3, 0); run(&tr, 300, 1.3, 1, 3, 0); run(&tr, FSTEP_RUN, 0.6, 1, 3, 0);
        int in_trial = tr.s.k == 1 && tr.s.trial == FSTEP_RUN;
        tr.s.settle = 1; run(&tr, 1, 600, 1, 3, 0); run(&tr, FSTEP_GRACE, 1.5, 1, 3, 0);
        check(in_trial && tr.s.trial == FSTEP_RUN && !tr.s.tpend, "  a stall during a trial pauses trial countdown across stall grace");
        sim ig = {0}; run(&ig, 60, 0.5, 1, 3, 0); ig.s.settle = 1; run(&ig, 1, 114, 1, 3, 0);
        for (int i = 0; i < FSTEP_GRACE; i++) { ig.s.gpu_on = 1; ig.s.gpu = 0.02; run(&ig, 1, 2.5, 1, 3, 0); }
        check(ig.s.k == 1 && ig.s.ilate_run == 0, "  idle-GPU late frames during stall grace do not advance ilate_run");
        // early returns (!s->k, fixed > 0) clear settle
        sim er = {0}; er.s.settle = 1; nvn_fstep_next(&er.s, 0, 0, 1, 3, 0, 0);
        int clr1 = er.s.settle == 0;
        er.s.settle = 1; nvn_fstep_next(&er.s, 1, 0.5, 1, 3, 2, 0);
        int clr2 = er.s.settle == 0;
        nvn_fstep_next(&er.s, 1, 0.5, 1, 3, 0, 0);
        check(clr1 && clr2 && er.s.stalls == 0, "  settle is cleared on first-frame and fixed-step early returns");
    }
    printf("== %d checks, %d failed ==\n", checks, fail);
    return fail != 0;
}
