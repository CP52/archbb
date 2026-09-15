#pragma once
#include <cstdint>
typedef struct { int max_freq_mhz; int min_freq_mhz; bool light_sleep_enable; } esp_pm_config_esp32s3_t;
int esp_pm_configure(const void*);
typedef int esp_err_t;
#define ESP_OK 0

typedef void* esp_pm_lock_handle_t;
typedef enum { ESP_PM_CPU_FREQ_MAX, ESP_PM_APB_FREQ_MAX, ESP_PM_NO_LIGHT_SLEEP } esp_pm_lock_type_t;
esp_err_t esp_pm_lock_create(esp_pm_lock_type_t, int, const char*, esp_pm_lock_handle_t*);
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t);
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t);
esp_err_t esp_pm_lock_delete(esp_pm_lock_handle_t);
