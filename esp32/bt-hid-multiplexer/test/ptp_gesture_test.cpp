// Host-side test of main/ptp_gesture.cpp: precision touchpad fingers becoming pointer movement, tap
// clicks, the pad button, two-finger scrolling and swipes, and three-finger swipes.
#include <stdio.h>
#include <string.h>
#include <initializer_list>
#include "ptp_gesture.h"

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

// 20 units per mm on both axes, 4 fingers per report, as the ProtoArc T1 Plus.
static HidPtpLayout layout() {
    HidPtpLayout l = {};
    l.contact_slots = 4;
    l.x_per_mm = 20;
    l.y_per_mm = 20;
    return l;
}

struct F {
    uint8_t id;
    float x_mm, y_mm;
    bool tip;
    bool confident;
};

struct Pad {
    PtpGestureState st = {};
    HidPtpLayout l = layout();
    uint32_t now = 1000;
    PtpGestureOutput total = {};  // everything since the last clear()

    // Sends one frame with the given fingers, `ms` after the previous one.
    PtpGestureOutput frame(std::initializer_list<F> fingers, bool button = false, uint32_t ms = 10) {
        HidPtpContact c[HID_PTP_MAX_CONTACTS] = {};
        uint8_t n = 0;
        for (const F &f : fingers) {
            c[n++] = {f.id, f.tip, f.confident, (int32_t)(f.x_mm * 20), (int32_t)(f.y_mm * 20)};
        }
        now += ms;
        PtpGestureOutput o;
        ptp_gesture_report(&st, l, c, n, button, now, &o);
        add(o);
        return o;
    }

    // All fingers up, as the devices report it: the last positions without Tip Switch.
    PtpGestureOutput lift(uint8_t id0 = 0, uint32_t ms = 10) {
        return frame({{id0, 0, 0, false, true}}, false, ms);
    }

    void add(const PtpGestureOutput &o) {
        total.dx += o.dx;
        total.dy += o.dy;
        total.clicks |= o.clicks;
        for (int g = 0; g < PTP_GESTURE_COUNT; g++) total.gestures[g] += o.gestures[g];
    }
    void clear() { memset(&total, 0, sizeof(total)); }
    int gestures() const {
        int sum = 0;
        for (int g = 0; g < PTP_GESTURE_COUNT; g++) sum += total.gestures[g];
        return sum;
    }
};

static F t(uint8_t id, float x, float y) { return {id, x, y, true, true}; }

static void test_pointer() {
    Pad p;
    p.frame({t(0, 50, 50)});
    for (int i = 1; i <= 10; i++) p.frame({t(0, 50 + i, 50 - i * 0.5f)});  // 1 mm right, 0.5 mm up per 10 ms
    CHECK(p.total.dx > 60 && p.total.dx < 150);
    CHECK(p.total.dy < 0 && p.total.dy > -75);
    CHECK(p.gestures() == 0 && p.total.clicks == 0);  // long and moving: no tap
    p.lift();

    // Slow movement gets less acceleration than fast movement over the same distance.
    Pad slow, fast;
    slow.frame({t(0, 50, 50)});
    fast.frame({t(0, 50, 50)});
    for (int i = 1; i <= 20; i++) slow.frame({t(0, 50 + i * 0.5f, 50)}, false, 20);
    for (int i = 1; i <= 2; i++) fast.frame({t(0, 50 + i * 5.0f, 50)}, false, 10);
    CHECK(fast.total.dx > slow.total.dx);
}

static void test_taps() {
    Pad p;
    p.frame({t(0, 50, 50)});
    p.frame({t(0, 50.2f, 50)});
    CHECK(p.lift().clicks == 1);

    // Two fingers: right click.
    p.frame({t(0, 50, 50), t(1, 70, 50)});
    p.frame({t(0, 50, 50), t(1, 70, 50)});
    CHECK(p.lift().clicks == 2);

    // Too long, or moved too far: no click.
    p.clear();
    p.frame({t(0, 50, 50)});
    p.frame({t(0, 50, 50)}, false, 300);
    p.lift();
    p.frame({t(0, 50, 50)});
    p.frame({t(0, 55, 50)});
    p.lift();
    CHECK(p.total.clicks == 0);

    // A palm (no Confidence) is no finger.
    p.frame({{0, 50, 50, true, false}});
    CHECK(p.lift().clicks == 0);
}

static void test_button() {
    Pad p;
    CHECK(p.frame({t(0, 50, 50)}, true).buttons == 1);
    CHECK(p.frame({t(0, 50, 50)}, true).buttons == 1);
    CHECK(p.frame({t(0, 50, 50)}, false).buttons == 0);
    CHECK(p.lift().clicks == 0);  // the press was the click, not a tap

    // With two fingers on the pad, the press is the right button, until it is released.
    CHECK(p.frame({t(0, 50, 50), t(1, 70, 50)}, true).buttons == 2);
    CHECK(p.frame({t(0, 50, 50)}, true).buttons == 2);
    CHECK(p.frame({t(0, 50, 50)}, false).buttons == 0);
    p.lift();
}

static void test_scroll() {
    Pad p;
    // Two fingers moving up 15 mm: 5 notches of 3 mm, but the first 2 mm only decide the direction
    // (and count towards the first notch).
    p.frame({t(0, 50, 60), t(1, 70, 60)});
    for (int i = 1; i <= 15; i++) p.frame({t(0, 50, 60.0f - i), t(1, 70, 60.0f - i)});
    CHECK(p.total.gestures[PTP_GESTURE_2F_UP] == 5);
    CHECK(p.gestures() == 5);
    CHECK(p.total.dx == 0 && p.total.dy == 0);  // no pointer movement
    // Back down 6 mm in the same gesture.
    p.clear();
    for (int i = 1; i <= 6; i++) p.frame({t(0, 50, 45.0f + i), t(1, 70, 45.0f + i)});
    CHECK(p.total.gestures[PTP_GESTURE_2F_DOWN] == 2 && p.gestures() == 2);
    CHECK(p.lift().clicks == 0);

    // Sideways wobble while scrolling does not swipe.
    p.clear();
    p.frame({t(0, 50, 60), t(1, 70, 60)});
    for (int i = 1; i <= 15; i++) p.frame({t(0, 50.0f + i, 60.0f - i * 1.2f), t(1, 70.0f + i, 60.0f - i * 1.2f)});
    CHECK(p.total.gestures[PTP_GESTURE_2F_RIGHT] == 0 && p.total.gestures[PTP_GESTURE_2F_UP] == 6);
    p.lift();
}

static void test_swipes() {
    // Two fingers left: one swipe, however far they go.
    Pad p;
    p.frame({t(0, 80, 50), t(1, 80, 70)});
    for (int i = 1; i <= 40; i++) p.frame({t(0, 80.0f - i, 50), t(1, 80.0f - i, 70)});
    CHECK(p.total.gestures[PTP_GESTURE_2F_LEFT] == 1 && p.gestures() == 1);
    p.lift();
    // Right: one swipe, but only once it has gone PTP_SWIPE_MM.
    p.clear();
    p.frame({t(0, 50, 50), t(1, 50, 70)});
    for (int i = 1; i <= 10; i++) p.frame({t(0, 50.0f + i, 50), t(1, 50.0f + i, 70)});
    CHECK(p.gestures() == 0);
    for (int i = 11; i <= 14; i++) p.frame({t(0, 50.0f + i, 50), t(1, 50.0f + i, 70)});
    CHECK(p.total.gestures[PTP_GESTURE_2F_RIGHT] == 1 && p.gestures() == 1);
    p.lift();

    // Three fingers, each direction once per swipe.
    struct Dir { float dx, dy; int gesture; };
    const Dir dirs[] = {{0, -1, PTP_GESTURE_3F_UP}, {0, 1, PTP_GESTURE_3F_DOWN},
                        {-1, 0, PTP_GESTURE_3F_LEFT}, {1, 0, PTP_GESTURE_3F_RIGHT}};
    for (const Dir &d : dirs) {
        p.clear();
        p.frame({t(0, 50, 50), t(1, 60, 50), t(2, 70, 50)});
        for (int i = 1; i <= 30; i++) {
            float ox = d.dx * i, oy = d.dy * i;
            p.frame({t(0, 50 + ox, 50 + oy), t(1, 60 + ox, 50 + oy), t(2, 70 + ox, 50 + oy)});
        }
        CHECK(p.total.gestures[d.gesture] == 1 && p.gestures() == 1);
        CHECK(p.total.dx == 0 && p.total.dy == 0);
        CHECK(p.lift().clicks == 0);
    }

    // Lifting to two fingers after a three-finger swipe starts over: no two-finger swipe from the same movement.
    p.clear();
    p.frame({t(0, 50, 50), t(1, 60, 50), t(2, 70, 50)});
    for (int i = 1; i <= 15; i++) p.frame({t(0, 50.0f - i, 50), t(1, 60.0f - i, 50), t(2, 70.0f - i, 50)});
    p.frame({t(0, 34, 50), t(1, 44, 50)});
    p.frame({t(0, 33, 50), t(1, 43, 50)});
    CHECK(p.total.gestures[PTP_GESTURE_3F_LEFT] == 1 && p.gestures() == 1);
    p.lift();
}

// A finger coming down or going up does not move the pointer.
static void test_no_jumps() {
    Pad p;
    p.frame({t(0, 50, 50)});
    p.frame({t(0, 50, 50), t(1, 90, 90)});
    p.frame({t(1, 90, 90)});
    p.frame({t(1, 90, 90)});
    CHECK(p.total.dx == 0 && p.total.dy == 0);
    p.lift(1);
}

// Hybrid mode: a frame of 6 fingers in two reports (count 6, then 0); the frame is handled once the
// second report arrives.
static void test_hybrid() {
    PtpGestureState st = {};
    HidPtpLayout l = layout();
    HidPtpContact a[4], b[4] = {};
    for (uint8_t i = 0; i < 4; i++) a[i] = {i, true, true, 1000 + i * 100, 1000};
    for (uint8_t i = 0; i < 2; i++) b[i] = {(uint8_t)(4 + i), true, true, 1600 + i * 100, 1000};
    PtpGestureOutput o;
    ptp_gesture_report(&st, l, a, 6, false, 100, &o);
    CHECK(st.prev_n == 0);  // not handled yet
    ptp_gesture_report(&st, l, b, 0, false, 110, &o);
    CHECK(st.prev_n == 6);
}

int main() {
    test_pointer();
    test_taps();
    test_button();
    test_scroll();
    test_swipes();
    test_no_jumps();
    test_hybrid();
    if (g_failures) {
        printf("ptp_gesture_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("All PTP gesture tests passed\n");
    return 0;
}
