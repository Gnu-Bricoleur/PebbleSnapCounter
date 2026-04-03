#include <pebble.h>

// ======================================================
// UI STRUCTURE
// ======================================================
typedef enum {
  SCREEN_HOME,
  SCREEN_MODE_SELECT,
  SCREEN_COUNTER,
  SCREEN_PERCENT
} Screen;

static Window *s_main_window;
static TextLayer *s_main_text;
static TextLayer *s_sub_text;
static TextLayer *s_hint_top;
static TextLayer *s_hint_bottom;

static Screen current_screen = SCREEN_HOME;

// ======================================================
// COUNTERS
// ======================================================
static int counter_a = 0;
static int counter_b = 0;

// ======================================================
// CALIBRATION STATE
// ======================================================
static bool calibrated = false;
static bool calibrating = false;

static int baseline_samples = 0;
static int baseline_sum = 0;
static int baseline_mean = 0;

#define SNAP_COUNT 10
static int snap_index = 0;
static int snap_energy[SNAP_COUNT];
static int snap_dir_z[SNAP_COUNT];

static int threshold = 400;
static int sharpness = 200;
static int avg_dir_z = 0;

#define COOLDOWN_MS 300
#define SNAP_GAP_MS 800

static uint32_t last_trigger = 0;
static uint32_t last_snap_time = 0;

// ======================================================
// SIGNAL PROCESSING
// ======================================================
#define WINDOW 12
static int buffer[WINDOW];
static int idx = 0;
static int moving_avg = 0;

static int magnitude(AccelData *d) {
  return abs(d->x) + abs(d->y) + abs(d->z);
}

static bool detect_snap(int *peak_out) {
  int peak = 0;
  int peak_i = -1;

  for(int i = 0; i < WINDOW; i++) {
    if(buffer[i] > peak) {
      peak = buffer[i];
      peak_i = i;
    }
  }

  if(peak < threshold) return false;

  if(peak_i > 0) {
    int rise = peak - buffer[peak_i-1];
    if(rise < sharpness) return false;
  }

  if(peak_i < WINDOW-1) {
    int fall = peak - buffer[peak_i+1];
    if(fall < sharpness/2) return false;
  }

  *peak_out = peak;
  return true;
}

// ======================================================
// DISPLAY
// ======================================================
static void render() {
  static char main[64];
  static char sub[64];

  switch(current_screen) {

    case SCREEN_HOME:
      snprintf(main, sizeof(main), "Snap\nCount");
      snprintf(sub, sizeof(sub), "%s", "");
      text_layer_set_text(s_hint_top, "+ Single");
      text_layer_set_text(s_hint_bottom, "- % Mode");
      break;

    case SCREEN_MODE_SELECT:
      snprintf(main, sizeof(main), "Resume?");
      snprintf(sub, sizeof(sub), "+ Yes  - New");
      text_layer_set_text(s_hint_top, "");
      text_layer_set_text(s_hint_bottom, "");
      break;

    case SCREEN_COUNTER:
      if(calibrating) {
        snprintf(main, sizeof(main), "Calib");
        snprintf(sub, sizeof(sub), "%d/10 snaps", snap_index);
      } else {
        snprintf(main, sizeof(main), "%d", counter_a);
        snprintf(sub, sizeof(sub), "Snap to count");
      }
      text_layer_set_text(s_hint_top, "");
      text_layer_set_text(s_hint_bottom, "");
      break;

    case SCREEN_PERCENT: {
      int total = counter_a + counter_b;
      int percent = total ? (counter_a * 100 / total) : 0;

      if(calibrating) {
        snprintf(main, sizeof(main), "Calib");
        snprintf(sub, sizeof(sub), "%d/10 snaps", snap_index);
      } else {
        snprintf(main, sizeof(main), "%d%%", percent);
        snprintf(sub, sizeof(sub), "A:%d B:%d", counter_a, counter_b);
      }
      text_layer_set_text(s_hint_top, "");
      text_layer_set_text(s_hint_bottom, "");
      break;
    }
  }

  text_layer_set_overflow_mode(s_main_text, GTextOverflowModeWordWrap);
  //text_layer_set_text_alignment(s_main_text, GTextAlignmentCenter);
  text_layer_set_text(s_main_text, main);
  text_layer_set_text(s_sub_text, sub);
}

// ======================================================
// CALIBRATION
// ======================================================
static void finish_calibration() {
  int sum = 0;
  int dir = 0;

  for(int i=0;i<SNAP_COUNT;i++){
    sum += snap_energy[i];
    dir += snap_dir_z[i];
  }

  int avg = sum / SNAP_COUNT;
  avg_dir_z = dir / SNAP_COUNT;

  threshold = avg * 0.6;
  sharpness = avg * 0.2;

  calibrated = true;
  calibrating = false;

  render();
}

// ======================================================
// MAIN PROCESSING
// ======================================================
static void process_sample(AccelData *d) {
  int mag = magnitude(d);

  moving_avg = (moving_avg * 7 + mag) / 8;
  int filtered = mag - moving_avg;

  buffer[idx] = filtered;
  idx = (idx + 1) % WINDOW;

  uint32_t now = time_ms(NULL, NULL);

  int peak;

  // ===== BASELINE =====
  if(calibrating && baseline_samples < 50) {
    baseline_sum += mag;
    baseline_samples++;

    if(baseline_samples == 50) {
      baseline_mean = baseline_sum / 50;
    }
    return;
  }

  // ===== SNAP CALIBRATION =====
  if(calibrating && snap_index < SNAP_COUNT) {

    if(now - last_snap_time < SNAP_GAP_MS) return;

    if(detect_snap(&peak)) {

      int raw_mag = magnitude(d);
      int dir = (raw_mag > 0) ? (d->z * 1000 / raw_mag) : 0;

      snap_energy[snap_index] = peak;
      snap_dir_z[snap_index] = dir;

      snap_index++;
      last_snap_time = now;

      vibes_short_pulse();

      if(snap_index == SNAP_COUNT) {
        finish_calibration();
      }

      render();
    }
    return;
  }

  // ===== DETECTION =====
  if(calibrated && !calibrating && (now - last_trigger > COOLDOWN_MS)) {

    if(detect_snap(&peak)) {

      int raw_mag = magnitude(d);
      int dir = (raw_mag > 0) ? (d->z * 1000 / raw_mag) : 0;

      int diff = abs(dir - avg_dir_z);

      if(diff < 300) {

        if(current_screen == SCREEN_COUNTER) {
          counter_a++;
        }

        if(current_screen == SCREEN_PERCENT) {
          if(dir > avg_dir_z) counter_a++;
          else counter_b++;
        }

        last_trigger = now;
        vibes_short_pulse();
        render();
      }
    }
  }
}

static void accel_handler(AccelData *data, uint32_t n) {
  for(uint32_t i=0;i<n;i++) process_sample(&data[i]);
}

// ======================================================
// BUTTONS
// ======================================================
static void select_click_handler(ClickRecognizerRef r, void *c) {
  calibrating = true;
  calibrated = false;

  baseline_samples = 0;
  baseline_sum = 0;

  snap_index = 0;
  last_snap_time = 0;

  render();
}

static void up_click_handler(ClickRecognizerRef r, void *c) {
    if(current_screen == SCREEN_HOME) {
        current_screen = SCREEN_MODE_SELECT;
    } else if(current_screen == SCREEN_MODE_SELECT) {
        current_screen = SCREEN_COUNTER;
    } else if(current_screen == SCREEN_COUNTER) {
        // Manual adjustment: increment
        counter_a++;
    } else if(current_screen == SCREEN_PERCENT) {
        // Manual adjustment: increment A
        counter_a++;
    }
    render();
}

static void down_click_handler(ClickRecognizerRef r, void *c) {
    if(current_screen == SCREEN_HOME) {
        current_screen = SCREEN_PERCENT;
    } else if(current_screen == SCREEN_MODE_SELECT) {
        counter_a = 0;
        current_screen = SCREEN_COUNTER;
    } else if(current_screen == SCREEN_COUNTER) {
        // Manual adjustment: decrement
        if(counter_a > 0) counter_a--;
    } else if(current_screen == SCREEN_PERCENT) {
        // Manual adjustment: increment B
        counter_b++;
    }
    render();
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
}

// ======================================================
// WINDOW
// ======================================================
static void main_window_load(Window *window) {
  Layer *layer = window_get_root_layer(window);
  GRect b = layer_get_bounds(layer);

  s_main_text = text_layer_create(GRect(0, 40, b.size.w, 50));
  text_layer_set_text_alignment(s_main_text, GTextAlignmentCenter);
  text_layer_set_font(s_main_text, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));

  s_sub_text = text_layer_create(GRect(0, 90, b.size.w, 40));
  text_layer_set_text_alignment(s_sub_text, GTextAlignmentCenter);
  text_layer_set_font(s_sub_text, fonts_get_system_font(FONT_KEY_GOTHIC_24));

  s_hint_top = text_layer_create(GRect(0, 0, b.size.w, 30));
  text_layer_set_text_alignment(s_hint_top, GTextAlignmentCenter);

  s_hint_bottom = text_layer_create(GRect(0, b.size.h - 30, b.size.w, 30));
  text_layer_set_text_alignment(s_hint_bottom, GTextAlignmentCenter);

  layer_add_child(layer, text_layer_get_layer(s_main_text));
  layer_add_child(layer, text_layer_get_layer(s_sub_text));
  layer_add_child(layer, text_layer_get_layer(s_hint_top));
  layer_add_child(layer, text_layer_get_layer(s_hint_bottom));

  render();
}

static void main_window_unload(Window *window) {
  text_layer_destroy(s_main_text);
  text_layer_destroy(s_sub_text);
  text_layer_destroy(s_hint_top);
  text_layer_destroy(s_hint_bottom);
}

// ======================================================
// APP
// ======================================================
static void init() {
  s_main_window = window_create();

  window_set_click_config_provider(s_main_window, click_config_provider);

  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload,
  });

  window_stack_push(s_main_window, true);

  accel_data_service_subscribe(5, accel_handler);
  accel_service_set_sampling_rate(ACCEL_SAMPLING_50HZ);
}

static void deinit() {
  accel_data_service_unsubscribe();
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
