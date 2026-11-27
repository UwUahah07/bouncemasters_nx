/* ---------------------------------------------------------------------------
 * bm_input.c -- expose the Switch touchscreen to UnityEngine.Input.
 *
 * Bouncemasters is a touchscreen title. Every interaction is a tap or a drag on
 * a button the game draws itself, so there is nothing to map buttons onto --
 * what it needs is for Input.touchCount / Input.GetTouch / Input.mousePosition
 * to describe the Switch panel.
 *
 * THE PRODUCER ALREADY EXISTS
 * ---------------------------
 * android_native_feed_hid() merges the touchscreen, the stick-driven cursor,
 * gyro aiming and a USB mouse into one stream in render space with a
 * bottom-left origin, and bm_touch.c turns that into per-finger DOWN / MOVE / UP
 * phases. This file is only the consumer: it answers managed queries from
 * bm_touch_poll(). No new input plumbing, no second source of truth.
 *
 * WHY HOOK Input RATHER THAN INJECT MOTIONEVENTS
 * ----------------------------------------------
 * Injecting through android::NewInput::ProcessTouchEvent would be the other
 * route, and killerbean_nx takes it. It needs a per-build native RVA and a
 * correctly shaped MotionEvent. Hooking the managed getters needs neither: the
 * addresses come from the same dump.cs as everything else here, the values are
 * plain scalars, and what the game reads is exactly what we wrote. For a game
 * whose input is entirely touch, the managed layer is where the truth needs to
 * be anyway.
 *
 * BOTH TOUCH AND MOUSE ARE ANSWERED
 * ---------------------------------
 * Unity UI (uGUI) drives its StandaloneInputModule from mousePosition +
 * GetMouseButton when touchSupported is false, and from touch when it is true.
 * Which one a given build uses depends on the EventSystem configuration, which
 * we cannot see from here -- so both are answered consistently from the same
 * finger. A tap is simultaneously "touch 0 in phase Began" and "mouse button 0
 * went down at that position", which is what a real Android device reports too.
 *
 * COORDINATES
 * -----------
 * bm_touch already delivers bottom-left origin in render pixels, which is
 * exactly Unity's screen space. No flip here; if the Y axis ever looks
 * inverted, the bug is in android_native_feed_hid's flip, not in this file.
 *
 * MIT.
 * ------------------------------------------------------------------------- */

#include <stdint.h>
#include <switch.h>
#include <string.h>

#include "bm_managed.h"
#include "bm_touch.h"

uint32_t port_frame_count(void);
#include "config.h"
#include "diag.h"
#include "so_util.h"
#include "util.h"

/* UnityEngine.Vector3 / Vector2 -- plain float triples/pairs. */
typedef struct { float x, y, z; } V3;
typedef struct { float x, y; }    V2;

/* UnityEngine.Touch, field offsets straight from dump.cs:
 *   0x00 int      m_FingerId
 *   0x04 Vector2  m_Position
 *   0x0C Vector2  m_RawPosition
 *   0x14 Vector2  m_PositionDelta
 *   0x1C float    m_TimeDelta
 *   0x20 int      m_TapCount
 *   0x24 TouchPhase m_Phase
 *   0x28 TouchType  m_Type
 *   0x2C float    m_Pressure
 *   0x30 float    m_maximumPossiblePressure
 *   0x34 float    m_Radius
 *   0x38 float    m_RadiusVariance
 *   0x3C float    m_AltitudeAngle
 *   0x40 float    m_AzimuthAngle
 * Total 0x44. Laid out explicitly rather than trusting struct packing. */
typedef struct {
  int32_t fingerId;
  V2      position;
  V2      rawPosition;
  V2      positionDelta;
  float   timeDelta;
  int32_t tapCount;
  int32_t phase;
  int32_t type;
  float   pressure;
  float   maxPressure;
  float   radius;
  float   radiusVariance;
  float   altitudeAngle;
  float   azimuthAngle;
} UTouch;

/* UnityEngine.TouchPhase */
enum { UTP_BEGAN = 0, UTP_MOVED = 1, UTP_STATIONARY = 2, UTP_ENDED = 3, UTP_CANCELED = 4 };

/* ------------------------------------------------------------ frame state */

static bm_touch g_cur[NX_MAX_TOUCH];
static int      g_n;
static V2       g_prev_pos[NX_MAX_TOUCH];
static int      g_prev_valid[NX_MAX_TOUCH];

/* Last known pointer position. Held across lift so mousePosition does not snap
 * to the origin the instant a finger leaves -- uGUI reads it on the release
 * frame to decide whether the pointer is still over the button it pressed. */
static V2 g_last_pos = { 0.0f, 0.0f };

/* Poll the tracker, but advance per-frame bookkeeping only ONCE per frame.
 *
 * Every hook calls refresh(), and uGUI calls several of them per frame --
 * get_touchCount, GetTouch, get_mousePosition, GetMouseButton* all land here.
 * bm_touch_poll itself is non-destructive so repeated reads are safe, but the
 * delta bookkeeping in bmi_GetTouch_fill is NOT: it overwrites g_prev_pos with
 * the current position, so a second GetTouch in the same frame reports a
 * deltaPosition of zero.
 *
 * Touch.get_deltaPosition has 4 call sites in this game, and a drag whose delta
 * randomly reads zero depending on how many times the module polled is exactly
 * the kind of bug that looks like "input is flaky" rather than "input is
 * wrong". Gate on the frame counter. */
static uint32_t g_last_refresh_frame = 0xffffffffu;
static int      g_frame_is_new;

static void refresh(void) {
  uint32_t f = port_frame_count();
  g_frame_is_new = (f != g_last_refresh_frame);
  g_last_refresh_frame = f;

  g_n = bm_touch_poll(g_cur, NX_MAX_TOUCH);

#if DEBUG_LOG
  /* Prove the path end to end, once each. Silence here was ambiguous for a
   * whole cycle: "the hooks are installed" told us nothing about whether the
   * engine ever calls them, or whether anything ever reaches the tracker. */
  {
    static int said_polled, said_touch;
    if (!said_polled) { said_polled = 1;
      debugPrintf("[input] first Input poll from the engine "
                  "(hooks are live)\n"); }
    if (!said_touch && g_n > 0) { said_touch = 1;
      debugPrintf("[input] first finger: n=%d id=%d at %.0f,%.0f phase=%d\n",
                  g_n, g_cur[0].id, (double)g_cur[0].x, (double)g_cur[0].y,
                  g_cur[0].phase); }
  }
#endif
  for (int i = 0; i < g_n; i++)
    if (g_cur[i].phase != BM_TOUCH_UP) { g_last_pos.x = g_cur[i].x; g_last_pos.y = g_cur[i].y; break; }
}

/* A finger counts as present for touchCount while it is DOWN or MOVE, and also
 * on its single UP frame -- Unity expects to see the finger one last time in
 * phase Ended, and a UI that only handles Ended never fires without it. */
static int live_count(void) {
  int n = 0;
  for (int i = 0; i < g_n; i++)
    if (g_cur[i].phase != BM_TOUCH_NONE) n++;
  return n;
}

/* ------------------------------------------------------------------ hooks */

static int32_t bmi_touchCount(void)     { refresh(); return live_count(); }
static int32_t bmi_touchSupported(void) { return 1; }
static int32_t bmi_mousePresent(void)   { return 1; }

static void bmi_GetTouch_fill(int32_t index, UTouch *ret);   /* defined below */

/* The wrapper the game actually calls. Vector3 is an HFA -- three floats back
 * in s0/s1/s2 -- which is exactly what returning this struct by value produces.
 * See bm_managed.h for why the _Injected hook below is not enough. */
static V3 bmi_mousePosition(void) {
  refresh();
  V3 v; v.x = g_last_pos.x; v.y = g_last_pos.y; v.z = 0.0f;
  return v;
}

/* Ditto for GetTouch. The 0x44-byte Touch comes back through the indirect-result
 * register x8, which is what the compiler emits for a struct this size. */
static UTouch bmi_GetTouch(int32_t index) {
  UTouch t;
  bmi_GetTouch_fill(index, &t);
  return t;
}

static void bmi_mousePosition_Injected(V3 *ret) {
  refresh();
  if (!ret) return;
  ret->x = g_last_pos.x;
  ret->y = g_last_pos.y;
  ret->z = 0.0f;
}

static void bmi_GetTouch_Injected(int32_t index, UTouch *ret) {
  bmi_GetTouch_fill(index, ret);
}

static void bmi_GetTouch_fill(int32_t index, UTouch *ret) {
  if (!ret) return;
  memset(ret, 0, sizeof *ret);
  refresh();

  /* Walk to the index-th live finger rather than indexing g_cur directly:
   * bm_touch's slots are densified but a NONE can still appear between live
   * entries, and Unity's index is over live touches only. */
  int seen = 0;
  for (int i = 0; i < g_n; i++) {
    if (g_cur[i].phase == BM_TOUCH_NONE) continue;
    if (seen++ != index) continue;

    int slot = g_cur[i].id;
    if (slot < 0 || slot >= NX_MAX_TOUCH) slot = 0;

    ret->fingerId    = g_cur[i].id;
    ret->position.x  = g_cur[i].x;
    ret->position.y  = g_cur[i].y;
    ret->rawPosition = ret->position;

    if (g_prev_valid[slot]) {
      ret->positionDelta.x = g_cur[i].x - g_prev_pos[slot].x;
      ret->positionDelta.y = g_cur[i].y - g_prev_pos[slot].y;
    }
    /* Only on the frame's FIRST poll -- see refresh(). Otherwise a second
     * GetTouch in the same frame would compare the position against itself. */
    if (g_frame_is_new) {
      g_prev_pos[slot]   = ret->position;
      g_prev_valid[slot] = (g_cur[i].phase != BM_TOUCH_UP);
    }

    switch (g_cur[i].phase) {
      case BM_TOUCH_DOWN: ret->phase = UTP_BEGAN; break;
      case BM_TOUCH_UP:   ret->phase = UTP_ENDED; break;
      /* A finger that has not moved this frame is Stationary, not Moved.
       * A drag-threshold check that sees Moved every frame starts a drag on a
       * stationary finger, which makes buttons impossible to press. */
      default:
        ret->phase = (ret->positionDelta.x == 0.0f && ret->positionDelta.y == 0.0f)
                   ? UTP_STATIONARY : UTP_MOVED;
        break;
    }

    ret->tapCount    = 1;
    ret->timeDelta   = 1.0f / 60.0f;
    ret->pressure    = 1.0f;
    ret->maxPressure = 1.0f;
    ret->type        = 0;                  /* TouchType.Direct */
    return;
  }
  /* Out of range: a zeroed Touch with fingerId -1, which is what Unity's own
   * bounds failure produces. */
  ret->fingerId = -1;
}

/* Mouse emulation, same finger. Button 0 only; 1 and 2 are always up. */
static int32_t bmi_GetMouseButton(int32_t button) {
  if (button != 0) return 0;
  refresh();
  for (int i = 0; i < g_n; i++)
    if (g_cur[i].phase == BM_TOUCH_DOWN || g_cur[i].phase == BM_TOUCH_MOVE) return 1;
  return 0;
}

static int32_t bmi_GetMouseButtonDown(int32_t button) {
  if (button != 0) return 0;
  refresh();
  for (int i = 0; i < g_n; i++) if (g_cur[i].phase == BM_TOUCH_DOWN) return 1;
  return 0;
}

static int32_t bmi_GetMouseButtonUp(int32_t button) {
  if (button != 0) return 0;
  refresh();
  for (int i = 0; i < g_n; i++) if (g_cur[i].phase == BM_TOUCH_UP) return 1;
  return 0;
}

/* anyKey covers "is the user touching anything at all", which some menus use to
 * skip a splash. anyKeyDown is the edge. */
static int32_t bmi_anyKey(void)     { refresh(); return bm_touch_any_down(); }
static int32_t bmi_anyKeyDown(void) { return bmi_GetMouseButtonDown(0); }


/* Switch gamepad -> Unity KeyCode mapping for OpenBonk */
static int keycode_held(int32_t kc) {
  static PadState pad;
  static int inited = 0;
  if (!inited) { padInitializeDefault(&pad); inited = 1; }
  padUpdate(&pad);
  u64 h = padGetButtons(&pad);
  HidAnalogStickState ls = padGetStickPos(&pad, 0);
  const int dz = 8000;
  switch (kc) {
    case 119: case 273: return ls.y >  dz || (h & HidNpadButton_Up);     /* W / Up */
    case 115: case 274: return ls.y < -dz || (h & HidNpadButton_Down);   /* S / Down */
    case 97:  case 276: return ls.x < -dz || (h & HidNpadButton_Left);   /* A / Left */
    case 100: case 275: return ls.x >  dz || (h & HidNpadButton_Right);  /* D / Right */
    case 13:  return (h & HidNpadButton_A)  != 0;                        /* Return -> A */
    case 32:  return (h & HidNpadButton_B)  != 0;                        /* Space  -> B (Jump) */
    case 27:  return (h & HidNpadButton_Y)  != 0;                        /* Escape -> Y (Pause) */
    case 113: case 9: return (h & HidNpadButton_X) != 0;                 /* Q/Tab  -> X */
    case 101: case 102: return (h & HidNpadButton_ZR) != 0;              /* E/F    -> ZR (Interact) */
    case 304: return (h & HidNpadButton_ZL) != 0;                        /* LShift -> ZL */
    default:  return 0;
  }
}

static uint8_t g_kc_prev[512];
static int32_t bmi_GetKeyInt(int32_t kc) {
  int v = keycode_held(kc);
  if (kc >= 0 && kc < 512) g_kc_prev[kc] = (uint8_t)v;
  return v;
}
static int32_t bmi_GetKeyDownInt(int32_t kc) {
  int v = keycode_held(kc);
  int prev = (kc >= 0 && kc < 512) ? g_kc_prev[kc] : 0;
  if (kc >= 0 && kc < 512) g_kc_prev[kc] = (uint8_t)v;
  return v && !prev;
}
static int32_t bmi_GetKeyUpInt(int32_t kc) {
  int v = keycode_held(kc);
  int prev = (kc >= 0 && kc < 512) ? g_kc_prev[kc] : 0;
  if (kc >= 0 && kc < 512) g_kc_prev[kc] = (uint8_t)v;
  return !v && prev;
}


/* ---- OpenBonk MyInputManager hooks (Left/Right sticks + controller buttons) ---- */
static PadState g_ob_pad;
static int g_ob_pad_init = 0;
static void ob_pad_poll(u64 *held_out, HidAnalogStickState *ls_out, HidAnalogStickState *rs_out) {
  if (!g_ob_pad_init) { padInitializeDefault(&g_ob_pad); g_ob_pad_init = 1; }
  padUpdate(&g_ob_pad);
  if (held_out) *held_out = padGetButtons(&g_ob_pad);
  if (ls_out)   *ls_out   = padGetStickPos(&g_ob_pad, 0);
  if (rs_out)   *rs_out   = padGetStickPos(&g_ob_pad, 1);
}

static int il2cpp_str_eq(void *s, const char *ascii) {
  if (!s || !ascii) return 0;
  int32_t len = *(const int32_t *)((const uint8_t *)s + 0x10);
  const uint16_t *chars = (const uint16_t *)((const uint8_t *)s + 0x14);
  for (int32_t i = 0; i < len; i++) {
    if (!ascii[i] || chars[i] != (uint16_t)(unsigned char)ascii[i]) return 0;
  }
  return ascii[len] == '\0';
}

static float ob_stick_norm(int32_t v) {
  const int32_t dz = 3000;
  if (v > -dz && v < dz) return 0.0f;
  float f = (float)v / 32767.0f;
  if (f >  1.0f) f =  1.0f;
  if (f < -1.0f) f = -1.0f;
  return f;
}

static float bmi_MyInput_GetAxis(void *action_str) {
  u64 h = 0; HidAnalogStickState ls = {0}, rs = {0};
  ob_pad_poll(&h, &ls, &rs);
  if (il2cpp_str_eq(action_str, "Move Horizontal")) {
    float v = ob_stick_norm(ls.x);
    if (h & HidNpadButton_Right) v =  1.0f;
    if (h & HidNpadButton_Left)  v = -1.0f;
    return v;
  }
  if (il2cpp_str_eq(action_str, "Move Vertical")) {
    float v = ob_stick_norm(ls.y);
    if (h & HidNpadButton_Up)   v =  1.0f;
    if (h & HidNpadButton_Down) v = -1.0f;
    return v;
  }
  if (il2cpp_str_eq(action_str, "Look Horizontal") ||
      il2cpp_str_eq(action_str, "ControllerLookHorizontal")) {
    return ob_stick_norm(rs.x);
  }
  if (il2cpp_str_eq(action_str, "Look Vertical") ||
      il2cpp_str_eq(action_str, "ControllerLookVertical")) {
    return -ob_stick_norm(rs.y);
  }
  return 0.0f;
}

static int ob_action_held(void *action_str, int *slot_out) {
  u64 h = 0;
  ob_pad_poll(&h, NULL, NULL);
  if (il2cpp_str_eq(action_str, "Jump")) {
    if (slot_out) *slot_out = 0;
    return (h & HidNpadButton_B) != 0;
  }
  if (il2cpp_str_eq(action_str, "Interact")) {
    if (slot_out) *slot_out = 1;
    return (h & HidNpadButton_ZR) != 0;
  }
  if (il2cpp_str_eq(action_str, "Pause")) {
    if (slot_out) *slot_out = 2;
    return (h & HidNpadButton_Y) != 0;
  }
  if (il2cpp_str_eq(action_str, "Confirm") || il2cpp_str_eq(action_str, "Submit")) {
    if (slot_out) *slot_out = 3;
    return (h & HidNpadButton_A) != 0;
  }
  if (il2cpp_str_eq(action_str, "Cancel") || il2cpp_str_eq(action_str, "Back")) {
    if (slot_out) *slot_out = 4;
    return (h & HidNpadButton_B) != 0;
  }
  if (il2cpp_str_eq(action_str, "Ability") || il2cpp_str_eq(action_str, "Dash") ||
      il2cpp_str_eq(action_str, "Slide")   || il2cpp_str_eq(action_str, "Crouch")) {
    if (slot_out) *slot_out = 5;
    return (h & (HidNpadButton_X | HidNpadButton_ZL)) != 0;
  }
  if (slot_out) *slot_out = -1;
  return 0;
}

static uint8_t g_ob_btn_down_prev[8];
static uint8_t g_ob_btn_up_prev[8];

static int32_t bmi_MyInput_GetButton(void *action_str) {
  return ob_action_held(action_str, NULL);
}
static int32_t bmi_MyInput_GetButtonDown(void *action_str) {
  int slot = -1;
  int v = ob_action_held(action_str, &slot);
  if (slot < 0) return 0;
  int prev = g_ob_btn_down_prev[slot];
  g_ob_btn_down_prev[slot] = (uint8_t)v;
  return v && !prev;
}
static int32_t bmi_MyInput_GetButtonUp(void *action_str) {
  int slot = -1;
  int v = ob_action_held(action_str, &slot);
  if (slot < 0) return 0;
  int prev = g_ob_btn_up_prev[slot];
  g_ob_btn_up_prev[slot] = (uint8_t)v;
  return !v && prev;
}

/* ---------------------------------------------------------------- install */

int bm_input_install(so_module *il2cpp) {
  static const uint32_t g_mp   [3] = BMM_G3_Input_get_mousePosition_Injected;
  static const uint32_t g_mpre [3] = BMM_G3_Input_get_mousePresent;
  static const uint32_t g_tsup [3] = BMM_G3_Input_get_touchSupported;
  static const uint32_t g_tcnt [3] = BMM_G3_Input_get_touchCount;
  static const uint32_t g_mb   [3] = BMM_G3_Input_GetMouseButton;
  static const uint32_t g_mbd  [3] = BMM_G3_Input_GetMouseButtonDown;
  static const uint32_t g_mbu  [3] = BMM_G3_Input_GetMouseButtonUp;
  static const uint32_t g_gt   [3] = BMM_G3_Input_GetTouch_Injected;
  static const uint32_t g_ak   [3] = BMM_G3_Input_get_anyKey;
  static const uint32_t g_akd  [3] = BMM_G3_Input_get_anyKeyDown;
  static const uint32_t g_mp2  [3] = BMM_G3_Input_get_mousePosition;
  static const uint32_t g_gt2  [3] = BMM_G3_Input_GetTouch;

  int n = 0;
  n += bm_managed_hook(il2cpp, "Input.get_mousePosition_Injected",
                       BMM_RVA_Input_get_mousePosition_Injected, g_mp,   (void *)&bmi_mousePosition_Injected);
  n += bm_managed_hook(il2cpp, "Input.get_mousePresent",
                       BMM_RVA_Input_get_mousePresent,           g_mpre, (void *)&bmi_mousePresent);
  n += bm_managed_hook(il2cpp, "Input.get_touchSupported",
                       BMM_RVA_Input_get_touchSupported,         g_tsup, (void *)&bmi_touchSupported);
  n += bm_managed_hook(il2cpp, "Input.get_touchCount",
                       BMM_RVA_Input_get_touchCount,             g_tcnt, (void *)&bmi_touchCount);
  n += bm_managed_hook(il2cpp, "Input.GetMouseButton",
                       BMM_RVA_Input_GetMouseButton,             g_mb,   (void *)&bmi_GetMouseButton);
  n += bm_managed_hook(il2cpp, "Input.GetMouseButtonDown",
                       BMM_RVA_Input_GetMouseButtonDown,         g_mbd,  (void *)&bmi_GetMouseButtonDown);
  n += bm_managed_hook(il2cpp, "Input.GetMouseButtonUp",
                       BMM_RVA_Input_GetMouseButtonUp,           g_mbu,  (void *)&bmi_GetMouseButtonUp);
  n += bm_managed_hook(il2cpp, "Input.GetTouch_Injected",
                       BMM_RVA_Input_GetTouch_Injected,          g_gt,   (void *)&bmi_GetTouch_Injected);
  n += bm_managed_hook(il2cpp, "Input.get_anyKey",
                       BMM_RVA_Input_get_anyKey,                 g_ak,   (void *)&bmi_anyKey);
  n += bm_managed_hook(il2cpp, "Input.get_anyKeyDown",
                       BMM_RVA_Input_get_anyKeyDown,             g_akd,  (void *)&bmi_anyKeyDown);

  /* THE TWO THAT MATTER. The game calls these directly; the _Injected pair
     above have zero call sites in this build. */
  n += bm_managed_hook(il2cpp, "Input.get_mousePosition  [REAL]",
                       BMM_RVA_Input_get_mousePosition, g_mp2, (void *)&bmi_mousePosition);
  n += bm_managed_hook(il2cpp, "Input.GetTouch           [REAL]",
                       BMM_RVA_Input_GetTouch,          g_gt2, (void *)&bmi_GetTouch);

  static const uint32_t g_gki [3] = BMM_G3_Input_GetKeyInt;
  static const uint32_t g_gkd [3] = BMM_G3_Input_GetKeyDownInt;
  static const uint32_t g_gku [3] = BMM_G3_Input_GetKeyUpInt;
  n += bm_managed_hook(il2cpp, "Input.GetKeyInt",     BMM_RVA_Input_GetKeyInt,     g_gki, (void *)&bmi_GetKeyInt);
  n += bm_managed_hook(il2cpp, "Input.GetKeyDownInt", BMM_RVA_Input_GetKeyDownInt, g_gkd, (void *)&bmi_GetKeyDownInt);
  n += bm_managed_hook(il2cpp, "Input.GetKeyUpInt",   BMM_RVA_Input_GetKeyUpInt,   g_gku, (void *)&bmi_GetKeyUpInt);
  static const uint32_t g_mia [3] = BMM_G3_MyInput_GetAxis;
  static const uint32_t g_mib [3] = BMM_G3_MyInput_GetButton;
  static const uint32_t g_mibd[3] = BMM_G3_MyInput_GetButtonDown;
  static const uint32_t g_mibu[3] = BMM_G3_MyInput_GetButtonUp;
  n += bm_managed_hook(il2cpp, "MyInputManager.GetAxis",       BMM_RVA_MyInput_GetAxis,       g_mia,  (void *)&bmi_MyInput_GetAxis);
  n += bm_managed_hook(il2cpp, "MyInputManager.GetButton",     BMM_RVA_MyInput_GetButton,     g_mib,  (void *)&bmi_MyInput_GetButton);
  n += bm_managed_hook(il2cpp, "MyInputManager.GetButtonDown", BMM_RVA_MyInput_GetButtonDown, g_mibd, (void *)&bmi_MyInput_GetButtonDown);
  n += bm_managed_hook(il2cpp, "MyInputManager.GetButtonUp",   BMM_RVA_MyInput_GetButtonUp,   g_mibu, (void *)&bmi_MyInput_GetButtonUp);
  debugPrintf("[input] %d/19 Input accessors hooked (touch + mouse emulation)\n", n);
  if (n == 0)
    debugPrintf("[input] NONE installed -- the game will be unplayable. Regenerate\n"
                "        bm_managed.h from a dump.cs matching THIS libil2cpp.\n");
  return n;
}