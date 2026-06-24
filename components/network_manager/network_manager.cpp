#include "network_manager.h"

#include <cstring>
#include <string>

#include "app_state.h"
#include "app_utils.h"
#include "error_manager.h"
#include "hw_pins.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_efuse.h"
#include "esp_err.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static constexpr char kTag[] = "NET";

#include "sensor_hub.h"

// ---------- module constants ----------

static constexpr time_t kValidUtcThreshold = 1'700'000'000;  // ~2023-11-14
static constexpr char   kHostname[]        = "miap-device";
static constexpr bool   kUseCustomMac      = true;
static uint8_t          s_custom_mac[6]    = {0x10, 0x00, 0x3B, 0x6E, 0x83, 0x70};
static constexpr int    kWifiConnectedBit  = BIT0;
static constexpr int    kWifiFailBit       = BIT1;

// ---------- module state ----------

static EventGroupHandle_t s_wifi_event_group         = nullptr;
static int                s_retry_count               = 0;
static bool               s_time_synced               = false;
static bool               s_netif_inited              = false;
static bool               s_sntp_started              = false;
static bool               s_sntp_time_available       = false;
static int64_t            s_last_sntp_sync_us         = 0;
static bool               s_wifi_inited               = false;
static esp_netif_t*       s_wifi_netif_sta            = nullptr;
static esp_netif_t*       s_wifi_netif_ap             = nullptr;
static TaskHandle_t       s_wifi_recover_task         = nullptr;
static TaskHandle_t       s_eth_preferred_fb_task     = nullptr;
static TaskHandle_t       s_config_ap_fb_task         = nullptr;
static bool               s_fallback_ap_active        = false;
static bool               s_wifi_recover_active       = false;
static wifi_config_t      s_sta_cfg_cached            = {};
static int64_t            s_last_wifi_connect_us      = 0;
static bool               s_eth_inited                = false;
static bool               s_eth_started               = false;
static bool               s_eth_handlers_registered   = false;
static esp_eth_handle_t   s_eth_handle                = nullptr;
static esp_netif_t*       s_eth_netif                 = nullptr;
static esp_eth_mac_t*     s_eth_mac                   = nullptr;
static esp_eth_phy_t*     s_eth_phy                   = nullptr;
static spi_device_interface_config_t s_eth_devcfg     = {};

// ---------- forward declarations ----------

void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void StartWifiRecoverTask(int interval_ms);
static void StopWifiRecoverTask();
static void StartEthPreferredWifiFallbackTask(int delay_ms);
static void StopEthPreferredWifiFallbackTask();
static bool StartConfigAp();
static void StartConfigApFallbackTask(int delay_ms);
static void StopConfigApFallbackTask();
static bool StartEthernet();
static esp_err_t EnsureWifiInit();
static void StopWifiInterface(bool clear_ap_ip);
static bool RequestWifiConnect(const char* reason);
static void ReadNetworkUpFlags(bool* wifi_up, bool* eth_up);
static void UpdateDefaultNetif();
static std::string GetNetifIp(esp_netif_t* netif);
static std::string FormatIp4(const esp_ip4_addr_t& ip);
static void MarkSntpSynced();
static void MarkSntpUnavailableIfNoNetwork();
static bool AnyNetworkHasIp();

// ---------- helpers ----------

static std::string FormatIp4(const esp_ip4_addr_t& ip) {
  if (ip.addr == 0) return "";
  char buf[16];
  std::snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip));
  return std::string(buf);
}

static std::string GetNetifIp(esp_netif_t* netif) {
  if (!netif) return "";
  esp_netif_ip_info_t info{};
  if (esp_netif_get_ip_info(netif, &info) != ESP_OK) return "";
  return FormatIp4(info.ip);
}

static void EnsureNetifInit() {
  if (!s_netif_inited) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif_inited = true;
  }
}

static void ReadNetworkUpFlags(bool* wifi_up, bool* eth_up) {
  bool wifi = false;
  bool eth = false;
  if (state_mutex && xSemaphoreTake(state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    wifi = !state.wifi_ip_sta.empty();
    eth = state.eth_ip_up || !state.eth_ip.empty();
    xSemaphoreGive(state_mutex);
  } else {
    wifi = !state.wifi_ip_sta.empty();
    eth = state.eth_ip_up || !state.eth_ip.empty();
  }
  if (wifi_up) *wifi_up = wifi;
  if (eth_up)  *eth_up  = eth;
}

static bool AnyNetworkHasIp() {
  bool wifi_up = false;
  bool eth_up  = false;
  ReadNetworkUpFlags(&wifi_up, &eth_up);
  return wifi_up || eth_up;
}

static void MarkSntpSynced() {
  s_time_synced = true;
  s_sntp_time_available = true;
  s_last_sntp_sync_us = esp_timer_get_time();
}

static void MarkSntpUnavailableIfNoNetwork() {
  if (!AnyNetworkHasIp()) s_sntp_time_available = false;
}

bool IsSntpUsable() {
  time_t now = time(nullptr);
  if (now <= kValidUtcThreshold) return false;
  if (!s_sntp_time_available) return false;
  if (!AnyNetworkHasIp()) return false;
  return true;
}

static void UpdateDefaultNetif() {
  esp_netif_t* target = nullptr;
  const NetMode mode = app_config.net_mode;
  const NetPriority prio = app_config.net_priority;
  bool wifi_up = false;
  bool eth_up = false;
  ReadNetworkUpFlags(&wifi_up, &eth_up);

  if (mode == NetMode::kWifiOnly) {
    if (s_wifi_netif_sta) target = s_wifi_netif_sta;
  } else if (mode == NetMode::kEthOnly) {
    if (s_eth_netif) target = s_eth_netif;
  } else {
    if (prio == NetPriority::kWifi) {
      if (wifi_up && s_wifi_netif_sta)  target = s_wifi_netif_sta;
      else if (eth_up && s_eth_netif)   target = s_eth_netif;
    } else {
      if (eth_up && s_eth_netif)        target = s_eth_netif;
      else if (wifi_up && s_wifi_netif_sta) target = s_wifi_netif_sta;
    }
  }
  if (target) esp_netif_set_default_netif(target);
}

static void EnsureConfiguredNetworkProgress() {
  bool wifi_up = false;
  bool eth_up = false;
  ReadNetworkUpFlags(&wifi_up, &eth_up);

  if (app_config.net_mode == NetMode::kWifiOnly) {
    if (!s_wifi_inited) {
      InitWifi(app_config.wifi_ssid, app_config.wifi_password, app_config.wifi_ap_mode);
    } else if (!wifi_up && !s_fallback_ap_active && !s_wifi_recover_task) {
      StartWifiRecoverTask(15000);
    }
    return;
  }
  if (app_config.net_mode == NetMode::kEthOnly) {
    if (!s_eth_started) StartEthernet();
    if (!eth_up && !s_fallback_ap_active && !s_config_ap_fb_task) StartConfigApFallbackTask(15000);
    return;
  }
  if (!s_eth_started) StartEthernet();
  if (app_config.net_priority == NetPriority::kEth) {
    if (!eth_up && !wifi_up && !s_wifi_recover_task && !s_fallback_ap_active && !s_eth_preferred_fb_task) {
      StartEthPreferredWifiFallbackTask(15000);
    }
  } else if (s_wifi_inited && !wifi_up && !s_fallback_ap_active && !s_wifi_recover_task) {
    StartWifiRecoverTask(15000);
  }
}

static bool TrySyncTimeOnNetif(esp_netif_t* netif, int timeout_ms) {
  if (!netif || timeout_ms <= 0) return false;
  esp_netif_t* prev = esp_netif_get_default_netif();
  if (prev != netif) esp_netif_set_default_netif(netif);

  esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  if (!esp_sntp_restart()) StartSntp();

  const int step_ms  = 200;
  const int max_steps = std::max(1, timeout_ms / step_ms);
  bool ok = false;
  for (int i = 0; i < max_steps; ++i) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = 0;
      time(&now);
      if (now > kValidUtcThreshold) { ok = true; break; }
    }
    vTaskDelay(pdMS_TO_TICKS(step_ms));
  }
  if (ok) MarkSntpSynced(); else s_sntp_time_available = false;
  if (prev && prev != netif) esp_netif_set_default_netif(prev);
  return ok;
}

// ---------- network monitor task ----------

static void NetworkMonitorTask(void*) {
  const int interval_ms      = 15000;
  const int check_timeout_ms = 1500;
  while (true) {
    UpdateState([](SharedState& s) {
      s.heap_free_bytes = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_8BIT));
      s.heap_min_free_bytes = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
      s.heap_largest_free_block_bytes = static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      s.heap_internal_free_bytes = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      s.heap_internal_largest_free_block_bytes =
          static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      s.heap_psram_free_bytes = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      s.heap_psram_largest_free_block_bytes =
          static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    });
    EnsureConfiguredNetworkProgress();
    if (app_config.net_mode == NetMode::kWifiEth) {
      bool wifi_up = false, eth_up = false;
      ReadNetworkUpFlags(&wifi_up, &eth_up);
      esp_netif_t* prefer = (app_config.net_priority == NetPriority::kWifi) ? s_wifi_netif_sta : s_eth_netif;
      esp_netif_t* other  = (prefer == s_wifi_netif_sta) ? s_eth_netif : s_wifi_netif_sta;
      bool prefer_has_ip = (prefer == s_wifi_netif_sta) ? wifi_up : eth_up;
      bool other_has_ip  = (other  == s_wifi_netif_sta) ? wifi_up : eth_up;
      bool prefer_ok = false, other_ok = false;
      if (prefer && prefer_has_ip) prefer_ok = TrySyncTimeOnNetif(prefer, check_timeout_ms);
      if (!prefer_ok && other && other_has_ip) other_ok = TrySyncTimeOnNetif(other, check_timeout_ms);
      if (prefer_ok && prefer)      esp_netif_set_default_netif(prefer);
      else if (other_ok && other)   esp_netif_set_default_netif(other);
      else { s_sntp_time_available = false; UpdateDefaultNetif(); }
    } else {
      UpdateDefaultNetif();
      bool wifi_up = false, eth_up = false;
      ReadNetworkUpFlags(&wifi_up, &eth_up);
      esp_netif_t* netif  = nullptr;
      bool has_ip = false;
      if (app_config.net_mode == NetMode::kWifiOnly)       { netif = s_wifi_netif_sta; has_ip = wifi_up; }
      else if (app_config.net_mode == NetMode::kEthOnly)   { netif = s_eth_netif;      has_ip = eth_up;  }
      if (netif && has_ip) { if (!TrySyncTimeOnNetif(netif, check_timeout_ms)) s_sntp_time_available = false; }
      else                  s_sntp_time_available = false;
    }
    vTaskDelay(pdMS_TO_TICKS(interval_ms));
  }
}

// ---------- Ethernet ----------

void EthEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  if (event_base == ETH_EVENT) {
    if (event_id == ETHERNET_EVENT_START) {
      ESP_LOGI(kTag, "Ethernet started");
    } else if (event_id == ETHERNET_EVENT_CONNECTED) {
      ESP_LOGI(kTag, "Ethernet link up");
      UpdateState([](SharedState& s) { s.eth_link_up = true; });
    } else if (event_id == ETHERNET_EVENT_DISCONNECTED) {
      ESP_LOGW(kTag, "Ethernet link down");
      UpdateState([](SharedState& s) {
        s.eth_link_up = false;
        s.eth_ip.clear();
        s.eth_ip_up = false;
      });
      UpdateDefaultNetif();
      if (app_config.net_mode == NetMode::kEthOnly)
        StartConfigApFallbackTask(5000);
      else if (app_config.net_mode == NetMode::kWifiEth && app_config.net_priority == NetPriority::kEth)
        StartEthPreferredWifiFallbackTask(5000);
    } else if (event_id == ETHERNET_EVENT_STOP) {
      ESP_LOGI(kTag, "Ethernet stopped");
      UpdateState([](SharedState& s) { s.eth_link_up = false; s.eth_ip.clear(); s.eth_ip_up = false; });
      UpdateDefaultNetif();
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);
    const std::string ip = FormatIp4(event->ip_info.ip);
    ESP_LOGI(kTag, "Ethernet got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    UpdateState([&](SharedState& s) { s.eth_ip = ip; s.eth_ip_up = !ip.empty(); });
    StopEthPreferredWifiFallbackTask();
    StopConfigApFallbackTask();
    if (s_wifi_inited &&
        (app_config.net_mode == NetMode::kEthOnly ||
         (app_config.net_mode == NetMode::kWifiEth && app_config.net_priority == NetPriority::kEth))) {
      StopWifiInterface(true);
    }
    UpdateDefaultNetif();
  }
}

static esp_err_t InitEthernet() {
#if !CONFIG_ETH_SPI_ETHERNET_W5500
  ESP_LOGE(kTag, "W5500 support not enabled in sdkconfig");
  return ESP_ERR_NOT_SUPPORTED;
#else
  if (s_eth_inited) return ESP_OK;
  EnsureNetifInit();
  ESP_LOGI(kTag, "Initializing W5500 (CS=%d INT=%d RST=%d)",
           static_cast<int>(ETH_CS), static_cast<int>(ETH_INT), static_cast<int>(ETH_RST));
  esp_err_t ret = InitSpiBus();
  if (ret != ESP_OK) {
    ESP_LOGE(kTag, "SPI bus init for Ethernet failed: %s", esp_err_to_name(ret));
    return ret;
  }
  gpio_set_level(ETH_CS, 1);
  gpio_set_level(ETH_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(10));

  s_eth_devcfg = {};
  s_eth_devcfg.clock_speed_hz = ETH_SPI_FREQ_HZ;
  s_eth_devcfg.spics_io_num   = ETH_CS;
  s_eth_devcfg.queue_size     = 20;

  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
  phy_config.reset_gpio_num = ETH_RST;

  eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &s_eth_devcfg);
  w5500_config.int_gpio_num  = ETH_INT;
  w5500_config.poll_period_ms = 0;

  s_eth_mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
  s_eth_phy = esp_eth_phy_new_w5500(&phy_config);
  if (!s_eth_mac || !s_eth_phy) {
    ESP_LOGE(kTag, "Failed to create W5500 MAC/PHY");
    if (s_eth_mac) { s_eth_mac->del(s_eth_mac); s_eth_mac = nullptr; }
    if (s_eth_phy) { s_eth_phy->del(s_eth_phy); s_eth_phy = nullptr; }
    gpio_set_level(ETH_CS, 1);
    gpio_set_level(ETH_RST, 0);
    return ESP_FAIL;
  }

  esp_eth_config_t config = ETH_DEFAULT_CONFIG(s_eth_mac, s_eth_phy);
  esp_err_t install_err = esp_eth_driver_install(&config, &s_eth_handle);
  if (install_err != ESP_OK) {
    ESP_LOGE(kTag, "Ethernet driver install failed: %s", esp_err_to_name(install_err));
    return install_err;
  }

  uint8_t base_mac[6] = {}, eth_addr[6] = {};
  esp_err_t mac_err = esp_efuse_mac_get_default(base_mac);
  if (mac_err == ESP_OK) mac_err = esp_derive_local_mac(eth_addr, base_mac);
  if (mac_err == ESP_OK) mac_err = esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, eth_addr);
  if (mac_err != ESP_OK) {
    ESP_LOGE(kTag, "Ethernet MAC setup failed: %s", esp_err_to_name(mac_err));
    return mac_err;
  }

  esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
  s_eth_netif = esp_netif_new(&cfg);
  ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth_handle)));
  if (strlen(kHostname) > 0) esp_netif_set_hostname(s_eth_netif, kHostname);
  if (!s_eth_handlers_registered) {
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &EthEventHandler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &EthEventHandler, nullptr));
    s_eth_handlers_registered = true;
  }
  s_eth_inited = true;
  ESP_LOGI(kTag, "W5500 Ethernet initialized");
  return ESP_OK;
#endif
}

static bool StartEthernet() {
  if (s_eth_started) return true;
  if (InitEthernet() != ESP_OK) return false;
  ESP_LOGI(kTag, "Starting Ethernet");
  esp_err_t err = esp_eth_start(s_eth_handle);
  if (err == ESP_OK) { s_eth_started = true; return true; }
  ESP_LOGE(kTag, "Ethernet start failed: %s", esp_err_to_name(err));
  return false;
}

static void StopEthernet() {
  if (!s_eth_started || !s_eth_handle) return;
  esp_eth_stop(s_eth_handle);
  s_eth_started = false;
  UpdateState([](SharedState& s) { s.eth_link_up = false; s.eth_ip.clear(); s.eth_ip_up = false; });
  UpdateDefaultNetif();
}

// ---------- ApplyNetworkConfig ----------

void ApplyNetworkConfig() {
  const NetMode mode = app_config.net_mode;
  ESP_LOGI(kTag, "Applying network config: mode=%s priority=%s",
           NetModeToString(mode).c_str(), NetPriorityToString(app_config.net_priority).c_str());
  if (mode == NetMode::kWifiOnly) {
    StopConfigApFallbackTask();
    StopEthPreferredWifiFallbackTask();
    StopEthernet();
    InitWifi(app_config.wifi_ssid, app_config.wifi_password, app_config.wifi_ap_mode);
  } else if (mode == NetMode::kEthOnly) {
    StopConfigApFallbackTask();
    StopEthPreferredWifiFallbackTask();
    StopWifiInterface(true);
    const bool eth_start_ok = StartEthernet();
    StartConfigApFallbackTask(eth_start_ok ? 15000 : 1000);
  } else {
    StopConfigApFallbackTask();
    if (app_config.net_priority == NetPriority::kEth) {
      StopWifiInterface(false);
      const bool eth_start_ok = StartEthernet();
      StartEthPreferredWifiFallbackTask(eth_start_ok ? 15000 : 1000);
    } else {
      StopEthPreferredWifiFallbackTask();
      StartEthernet();
      InitWifi(app_config.wifi_ssid, app_config.wifi_password, app_config.wifi_ap_mode);
    }
  }
  UpdateDefaultNetif();
}

// ---------- Wi-Fi recovery tasks ----------

static void WifiRecoverTask(void* arg) {
  const int interval_ms = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  while (s_wifi_recover_active) {
    RequestWifiConnect("recover");
    vTaskDelay(pdMS_TO_TICKS(interval_ms));
  }
  vTaskDelete(nullptr);
}

static void StartWifiRecoverTask(int interval_ms) {
  if (s_wifi_recover_task) return;
  s_wifi_recover_active = true;
  xTaskCreatePinnedToCore(&WifiRecoverTask, "wifi_recover", 2048,
                          reinterpret_cast<void*>(static_cast<intptr_t>(interval_ms)),
                          1, &s_wifi_recover_task, 0);
}

static void StopWifiRecoverTask() {
  if (!s_wifi_recover_task) return;
  s_wifi_recover_active = false;
  TaskHandle_t t = s_wifi_recover_task;
  s_wifi_recover_task = nullptr;
  vTaskDelete(t);
}

static void EthPreferredWifiFallbackTask(void* arg) {
  const int delay_ms = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  vTaskDelay(pdMS_TO_TICKS(delay_ms));
  bool wifi_up = false, eth_up = false;
  ReadNetworkUpFlags(&wifi_up, &eth_up);
  s_eth_preferred_fb_task = nullptr;
  if (app_config.net_mode == NetMode::kWifiEth && app_config.net_priority == NetPriority::kEth && !eth_up && !wifi_up)
    InitWifi(app_config.wifi_ssid, app_config.wifi_password, app_config.wifi_ap_mode);
  vTaskDelete(nullptr);
}

static void StartEthPreferredWifiFallbackTask(int delay_ms) {
  if (s_eth_preferred_fb_task) return;
  xTaskCreatePinnedToCore(&EthPreferredWifiFallbackTask, "eth_wifi_fb", 4096,
                          reinterpret_cast<void*>(static_cast<intptr_t>(delay_ms)),
                          1, &s_eth_preferred_fb_task, 0);
}

static void StopEthPreferredWifiFallbackTask() {
  if (!s_eth_preferred_fb_task) return;
  TaskHandle_t t = s_eth_preferred_fb_task;
  s_eth_preferred_fb_task = nullptr;
  vTaskDelete(t);
}

static void ConfigApFallbackTask(void* arg) {
  const int delay_ms = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  vTaskDelay(pdMS_TO_TICKS(delay_ms));
  bool wifi_up = false, eth_up = false;
  ReadNetworkUpFlags(&wifi_up, &eth_up);
  s_config_ap_fb_task = nullptr;
  if (!wifi_up && !eth_up) { ESP_LOGW(kTag, "No IP after %d ms, starting cfg AP", delay_ms); StartConfigAp(); }
  vTaskDelete(nullptr);
}

static void StartConfigApFallbackTask(int delay_ms) {
  if (s_config_ap_fb_task || s_fallback_ap_active) return;
  xTaskCreatePinnedToCore(&ConfigApFallbackTask, "cfg_ap_fb", 4096,
                          reinterpret_cast<void*>(static_cast<intptr_t>(delay_ms)),
                          1, &s_config_ap_fb_task, 0);
}

static void StopConfigApFallbackTask() {
  if (!s_config_ap_fb_task) return;
  TaskHandle_t t = s_config_ap_fb_task;
  s_config_ap_fb_task = nullptr;
  vTaskDelete(t);
}

// ---------- Wi-Fi helpers ----------

static esp_err_t EnsureWifiInit() {
  if (!s_wifi_event_group) s_wifi_event_group = xEventGroupCreate();
  if (s_wifi_inited) return ESP_OK;
  EnsureNetifInit();
  s_wifi_netif_sta = esp_netif_create_default_wifi_sta();
  s_wifi_netif_ap  = esp_netif_create_default_wifi_ap();
  if (s_wifi_netif_sta && strlen(kHostname) > 0) esp_netif_set_hostname(s_wifi_netif_sta, kHostname);
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), kTag, "Wi-Fi init failed");
  ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler, nullptr),
                      kTag, "Wi-Fi event handler register failed");
  ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiEventHandler, nullptr),
                      kTag, "Wi-Fi IP event handler register failed");
  s_wifi_inited = true;
  return ESP_OK;
}

static void StopWifiInterface(bool clear_ap_ip) {
  StopWifiRecoverTask();
  StopConfigApFallbackTask();
  s_fallback_ap_active   = false;
  s_last_wifi_connect_us = 0;
  ErrorManagerClearLocal(ErrorCode::kWifiFallback);
  if (s_wifi_inited) { esp_wifi_stop(); esp_wifi_set_mode(WIFI_MODE_NULL); }
  UpdateState([&](SharedState& s) {
    s.wifi_ip.clear();
    s.wifi_ip_sta.clear();
    if (clear_ap_ip) s.wifi_ip_ap.clear();
    s.wifi_rssi_dbm = -127;
    s.wifi_quality  = 0;
  });
}

static bool RequestWifiConnect(const char* reason) {
  if (!s_wifi_inited) return false;
  const int64_t now_us = esp_timer_get_time();
  if (s_last_wifi_connect_us != 0 && now_us - s_last_wifi_connect_us < 2000000) return false;
  s_last_wifi_connect_us = now_us;
  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "Wi-Fi connect failed (%s): %s", reason ? reason : "", esp_err_to_name(err));
    return false;
  }
  return true;
}

static void FillApConfig(wifi_config_t* ap_config, const char* ssid, const char* password) {
  if (!ap_config) return;
  *ap_config = {};
  std::strncpy(reinterpret_cast<char*>(ap_config->ap.ssid), ssid, sizeof(ap_config->ap.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(ap_config->ap.password), password, sizeof(ap_config->ap.password) - 1);
  ap_config->ap.ssid_len      = strlen(reinterpret_cast<char*>(ap_config->ap.ssid));
  ap_config->ap.channel       = 1;
  ap_config->ap.authmode      = (strlen(password) >= 8) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
  ap_config->ap.max_connection = 4;
}

static bool StartConfigAp() {
  StopWifiRecoverTask();
  StopConfigApFallbackTask();
  if (s_fallback_ap_active && s_wifi_inited) return true;

  if (!s_wifi_event_group) s_wifi_event_group = xEventGroupCreate();
  else xEventGroupClearBits(s_wifi_event_group, kWifiConnectedBit | kWifiFailBit);

  if (EnsureWifiInit() != ESP_OK) return false;
  esp_wifi_stop();
  wifi_config_t ap_config = {};
  FillApConfig(&ap_config, "esp", "12345678");

  if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK) return false;
  if (esp_wifi_set_config(WIFI_IF_AP, &ap_config) != ESP_OK) return false;
  if (esp_wifi_start() != ESP_OK) return false;
  esp_wifi_set_ps(WIFI_PS_NONE);

  s_fallback_ap_active = true;
  ErrorManagerSetLocal(ErrorCode::kWifiFallback, ErrorSeverity::kWarning, "Configuration AP active");
  const std::string ap_ip = GetNetifIp(s_wifi_netif_ap);
  ESP_LOGW(kTag, "Configuration AP active: SSID=esp IP=%s", ap_ip.empty() ? "192.168.4.1" : ap_ip.c_str());
  UpdateState([&](SharedState& s) {
    s.wifi_ip_ap = ap_ip.empty() ? "192.168.4.1" : ap_ip;
    s.wifi_ip    = s.wifi_ip_ap;
    s.wifi_ip_sta.clear();
    s.wifi_rssi_dbm = -127;
    s.wifi_quality  = 0;
  });
  return true;
}

static bool EnableFallbackAp() {
  if (app_config.wifi_ap_mode || s_fallback_ap_active) return false;
  ESP_LOGW(kTag, "Starting fallback AP and continuing STA retries");
  s_fallback_ap_active = true;
  ErrorManagerSetLocal(ErrorCode::kWifiFallback, ErrorSeverity::kWarning, "Fallback AP active");

  wifi_config_t ap_config = {};
  FillApConfig(&ap_config, "esp", "12345678");
  esp_err_t err = esp_wifi_disconnect();
  if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT)
    ESP_LOGW(kTag, "Wi-Fi disconnect before fallback AP failed: %s", esp_err_to_name(err));

  err = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (err != ESP_OK) { s_fallback_ap_active = false; return false; }
  err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
  if (err != ESP_OK) { s_fallback_ap_active = false; return false; }

  const std::string ap_ip = GetNetifIp(s_wifi_netif_ap);
  UpdateState([&](SharedState& s) { if (!ap_ip.empty()) s.wifi_ip_ap = ap_ip; });
  return true;
}

// ---------- Wi-Fi event handler ----------

void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    RequestWifiConnect("sta_start");
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    bool eth_up = false;
    ReadNetworkUpFlags(nullptr, &eth_up);
    if (app_config.net_mode == NetMode::kWifiEth && app_config.net_priority == NetPriority::kEth && eth_up) {
      StopWifiRecoverTask();
      UpdateState([](SharedState& s) { s.wifi_ip.clear(); s.wifi_ip_sta.clear(); });
      return;
    }
    std::string reason_msg = "Wi-Fi disconnected";
    if (event_data) {
      auto* info = static_cast<wifi_event_sta_disconnected_t*>(event_data);
      reason_msg += " reason=" + std::to_string(info->reason);
    }
    ErrorManagerSetLocal(ErrorCode::kWifiDisconnected, ErrorSeverity::kWarning, reason_msg);
    UpdateState([](SharedState& s) { s.wifi_ip.clear(); s.wifi_ip_sta.clear(); });
    UpdateDefaultNetif();
    if (s_retry_count < 5) {
      if (RequestWifiConnect("event_retry")) {
        s_retry_count++;
        ESP_LOGW(kTag, "Retry Wi-Fi connection (%d)", s_retry_count);
      }
    } else {
      xEventGroupSetBits(s_wifi_event_group, kWifiFailBit);
      EnableFallbackAp();
      StartWifiRecoverTask(15000);
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    s_retry_count          = 0;
    s_last_wifi_connect_us = 0;
    ErrorManagerClearLocal(ErrorCode::kWifiDisconnected);
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);
    ESP_LOGI(kTag, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    const std::string sta_ip = FormatIp4(event->ip_info.ip);
    const std::string ap_ip  = GetNetifIp(s_wifi_netif_ap);
    UpdateState([&](SharedState& s) {
      s.wifi_ip = sta_ip; s.wifi_ip_sta = sta_ip;
      if (!ap_ip.empty()) s.wifi_ip_ap = ap_ip;
    });
    UpdateDefaultNetif();
    xEventGroupSetBits(s_wifi_event_group, kWifiConnectedBit);
    StopWifiRecoverTask();
    if (s_fallback_ap_active && !app_config.wifi_ap_mode) {
      s_fallback_ap_active = false;
      ErrorManagerClearLocal(ErrorCode::kWifiFallback);
      esp_wifi_set_mode(WIFI_MODE_STA);
    }
  }
}

// ---------- Wi-Fi monitor task ----------

static void WifiMonitorTask(void*) {
  while (true) {
    wifi_ap_record_t ap{};
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    int rssi    = (err == ESP_OK) ? ap.rssi : -127;
    int quality = (err == ESP_OK) ? RssiToQuality(rssi) : 0;
    UpdateState([&](SharedState& s) { s.wifi_rssi_dbm = rssi; s.wifi_quality = quality; });
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// ---------- InitWifi ----------

void InitWifi(const std::string& ssid, const std::string& password, bool ap_mode) {
  StopConfigApFallbackTask();
  StopWifiRecoverTask();
  s_fallback_ap_active   = false;
  s_last_wifi_connect_us = 0;

  if (!s_wifi_event_group) s_wifi_event_group = xEventGroupCreate();
  else xEventGroupClearBits(s_wifi_event_group, kWifiConnectedBit | kWifiFailBit);

  const bool was_inited = s_wifi_inited;
  ESP_ERROR_CHECK(EnsureWifiInit());
  if (was_inited) esp_wifi_stop();

  s_retry_count = 0;

  wifi_config_t wifi_config = {};
  std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password), password.c_str(), sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
  wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg = {.capable = true, .required = false};

  wifi_config_t ap_config = {};
  const char* ap_ssid = ap_mode ? ssid.c_str() : "esp";
  const char* ap_pass = ap_mode ? password.c_str() : "12345678";
  FillApConfig(&ap_config, ap_ssid, ap_pass);

  const wifi_mode_t mode = ap_mode ? WIFI_MODE_APSTA : WIFI_MODE_STA;
  s_sta_cfg_cached = wifi_config;
  ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
  if (kUseCustomMac) ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, s_custom_mac));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  if (mode == WIFI_MODE_APSTA) ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  const std::string ap_ip = GetNetifIp(s_wifi_netif_ap);
  UpdateState([&](SharedState& s) {
    if (!ap_ip.empty()) { s.wifi_ip_ap = ap_ip; if (ap_mode) s.wifi_ip = ap_ip; }
  });

  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                          kWifiConnectedBit | kWifiFailBit,
                                          pdFALSE, pdFALSE, pdMS_TO_TICKS(15'000));
  if (bits & kWifiConnectedBit) {
    ESP_LOGI(kTag, "Connected to SSID:%s", ssid.c_str());
  } else {
    ESP_LOGW(kTag, "Failed to connect to SSID:%s", ssid.c_str());
    xEventGroupSetBits(s_wifi_event_group, kWifiFailBit);
    EnableFallbackAp();
    StartWifiRecoverTask(15000);
  }
}

// ---------- SNTP / time ----------

void StartSntp() {
  if (s_sntp_started) return;
  s_sntp_started = true;
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();
}

static bool WaitForTimeSyncMs(int timeout_ms) {
  const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
  while (esp_timer_get_time() < deadline_us) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = 0;
      time(&now);
      if (now > kValidUtcThreshold) { MarkSntpSynced(); return true; }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  MarkSntpUnavailableIfNoNetwork();
  return false;
}

bool EnsureTimeSynced(int timeout_ms) {
  if (s_time_synced) return true;
  return WaitForTimeSyncMs(timeout_ms);
}

// ---------- task start ----------

void StartNetworkTasks() {
  xTaskCreatePinnedToCore(&NetworkMonitorTask, "net_mon",  5120, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(&WifiMonitorTask,   "wifi_mon", 2048, nullptr, 1, nullptr, 0);
}
