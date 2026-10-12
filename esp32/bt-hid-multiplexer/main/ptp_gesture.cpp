#include "ptp_gesture.h"
#include <math.h>
#include <string.h>

// Units per millimetre when the descriptor does not say (both touchpads seen so far are 17-20).
static const float DEFAULT_UNITS_PER_MM = 20.0f;

static void reset_gesture(PtpGestureState *s) {
    s->gx = s->gy = 0;
    s->axis = 0;
    s->fired = false;
    s->scroll_mm = 0;
}

// Handles a complete frame: the fingers on the pad and the button.
static void process_frame(PtpGestureState *s, const HidPtpLayout &layout, bool button, uint32_t now_ms,
                          PtpGestureOutput *out) {
    float x_per_mm = layout.x_per_mm > 0 ? layout.x_per_mm : DEFAULT_UNITS_PER_MM;
    float y_per_mm = layout.y_per_mm > 0 ? layout.y_per_mm : DEFAULT_UNITS_PER_MM;

    // The fingers touching: a contact without Tip Switch is lifting, and one without Confidence is a
    // palm or the edge of a hand.
    PtpGestureState::Finger cur[2 * HID_PTP_MAX_CONTACTS];
    uint8_t n = 0;
    for (uint8_t i = 0; i < s->frame_n; i++) {
        const HidPtpContact &c = s->frame[i];
        if (!c.tip || !c.confident) continue;
        cur[n++] = {c.id, (float)c.x / x_per_mm, (float)c.y / y_per_mm};
    }

    // Movement of the fingers that were already down, averaged. Fingers that just came down or went
    // up do not count, so that the count changing does not make the pointer jump.
    float dx = 0, dy = 0;
    uint8_t matched = 0;
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t j = 0; j < s->prev_n; j++) {
            if (s->prev[j].id == cur[i].id) {
                dx += cur[i].x - s->prev[j].x;
                dy += cur[i].y - s->prev[j].y;
                matched++;
                break;
            }
        }
    }
    if (matched > 0) {
        dx /= matched;
        dy /= matched;
    }

    if (n > 0 && !s->session) {
        s->session = true;
        s->session_start_ms = now_ms;
        s->session_max_fingers = 0;
        s->session_travel_mm = 0;
        s->session_pressed = false;
    }
    if (s->session) {
        if (n > s->session_max_fingers) s->session_max_fingers = n;
        s->session_travel_mm += fabsf(dx) + fabsf(dy);
    }

    // Pressing the pad down: the right button with two or more fingers on it, as on a laptop.
    if (button && !s->button_down) {
        s->button_down = true;
        s->button_mask = (n >= 2) ? 2 : 1;
        s->session_pressed = true;
    } else if (!button && s->button_down) {
        s->button_down = false;
        s->button_mask = 0;
    }

    if (n != s->prev_n) reset_gesture(s);

    if (matched > 0) {
        if (n == 1) {
            uint32_t dt = now_ms - s->prev_ms;
            if (dt < 4) dt = 4;
            if (dt > 50) dt = 50;
            float speed = sqrtf(dx * dx + dy * dy) * 1000.0f / (float)dt;  // mm/s
            float accel = 1.0f + fminf(speed / 150.0f, 1.5f);
            s->rem_x += dx * PTP_POINTER_COUNTS_PER_MM * accel;
            s->rem_y += dy * PTP_POINTER_COUNTS_PER_MM * accel;
            float ix = truncf(s->rem_x), iy = truncf(s->rem_y);
            ix = fmaxf(fminf(ix, 32767), -32767);
            iy = fmaxf(fminf(iy, 32767), -32767);
            out->dx = (int16_t)ix;
            out->dy = (int16_t)iy;
            s->rem_x -= ix;
            s->rem_y -= iy;
        } else if (n == 2) {
            s->gx += dx;
            s->gy += dy;
            if (s->axis == 0) {
                if (fabsf(s->gy) >= PTP_AXIS_LOCK_MM && fabsf(s->gy) >= fabsf(s->gx)) {
                    s->axis = 1;
                    s->scroll_mm = s->gy;
                } else if (fabsf(s->gx) >= PTP_AXIS_LOCK_MM && fabsf(s->gx) > fabsf(s->gy)) {
                    s->axis = 2;
                }
            } else if (s->axis == 1) {
                s->scroll_mm += dy;
            }
            if (s->axis == 1) {
                while (s->scroll_mm <= -PTP_SCROLL_MM_PER_NOTCH) {
                    out->gestures[PTP_GESTURE_2F_UP]++;
                    s->scroll_mm += PTP_SCROLL_MM_PER_NOTCH;
                }
                while (s->scroll_mm >= PTP_SCROLL_MM_PER_NOTCH) {
                    out->gestures[PTP_GESTURE_2F_DOWN]++;
                    s->scroll_mm -= PTP_SCROLL_MM_PER_NOTCH;
                }
            } else if (s->axis == 2 && !s->fired && fabsf(s->gx) >= PTP_SWIPE_MM) {
                out->gestures[s->gx < 0 ? PTP_GESTURE_2F_LEFT : PTP_GESTURE_2F_RIGHT]++;
                s->fired = true;
            }
        } else if (n >= 3) {
            s->gx += dx;
            s->gy += dy;
            if (!s->fired && fmaxf(fabsf(s->gx), fabsf(s->gy)) >= PTP_SWIPE_MM) {
                if (fabsf(s->gy) >= fabsf(s->gx)) {
                    out->gestures[s->gy < 0 ? PTP_GESTURE_3F_UP : PTP_GESTURE_3F_DOWN]++;
                } else {
                    out->gestures[s->gx < 0 ? PTP_GESTURE_3F_LEFT : PTP_GESTURE_3F_RIGHT]++;
                }
                s->fired = true;
            }
        }
    }

    // All fingers up: a short touch that hardly moved is a tap.
    if (n == 0 && s->session) {
        if (!s->session_pressed && now_ms - s->session_start_ms <= PTP_TAP_MAX_MS &&
            s->session_travel_mm <= PTP_TAP_MAX_MOVE_MM) {
            if (s->session_max_fingers == 1) out->clicks = 1;
            if (s->session_max_fingers == 2) out->clicks = 2;
        }
        s->session = false;
    }
    if (n == 0) {
        s->rem_x = s->rem_y = 0;
    }

    memcpy(s->prev, cur, n * sizeof(cur[0]));
    s->prev_n = n;
    s->prev_ms = now_ms;
}

void ptp_gesture_report(PtpGestureState *state, const HidPtpLayout &layout, const HidPtpContact *contacts,
                        uint8_t contact_count, bool button, uint32_t now_ms, PtpGestureOutput *out) {
    memset(out, 0, sizeof(*out));

    // In hybrid mode the first report of a frame carries the number of fingers in the whole frame,
    // and the reports that follow carry 0 and the rest of the fingers.
    uint8_t take;
    if (contact_count > 0 || state->frame_expected == 0) {
        state->frame_n = 0;
        state->frame_expected = contact_count;
        if (state->frame_expected > 2 * HID_PTP_MAX_CONTACTS) state->frame_expected = 2 * HID_PTP_MAX_CONTACTS;
        take = state->frame_expected;
    } else {
        take = state->frame_expected - state->frame_n;
    }
    if (take > layout.contact_slots) take = layout.contact_slots;
    for (uint8_t i = 0; i < take; i++) state->frame[state->frame_n++] = contacts[i];

    if (state->frame_n >= state->frame_expected) {
        process_frame(state, layout, button, now_ms, out);
        state->frame_n = 0;
        state->frame_expected = 0;
    }
    out->buttons = state->button_mask;
}
