#include "motion_controller.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_state.h"
#include "app_utils.h"
#include "data_logger.h"
#include "error_manager.h"
#include "hw_pins.h"
#include "gps_module.h"
#include "sensor_hub.h"
#include "storage_manager.h"
#include "upload_pipeline.h"

static constexpr char kTag[] = "MOTION";

// ---------- private globals ----------

static constexpr uint32_t kExtPwrCycleStackBytes = 6144;
static TaskHandle_t s_ext_pwr_cycle_task = nullptr;
static MeasurementPublishFn s_publish_fn = nullptr;
static volatile uint32_t s_hall_edge_count = 0;
static volatile uint32_t s_hall_level0_edge_count = 0;
static volatile uint32_t s_hall_level1_edge_count = 0;
static volatile int s_hall_last_raw_level = -1;
static bool s_hall_isr_registered = false;
static uint32_t s_hall_last_reported_edge_count = 0;
static int64_t s_hall_last_edge_seen_us = 0;

// ---------- GPIO helpers ----------

static void IRAM_ATTR HallSensorIsr(void*) {
  const int raw = gpio_get_level(MT_HALL_SEN);
  s_hall_last_raw_level = raw;
  s_hall_edge_count = s_hall_edge_count + 1;
  if (raw == 0) {
    s_hall_level0_edge_count = s_hall_level0_edge_count + 1;
  } else {
    s_hall_level1_edge_count = s_hall_level1_edge_count + 1;
  }
}

uint32_t HallEdgeCount() { return s_hall_edge_count; }

uint32_t HallLevel0EdgeCount() { return s_hall_level0_edge_count; }

uint32_t HallLevel1EdgeCount() { return s_hall_level1_edge_count; }

uint32_t HallActiveEdgeCount() {
  return app_config.motor_hall_active_level ? HallLevel1EdgeCount() : HallLevel0EdgeCount();
}

int HallLastRawLevel() { return s_hall_last_raw_level; }

void RefreshHallDebugState() {
  const int raw = gpio_get_level(MT_HALL_SEN);
  const bool triggered = raw == app_config.motor_hall_active_level;
  const uint32_t edge_count = HallEdgeCount();
  const uint32_t active_edge_count = HallActiveEdgeCount();
  const uint32_t level0_edges = HallLevel0EdgeCount();
  const uint32_t level1_edges = HallLevel1EdgeCount();
  const int last_edge_level = HallLastRawLevel();
  const int64_t now_us = esp_timer_get_time();
  if (edge_count != s_hall_last_reported_edge_count) {
    s_hall_last_reported_edge_count = edge_count;
    s_hall_last_edge_seen_us = now_us;
  }
  UpdateState([&](SharedState& s) {
    s.motor_hall_raw_level = raw;
    s.motor_hall_triggered = triggered;
    s.motor_hall_edge_count = edge_count;
    s.motor_hall_active_edge_count = active_edge_count;
    s.motor_hall_level0_edge_count = level0_edges;
    s.motor_hall_level1_edge_count = level1_edges;
    s.motor_hall_last_edge_level = last_edge_level;
    s.motor_hall_last_edge_seen_us = s_hall_last_edge_seen_us;
  });
}

bool IsHallTriggered() {
  return gpio_get_level(MT_HALL_SEN) == app_config.motor_hall_active_level;
}

void SetExternalPower(bool enabled) {
  gpio_set_level(EXT_PWR_ON, enabled ? 1 : 0);
  UpdateState([&](SharedState& s) { s.external_power_on = enabled; });
  ESP_LOGI(kTag, "External module power %s", enabled ? "ON" : "OFF");
}

static void ExternalPowerCycleTask(void* arg) {
  const uint32_t delay_ms = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
  SetExternalPower(false);
  vTaskDelay(pdMS_TO_TICKS(delay_ms));
  SetExternalPower(true);
  s_ext_pwr_cycle_task = nullptr;
  vTaskDelete(nullptr);
}

bool CycleExternalPower(uint32_t off_ms) {
  if (s_ext_pwr_cycle_task != nullptr) return false;
  off_ms = std::clamp<uint32_t>(off_ms, 100, 30000);
  BaseType_t ok = xTaskCreate(&ExternalPowerCycleTask, "ext_pwr_cycle", kExtPwrCycleStackBytes,
                              reinterpret_cast<void*>(static_cast<uintptr_t>(off_ms)), 4,
                              &s_ext_pwr_cycle_task);
  return ok == pdPASS;
}

int GetGpsAntennaShortRaw() {
  if (GPS_ANT_SHORT == GPIO_NUM_NC) return -1;
  return gpio_get_level(GPS_ANT_SHORT);
}

bool IsGpsAntennaShort() {
  return GetGpsAntennaShortRaw() == 0;
}

// ---------- heater / fan PWM ----------

void MotionControllerSetPublisher(MeasurementPublishFn fn) { s_publish_fn = fn; }

void MotionControllerInit() {
  if (MT_HALL_SEN != GPIO_NUM_NC && !s_hall_isr_registered) {
    s_hall_last_raw_level = gpio_get_level(MT_HALL_SEN);
    EnsureGpioIsrServiceInstalled();
    ESP_ERROR_CHECK(gpio_set_intr_type(MT_HALL_SEN, GPIO_INTR_ANYEDGE));
    ESP_ERROR_CHECK(gpio_isr_handler_add(MT_HALL_SEN, HallSensorIsr, nullptr));
    s_hall_isr_registered = true;
    RefreshHallDebugState();
    ESP_LOGI(kTag, "Hall capture enabled: raw=%d active=%d",
             gpio_get_level(MT_HALL_SEN), app_config.motor_hall_active_level);
  }

  // Heater: LEDC timer 0 + channel 0 at 1 kHz, 10-bit
  if (HEATER_PWM != GPIO_NUM_NC) {
    ledc_timer_config_t t = {};
    t.speed_mode     = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_10_BIT;
    t.timer_num      = LEDC_TIMER_0;
    t.freq_hz        = 1000;
    t.clk_cfg        = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c = {};
    c.gpio_num   = HEATER_PWM;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel    = LEDC_CHANNEL_0;
    c.timer_sel  = LEDC_TIMER_0;
    c.duty       = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&c));
  }

  // Fan: LEDC timer 1 + channel 1 at 25 kHz, 10-bit
  if (FAN_PWM != GPIO_NUM_NC) {
    ledc_timer_config_t t = {};
    t.speed_mode      = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_10_BIT;
    t.timer_num       = LEDC_TIMER_1;
    t.freq_hz         = 25000;
    t.clk_cfg         = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c = {};
    c.gpio_num   = FAN_PWM;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel    = LEDC_CHANNEL_1;
    c.timer_sel  = LEDC_TIMER_1;
    c.duty       = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&c));

    FanSetPowerPercent(100.0f);
  } else {
    UpdateState([](SharedState& s) { s.fan_power = 100.0f; });
  }
}

void HeaterSetPowerPercent(float p) {
  if (p < 0.0f) p = 0.0f;
  if (p > 100.0f) p = 100.0f;
  const uint32_t duty = static_cast<uint32_t>(p * 1023.0f / 100.0f);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
  UpdateState([&](SharedState& s) { s.heater_power = p; });
}

void FanSetPowerPercent(float p) {
  if (p < 0.0f) p = 0.0f;
  if (p > 100.0f) p = 100.0f;
  if (FAN_PWM == GPIO_NUM_NC) {
    UpdateState([&](SharedState& s) { s.fan_power = 100.0f; });
    return;
  }
  const uint32_t duty = static_cast<uint32_t>(p * 1023.0f / 100.0f);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
  UpdateState([&](SharedState& s) { s.fan_power = p; });
}

// ---------- stepper primitives ----------

void EnableStepper() {
  gpio_set_level(STEPPER_EN, 0);
  UpdateState([](SharedState& s) { s.stepper_enabled = true; });
}

void DisableStepper() {
  gpio_set_level(STEPPER_EN, 1);
  UpdateState([](SharedState& s) {
    s.stepper_enabled = false;
    s.stepper_moving  = false;
  });
}

void StopStepper() {
  UpdateState([](SharedState& s) {
    s.stepper_moving = false;
    s.stepper_abort  = true;
    s.homing         = false;
  });
}

void StartStepperMove(int steps, bool forward, int speed_us) {
  const int clamped = std::max(speed_us, 1);
  UpdateState([=](SharedState& s) {
    if (!s.stepper_enabled) return;
    s.stepper_abort              = false;
    s.stepper_direction_forward  = forward;
    s.stepper_speed_us           = clamped;
    s.stepper_target             = s.stepper_position + (forward ? steps : -steps);
    s.stepper_moving             = true;
    s.last_step_timestamp_us     = esp_timer_get_time();
  });
  gpio_set_level(STEPPER_DIR, forward ? 1 : 0);
}

// ---------- StepperTask ----------

struct StepperTaskSnapshot {
  bool    homing                  = false;
  bool    stepper_abort           = false;
  bool    stepper_enabled         = false;
  bool    stepper_moving          = false;
  bool    stepper_direction_forward = true;
  int     stepper_speed_us        = 1;
  int64_t last_step_timestamp_us  = 0;
};

static StepperTaskSnapshot ReadStepperTaskSnapshot() {
  StepperTaskSnapshot out{};
  if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    out.homing                   = state.homing;
    out.stepper_abort            = state.stepper_abort;
    out.stepper_enabled          = state.stepper_enabled;
    out.stepper_moving           = state.stepper_moving;
    out.stepper_direction_forward = state.stepper_direction_forward;
    out.stepper_speed_us         = state.stepper_speed_us;
    out.last_step_timestamp_us   = state.last_step_timestamp_us;
    xSemaphoreGive(state_mutex);
  }
  out.stepper_speed_us = std::max(out.stepper_speed_us, 1);
  return out;
}

static void StepperTask(void*) {
  const TickType_t idle_active = pdMS_TO_TICKS(1);
  const TickType_t idle_idle   = pdMS_TO_TICKS(5);
  while (true) {
    StepperTaskSnapshot snap = ReadStepperTaskSnapshot();
    if (snap.homing && !snap.stepper_abort) {
      vTaskDelay(idle_idle);
      continue;
    }
    if (snap.stepper_enabled && snap.stepper_moving && !snap.stepper_abort) {
      gpio_set_level(STEPPER_DIR, snap.stepper_direction_forward ? 1 : 0);
      const int64_t now = esp_timer_get_time();
      if (now - snap.last_step_timestamp_us >= snap.stepper_speed_us) {
        gpio_set_level(STEPPER_STEP, 1);
        esp_rom_delay_us(4);
        gpio_set_level(STEPPER_STEP, 0);
        UpdateState([&](SharedState& s) {
          if (!s.stepper_moving || !s.stepper_enabled) return;
          s.stepper_position += s.stepper_direction_forward ? 1 : -1;
          s.last_step_timestamp_us = now;
          if ((s.stepper_direction_forward  && s.stepper_position >= s.stepper_target) ||
              (!s.stepper_direction_forward && s.stepper_position <= s.stepper_target)) {
            s.stepper_moving = false;
          }
        });
        vTaskDelay(idle_active);
      }
    } else if (snap.stepper_abort) {
      UpdateState([](SharedState& s) { s.stepper_moving = false; });
    }
    vTaskDelay(snap.stepper_moving ? idle_active : idle_idle);
  }
}

// ---------- PidTask ----------

static void PidTask(void*) {
  float  integral        = 0.0f;
  float  prev_error      = 0.0f;
  int64_t prev_update_us = 0;
  bool   have_prev_error = false;

  SharedState initial = CopyState();
  if (pid_config.from_file && initial.pid_enabled) {
    UpdateState([](SharedState& s) { s.pid_enabled = true; });
  }

  while (true) {
    SharedState snap = CopyState();
    if (!snap.pid_enabled) {
      integral = prev_error = 0.0f;
      prev_update_us  = 0;
      have_prev_error = false;
      UpdateState([](SharedState& s) {
        s.pid_temperature         = 0.0f;
        s.pid_error               = 0.0f;
        s.pid_integral            = 0.0f;
        s.pid_integral_candidate  = 0.0f;
        s.pid_derivative          = 0.0f;
        s.pid_p_term              = 0.0f;
        s.pid_i_term              = 0.0f;
        s.pid_d_term              = 0.0f;
        s.pid_raw_output          = 0.0f;
        s.pid_dt                  = 0.0f;
        s.pid_saturated_high      = false;
        s.pid_saturated_low       = false;
        s.pid_integral_held       = false;
      });
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (snap.temp_sensor_count <= 0) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    uint16_t mask = snap.pid_sensor_mask;
    if (mask == 0) {
      int idx = std::clamp(snap.pid_sensor_index, 0, snap.temp_sensor_count - 1);
      mask = static_cast<uint16_t>(1u << idx);
    }
    float temp_sum  = 0.0f;
    int   temp_count = 0;
    for (int i = 0; i < snap.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i) {
      if ((mask & (1u << i)) == 0) continue;
      float t = snap.temps_c[i];
      if (!std::isfinite(t)) continue;
      temp_sum += t;
      temp_count++;
    }
    if (temp_count == 0) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    const float  temp   = temp_sum / static_cast<float>(temp_count);
    const int64_t now_us = esp_timer_get_time();
    float dt = prev_update_us > 0 ? static_cast<float>(now_us - prev_update_us) / 1000000.0f : 1.0f;
    if (!std::isfinite(dt) || dt <= 0.0f || dt > 10.0f) dt = 1.0f;

    const float error      = snap.pid_setpoint - temp;
    const float derivative = (have_prev_error && dt > 0.0f) ? (error - prev_error) / dt : 0.0f;
    const float candidate  = std::clamp(integral + error * dt, 0.0f, 1500.0f);
    const float p_term     = snap.pid_kp * error;
    const float i_term     = snap.pid_ki * candidate;
    const float d_term     = snap.pid_kd * derivative;
    const float raw_out    = p_term + i_term + d_term;
    const float output     = std::clamp(raw_out, 0.0f, 100.0f);
    const bool sat_hi      = raw_out > 100.0f;
    const bool sat_lo      = raw_out < 0.0f;
    bool held = true;
    if ((!sat_hi && !sat_lo) || (sat_hi && error < 0.0f) || (sat_lo && error > 0.0f)) {
      integral = candidate;
      held     = false;
    }

    HeaterSetPowerPercent(std::clamp(output, 0.0f, 100.0f));
    UpdateState([&](SharedState& s) {
      s.pid_output             = output;
      s.pid_temperature        = temp;
      s.pid_error              = error;
      s.pid_integral           = integral;
      s.pid_integral_candidate = candidate;
      s.pid_derivative         = derivative;
      s.pid_p_term             = p_term;
      s.pid_i_term             = i_term;
      s.pid_d_term             = d_term;
      s.pid_raw_output         = raw_out;
      s.pid_dt                 = dt;
      s.pid_saturated_high     = sat_hi;
      s.pid_saturated_low      = sat_lo;
      s.pid_integral_held      = held;
    });
    prev_error      = error;
    prev_update_us  = now_us;
    have_prev_error = true;
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// ---------- MotionControllerStartTasks ----------

void MotionControllerStartTasks() {
  xTaskCreatePinnedToCore(&StepperTask, "stepper_task", 4096, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(&PidTask,     "pid_task",     8192, nullptr, 2, nullptr, 0);
}

// ---------- CalibrateZero ----------

void CalibrateZero() {
  bool already_running = false;
  UpdateState([&](SharedState& s) {
    already_running = s.calibrating;
    if (!s.calibrating) s.calibrating = true;
  });
  if (already_running) {
    ESP_LOGW(kTag, "Calibration already in progress");
    return;
  }

  gpio_set_level(RELAY_PIN, 1);
  vTaskDelay(pdMS_TO_TICKS(1000));

  constexpr int kSamples       = 100;
  constexpr int kIgnoreSamples = 10;
  float sum1 = 0.0f, sum2 = 0.0f, sum3 = 0.0f;
  int   valid = 0;
  for (int i = 0; i < kSamples; ++i) {
    float v1 = 0.0f, v2 = 0.0f, v3 = 0.0f;
    if (ReadAllAdc(&v1, &v2, &v3) == ESP_OK && i >= kIgnoreSamples) {
      sum1 += v1;
      sum2 += v2;
      sum3 += v3;
      valid++;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }

  if (valid > 0) {
    const float o1 = sum1 / valid, o2 = sum2 / valid, o3 = sum3 / valid;
    UpdateState([&](SharedState& s) {
      s.offset1     = o1;
      s.offset2     = o2;
      s.offset3     = o3;
      s.calibrating = false;
    });
    ESP_LOGI(kTag, "Calibration done: offsets %.6f, %.6f, %.6f", o1, o2, o3);
  } else {
    UpdateState([](SharedState& s) { s.calibrating = false; });
    ESP_LOGW(kTag, "Calibration collected no samples");
  }
  gpio_set_level(RELAY_PIN, 0);
}

void CalibrationTask(void*) {
  CalibrateZero();
  calibration_task = nullptr;
  vTaskDelete(nullptr);
}

// ---------- stepper homing helpers (moved from http_handlers anonymous namespace) ----------

struct StepperHomeResult {
  bool aborted      = false;
  bool hall_found   = false;
  bool offset_done  = false;
  int  hall_steps   = 0;
  int  offset_steps = 0;
};

static constexpr int kStepperHomeRetryAttempts = 3;

static bool StepperHomeSucceeded(const StepperHomeResult& r) {
  return !r.aborted && r.hall_found && r.offset_done;
}

static std::string StepperHomeFailureMessage(const char* phase, const StepperHomeResult& r, int attempts) {
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "%s failed after %d attempt(s): hall=%s offset=%s aborted=%s hall_steps=%d offset_steps=%d",
                phase ? phase : "Stepper homing", attempts,
                r.hall_found ? "yes" : "no",
                r.offset_done ? "yes" : "no",
                r.aborted ? "yes" : "no",
                r.hall_steps, r.offset_steps);
  return std::string(buf);
}

static void StepperPulseOnce() {
  gpio_set_level(STEPPER_STEP, 1);
  esp_rom_delay_us(4);
  gpio_set_level(STEPPER_STEP, 0);
}

static bool MoveStepperBlockingSigned(int signed_steps, int step_delay_us, const char* log_context) {
  if (signed_steps == 0) return true;
  const bool forward = signed_steps > 0;
  const int  steps   = std::abs(signed_steps);
  gpio_set_level(STEPPER_DIR, forward ? 1 : 0);
  UpdateState([&](SharedState& s) {
    s.stepper_direction_forward = forward;
    s.stepper_moving            = true;
    s.stepper_target            = s.stepper_position + signed_steps;
  });
  for (int i = 0; i < steps; ++i) {
    if (CopyState().stepper_abort) {
      ESP_LOGW(kTag, "%s offset aborted after %d/%d steps", log_context, i, steps);
      return false;
    }
    StepperPulseOnce();
    UpdateState([&](SharedState& s) { s.stepper_position += forward ? 1 : -1; });
    esp_rom_delay_us(step_delay_us);
  }
  UpdateState([](SharedState& s) { s.stepper_moving = false; });
  return true;
}

static StepperHomeResult HomeStepperToUserZeroBlocking(bool enable_motor, const char* log_context) {
  StepperHomeResult result{};
  if (enable_motor) EnableStepper();

  UpdateState([](SharedState& s) {
    s.stepper_abort       = false;
    s.homing              = true;
    s.stepper_moving      = true;
    s.stepper_homed       = false;
    s.stepper_home_status = "seeking_hall";
  });

  const int step_delay_us     = std::max(CopyState().stepper_speed_us, 1);
  const uint32_t start_active_edges = HallActiveEdgeCount();
  const uint32_t start_total_edges = HallEdgeCount();
  uint32_t last_logged_edges = start_total_edges;
  RefreshHallDebugState();
  ESP_LOGI(kTag,
           "%s start: raw=%d active=%d triggered=%s speed_us=%d active_edges=%u total_edges=%u",
           log_context, gpio_get_level(MT_HALL_SEN), app_config.motor_hall_active_level,
           IsHallTriggered() ? "yes" : "no", step_delay_us,
           static_cast<unsigned>(start_active_edges), static_cast<unsigned>(start_total_edges));
  constexpr int kMaxSteps     = 20000;
  constexpr bool kHomeFwd     = true;
  gpio_set_level(STEPPER_DIR, kHomeFwd ? 1 : 0);
  UpdateState([](SharedState& s) { s.stepper_direction_forward = kHomeFwd; });

  while (!IsHallTriggered() && HallActiveEdgeCount() == start_active_edges && result.hall_steps < kMaxSteps) {
    if (CopyState().stepper_abort) {
      ESP_LOGW(kTag, "%s aborted before Hall after %d steps", log_context, result.hall_steps);
      result.aborted = true;
      break;
    }
    StepperPulseOnce();
    UpdateState([](SharedState& s) { s.stepper_position += kHomeFwd ? 1 : -1; });
    esp_rom_delay_us(step_delay_us);
    result.hall_steps++;
    const uint32_t current_edges = HallEdgeCount();
    if (current_edges != last_logged_edges) {
      last_logged_edges = current_edges;
      RefreshHallDebugState();
      ESP_LOGI(kTag, "%s Hall edge: steps=%d raw=%d active_edges=%u total_edges=%u",
               log_context, result.hall_steps, gpio_get_level(MT_HALL_SEN),
               static_cast<unsigned>(HallActiveEdgeCount()), static_cast<unsigned>(current_edges));
    }
  }

  if (!result.aborted) {
    const bool found_by_level = IsHallTriggered();
    const bool found_by_latch = HallActiveEdgeCount() != start_active_edges;
    RefreshHallDebugState();
    if (result.hall_steps >= kMaxSteps && !found_by_level && !found_by_latch) {
      ESP_LOGW(kTag,
               "%s Hall not found after %d steps: raw=%d active=%d active_edges=%u->%u total_edges=%u->%u",
               log_context, kMaxSteps, gpio_get_level(MT_HALL_SEN), app_config.motor_hall_active_level,
               static_cast<unsigned>(start_active_edges), static_cast<unsigned>(HallActiveEdgeCount()),
               static_cast<unsigned>(start_total_edges), static_cast<unsigned>(HallEdgeCount()));
    } else {
      result.hall_found = true;
      ESP_LOGI(kTag, "%s Hall found by %s after %d steps: raw=%d active_edges=%u->%u",
               log_context, found_by_level ? "level" : "edge", result.hall_steps,
               gpio_get_level(MT_HALL_SEN), static_cast<unsigned>(start_active_edges),
               static_cast<unsigned>(HallActiveEdgeCount()));
      UpdateState([](SharedState& s) {
        s.stepper_home_status = "hall_found";
        s.stepper_homed       = true;
        s.stepper_position    = 0;
        s.stepper_target      = 0;
      });
      const int offset = app_config.stepper_home_offset_steps;
      if (offset != 0) {
        UpdateState([](SharedState& s) { s.stepper_home_status = "applying_offset"; });
        const bool ok = MoveStepperBlockingSigned(offset, step_delay_us, log_context);
        if (ok) {
          result.offset_done = true;
          UpdateState([](SharedState& s) {
            s.stepper_home_status = "at_user_zero";
            s.stepper_position    = 0;
            s.stepper_target      = 0;
          });
        } else {
          result.aborted = true;
          ESP_LOGW(kTag, "%s offset aborted", log_context);
        }
      } else {
        result.offset_done = true;
        UpdateState([](SharedState& s) { s.stepper_home_status = "at_user_zero"; });
      }
    }
  }

  UpdateState([&](SharedState& s) {
    s.homing         = false;
    s.stepper_moving = false;
    if (result.aborted) {
      s.stepper_abort       = true;
      s.stepper_home_status = "aborted";
    }
  });
  return result;
}

static StepperHomeResult HomeStepperToUserZeroWithRetries(bool enable_motor, const char* log_context,
                                                           int attempts) {
  if (enable_motor) EnableStepper();
  StepperHomeResult last{};
  for (int i = 0; i < attempts; ++i) {
    last = HomeStepperToUserZeroBlocking(false, log_context);
    if (StepperHomeSucceeded(last)) return last;
    if (last.aborted) return last;
    ESP_LOGW(kTag, "%s retry %d/%d", log_context, i + 1, attempts);
    vTaskDelay(pdMS_TO_TICKS(300));
  }
  return last;
}

// ---------- FindZeroTask ----------

void FindZeroTask(void*) {
  const bool hall_initial = IsHallTriggered();
  ESP_LOGI(kTag, "FindZero: start, hall=%s, offset=%d",
           hall_initial ? "yes" : "no", app_config.stepper_home_offset_steps);
  (void)HomeStepperToUserZeroBlocking(true, "FindZero");
  find_zero_task = nullptr;
  vTaskDelete(nullptr);
}

bool StartFindZeroTask(std::string* out_message) {
  if (find_zero_task) {
    if (out_message) *out_message = "homing already running";
    return false;
  }
  UpdateState([](SharedState& s) {
    s.stepper_abort       = false;
    s.stepper_home_status = "running";
    s.stepper_homed       = false;
    s.homing              = true;
  });
  xTaskCreatePinnedToCore(&FindZeroTask, "find_zero", 4096, nullptr, 4, &find_zero_task, 1);
  if (out_message) *out_message = "homing_started";
  return true;
}

// ---------- Logging helpers ----------

static void AppendGpsCsvFields(FILE* file, const GpsPositionSnapshot& gps) {
  if (!file) return;
  if (gps.valid) {
    fprintf(file, ",%.8f,%.8f,%.3f,%d,%d,%lld",
            gps.latitude_deg, gps.longitude_deg, gps.altitude_m,
            gps.fix_quality, gps.satellites, static_cast<long long>(gps.age_ms));
  } else {
    fprintf(file, ",,,,,,");
  }
}

static void PublishLogMeasurement(const std::string& iso, uint64_t ts_ms, const SharedState& base,
                                  const SharedState* cal, UtcTimeSource time_source,
                                  const GpsPositionSnapshot& gps) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "timestampIso", iso.c_str());
  cJSON_AddNumberToObject(root, "timestampMs", static_cast<double>(ts_ms));
  cJSON_AddStringToObject(root, "timeSource", UtcTimeSourceName(time_source));
  cJSON_AddNumberToObject(root, "adc1", base.voltage1);
  cJSON_AddNumberToObject(root, "adc2", base.voltage2);
  cJSON_AddNumberToObject(root, "adc3", base.voltage3);
  cJSON* temps    = cJSON_CreateArray();
  cJSON* temp_obj = cJSON_CreateObject();
  for (int i = 0; i < base.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i) {
    cJSON_AddItemToArray(temps, cJSON_CreateNumber(base.temps_c[i]));
    const std::string key = "t" + std::to_string(i + 1);
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "value", base.temps_c[i]);
    cJSON_AddStringToObject(entry, "address", base.temp_addresses[i].c_str());
    cJSON_AddStringToObject(entry, "label", key.c_str());
    cJSON_AddItemToObject(temp_obj, key.c_str(), entry);
  }
  cJSON_AddItemToObject(root, "temps", temps);
  cJSON_AddItemToObject(root, "tempSensors", temp_obj);
  cJSON_AddNumberToObject(root, "busV", base.ina_bus_voltage);
  cJSON_AddNumberToObject(root, "busI", base.ina_current);
  cJSON_AddNumberToObject(root, "busP", base.ina_power);
  cJSON_AddBoolToObject(root, "logUseMotor", log_config.use_motor);
  cJSON_AddNumberToObject(root, "logDuration", log_config.duration_s);
  SharedState cur = CopyState();
  if (!cur.log_filename.empty()) {
    cJSON_AddStringToObject(root, "logFilename", cur.log_filename.c_str());
  }
  if (cal) {
    cJSON_AddNumberToObject(root, "adc1Cal", cal->voltage1);
    cJSON_AddNumberToObject(root, "adc2Cal", cal->voltage2);
    cJSON_AddNumberToObject(root, "adc3Cal", cal->voltage3);
  }
  // GPS fields
  cJSON_AddBoolToObject(root, "gpsPositionValid", gps.valid);
  if (gps.valid) {
    cJSON_AddNumberToObject(root, "gpsLat",      gps.latitude_deg);
    cJSON_AddNumberToObject(root, "gpsLon",      gps.longitude_deg);
    cJSON_AddNumberToObject(root, "gpsAlt",      gps.altitude_m);
    cJSON_AddNumberToObject(root, "gpsFixQuality", gps.fix_quality);
    cJSON_AddNumberToObject(root, "gpsSatellites", gps.satellites);
    cJSON_AddNumberToObject(root, "gpsFixAgeMs", static_cast<double>(gps.age_ms));
  }
  const char* json = cJSON_PrintUnformatted(root);
  if (json) {
    if (s_publish_fn) s_publish_fn(json);
  }
  cJSON_free((void*)json);
  cJSON_Delete(root);
}

// ---------- StopLogging ----------

void StopLogging() {
  if (!QueueCurrentLogForUpload()) {
    SdLockGuard guard;
    if (guard.locked()) {
      if (log_file) { fclose(log_file); log_file = nullptr; }
    } else if (log_file) {
      fclose(log_file);
      log_file = nullptr;
    }
  }
  UnmountLogSd();
  UpdateState([](SharedState& s) {
    s.logging        = false;
    s.log_filename.clear();
    s.stepper_abort  = true;
    s.stepper_moving = false;
  });
  log_config.active      = false;
  log_config.homed_once  = false;
  log_config.postfix.clear();
  log_config.file_start_us = 0;
  ErrorManagerClear(ErrorCode::kLogTaskStack);
  if (log_task) {
    vTaskDelete(log_task);
    log_task = nullptr;
  }
  DisableStepper();
}

// ---------- LoggingTask ----------

static void LoggingTask(void*) {
  constexpr int kGpsPositionTimeoutMs  = 1500;
  const TickType_t settle_delay        = pdMS_TO_TICKS(1000);
  constexpr UBaseType_t kLogStackLow   = 512;

  auto home_blocking = [&]() {
    return HomeStepperToUserZeroWithRetries(true, "Logging home", kStepperHomeRetryAttempts);
  };

  auto move_blocking = [&](int steps, bool forward) -> bool {
    int done = 0;
    UpdateState([&](SharedState& s) {
      s.homing                    = true;
      s.stepper_abort             = false;
      s.stepper_moving            = true;
      s.stepper_direction_forward = forward;
      s.stepper_target            = s.stepper_position + (forward ? steps : -steps);
    });
    EnableStepper();
    gpio_set_level(STEPPER_DIR, forward ? 1 : 0);
    const int step_delay_us = std::max(CopyState().stepper_speed_us, 1);
    for (int i = 0; i < steps; ++i) {
      if (CopyState().stepper_abort) {
        ESP_LOGW(kTag, "Logging move aborted after %d/%d steps", i, steps);
        break;
      }
      gpio_set_level(STEPPER_STEP, 1);
      esp_rom_delay_us(4);
      gpio_set_level(STEPPER_STEP, 0);
      esp_rom_delay_us(step_delay_us);
      done++;
    }
    UpdateState([&](SharedState& s) {
      s.homing         = false;
      s.stepper_moving = false;
      s.stepper_position += forward ? done : -done;
      s.stepper_target   = s.stepper_position;
    });
    return done == steps && !CopyState().stepper_abort;
  };

  auto collect_avg = [&](float duration_s, int temp_count, SharedState* out) -> bool {
    if (!out) return false;
    const TickType_t interval   = pdMS_TO_TICKS(200);
    const uint64_t duration_ms  = static_cast<uint64_t>(duration_s * 1000.0f);
    const uint64_t start        = esp_timer_get_time() / 1000ULL;
    int samples = 0;
    double sv1 = 0, sv2 = 0, sv3 = 0;
    std::array<double, MAX_TEMP_SENSORS> temp_sum{};
    double s_bus_v = 0, s_bus_i = 0, s_bus_p = 0;
    while ((esp_timer_get_time() / 1000ULL - start) < duration_ms) {
      SharedState snap = CopyState();
      if (log_config.use_motor && (snap.stepper_moving || snap.homing)) {
        ESP_LOGW(kTag, "Logging: stepper moved during averaging, discarding samples");
        return false;
      }
      sv1 += snap.voltage1; sv2 += snap.voltage2; sv3 += snap.voltage3;
      for (int i = 0; i < temp_count && i < MAX_TEMP_SENSORS; ++i) temp_sum[i] += snap.temps_c[i];
      s_bus_v += snap.ina_bus_voltage;
      s_bus_i += snap.ina_current;
      s_bus_p += snap.ina_power;
      samples++;
      vTaskDelay(interval);
    }
    if (samples == 0) return false;
    out->voltage1        = sv1 / samples;
    out->voltage2        = sv2 / samples;
    out->voltage3        = sv3 / samples;
    out->ina_bus_voltage = s_bus_v / samples;
    out->ina_current     = s_bus_i / samples;
    out->ina_power       = s_bus_p / samples;
    out->temp_sensor_count = temp_count;
    for (int i = 0; i < temp_count && i < MAX_TEMP_SENSORS; ++i)
      out->temps_c[i] = static_cast<float>(temp_sum[i] / samples);
    return true;
  };

  SharedState pending_base{};
  bool has_pending_base  = false;
  bool at_zero           = true;
  int  pending_steps     = 0;

  if (log_config.use_motor || !log_config.homed_once) {
    StepperHomeResult home_result = home_blocking();
    log_config.homed_once = true;
    if (!StepperHomeSucceeded(home_result)) {
      const std::string msg = StepperHomeFailureMessage("Logging stopped: homing",
                                                        home_result, kStepperHomeRetryAttempts);
      ESP_LOGW(kTag, "%s", msg.c_str());
      ErrorManagerSet(ErrorCode::kStepperHoming, ErrorSeverity::kError, msg);
      StopLogging();
      vTaskDelete(nullptr);
    }
  }

  while (true) {
    const UBaseType_t wm = uxTaskGetStackHighWaterMark(nullptr);
    if (wm > 0 && wm < kLogStackLow) {
      ErrorManagerSet(ErrorCode::kLogTaskStack, ErrorSeverity::kWarning, "log_task stack low");
      ESP_LOGW(kTag, "log_task stack low: %u words", static_cast<unsigned>(wm));
    } else {
      ErrorManagerClear(ErrorCode::kLogTaskStack);
    }

    SharedState current = CopyState();
    if (!current.logging || !log_file) {
      StopLogging();
      vTaskDelete(nullptr);
    }

    const uint64_t now_us = esp_timer_get_time();
    if (log_config.file_start_us > 0 && (now_us - log_config.file_start_us) >= 3'600'000'000ULL) {
      ESP_LOGI(kTag, "Rotating log file after 1 hour");
      (void)QueueCurrentLogForUpload();
      if (!OpenLogFileWithPostfix(log_config.postfix)) {
        StopLogging();
        vTaskDelete(nullptr);
      }
      continue;
    }

    if (log_config.use_motor) {
      if (at_zero) {
        vTaskDelay(settle_delay);
        SharedState avg{};
        if (!collect_avg(log_config.duration_s, log_config.temp_sensor_count, &avg)) {
          vTaskDelay(pdMS_TO_TICKS(500));
          continue;
        }
        pending_base   = avg;
        has_pending_base = true;
        pending_steps  = std::clamp(app_config.logging_motor_steps, 1, 20000);
        if (!move_blocking(pending_steps, true)) {
          ESP_LOGW(kTag, "Logging aborted during stepper move");
          StopLogging();
          vTaskDelete(nullptr);
        }
        at_zero = false;
        continue;
      }

      vTaskDelay(settle_delay);
      SharedState avg{};
      if (!collect_avg(log_config.duration_s, log_config.temp_sensor_count, &avg)) {
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }
      if (has_pending_base) {
        GpsPositionSnapshot gps{};
        (void)RequestGpsPositionOnce(kGpsPositionTimeoutMs, &gps);
        SdLockGuard guard(pdMS_TO_TICKS(2000));
        if (!guard.locked()) {
          vTaskDelay(pdMS_TO_TICKS(200));
          continue;
        }
        const UtcTimeSnapshot row_time = GetBestUtcTimeForData();
        const uint64_t ts_ms           = UtcTimeToUnixMs(row_time);
        const std::string iso          = FormatUtcIso(row_time);
        fprintf(log_file, "%s,%llu,%.6f,%.6f,%.6f", iso.c_str(), (unsigned long long)ts_ms,
                pending_base.voltage1, pending_base.voltage2, pending_base.voltage3);
        for (int i = 0; i < pending_base.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i)
          fprintf(log_file, ",%.2f", pending_base.temps_c[i]);
        fprintf(log_file, ",%.3f,%.3f,%.3f", pending_base.ina_bus_voltage, pending_base.ina_current, pending_base.ina_power);
        fprintf(log_file, ",%.6f,%.6f,%.6f", avg.voltage1, avg.voltage2, avg.voltage3);
        AppendGpsCsvFields(log_file, gps);
        fprintf(log_file, "\n");
        FlushLogFile();
        ESP_LOGD(kTag, "Logging: wrote row ts=%llu iso=%s", (unsigned long long)ts_ms, iso.c_str());
        PublishLogMeasurement(iso, ts_ms, pending_base, &avg, row_time.source, gps);
        UpdateState([&](SharedState& s) {
          s.voltage1_cal = avg.voltage1;
          s.voltage2_cal = avg.voltage2;
          s.voltage3_cal = avg.voltage3;
        });
      }

      if (app_config.logging_home_each_cycle) {
        StepperHomeResult hr = home_blocking();
        if (!StepperHomeSucceeded(hr)) {
          const std::string msg = StepperHomeFailureMessage("Logging stopped: return homing",
                                                             hr, kStepperHomeRetryAttempts);
          ESP_LOGW(kTag, "%s", msg.c_str());
          ErrorManagerSet(ErrorCode::kStepperHoming, ErrorSeverity::kError, msg);
          StopLogging();
          vTaskDelete(nullptr);
        }
      } else {
        const int ret = std::max(pending_steps, 1);
        if (!move_blocking(ret, false)) {
          ErrorManagerSet(ErrorCode::kStepperHoming, ErrorSeverity::kError,
                          "Logging stopped: step return failed");
          StopLogging();
          vTaskDelete(nullptr);
        }
        UpdateState([](SharedState& s) {
          s.stepper_position    = 0;
          s.stepper_target      = 0;
          s.stepper_homed       = true;
          s.stepper_home_status = "step_return_zero";
        });
        ErrorManagerClear(ErrorCode::kStepperHoming);
      }
      at_zero          = true;
      has_pending_base = false;
      pending_steps    = 0;
      continue;
    }

    // No motor — plain measurement
    SharedState avg1{};
    if (!collect_avg(log_config.duration_s, log_config.temp_sensor_count, &avg1)) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    GpsPositionSnapshot gps{};
    (void)RequestGpsPositionOnce(kGpsPositionTimeoutMs, &gps);

    SdLockGuard guard(pdMS_TO_TICKS(2000));
    if (!guard.locked()) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    const UtcTimeSnapshot row_time = GetBestUtcTimeForData();
    const uint64_t ts_ms           = UtcTimeToUnixMs(row_time);
    const std::string iso          = FormatUtcIso(row_time);
    fprintf(log_file, "%s,%llu,%.6f,%.6f,%.6f", iso.c_str(), (unsigned long long)ts_ms,
            avg1.voltage1, avg1.voltage2, avg1.voltage3);
    for (int i = 0; i < avg1.temp_sensor_count && i < MAX_TEMP_SENSORS; ++i)
      fprintf(log_file, ",%.2f", avg1.temps_c[i]);
    fprintf(log_file, ",%.3f,%.3f,%.3f", avg1.ina_bus_voltage, avg1.ina_current, avg1.ina_power);
    AppendGpsCsvFields(log_file, gps);
    fprintf(log_file, "\n");
    FlushLogFile();
    ESP_LOGD(kTag, "Logging: wrote row ts=%llu iso=%s", (unsigned long long)ts_ms, iso.c_str());
    PublishLogMeasurement(iso, ts_ms, avg1, nullptr, row_time.source, gps);
    UpdateState([&](SharedState& s) {
      s.voltage1_cal = avg1.voltage1;
      s.voltage2_cal = avg1.voltage2;
      s.voltage3_cal = avg1.voltage3;
    });
  }
}

// ---------- StartLoggingToFile ----------

bool StartLoggingToFile(const std::string& postfix_raw) {
  const std::string postfix = SanitizePostfix(postfix_raw);
  log_config.postfix      = postfix;
  log_config.active       = true;
  log_config.homed_once   = false;
  if (log_config.duration_s <= 0.0f) log_config.duration_s = 1.0f;

  if (!OpenLogFileWithPostfix(postfix)) {
    log_config.active = false;
    return false;
  }
  if (log_task == nullptr) {
    xTaskCreatePinnedToCore(&LoggingTask, "log_task", 12288, nullptr, 2, &log_task, 0);
  }
  ESP_LOGI(kTag, "Logging started");
  UpdateState([&](SharedState& s) {
    s.logging        = true;
    s.log_use_motor  = log_config.use_motor;
    s.log_duration_s = log_config.duration_s;
  });
  return true;
}
