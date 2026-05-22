#include "bell_controller.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "hal/gpio_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "bell";
static SemaphoreHandle_t s_ring_mutex = NULL;

esp_err_t bell_controller_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BELL_GPIO_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    gpio_set_level(BELL_GPIO_PIN, 0);
    s_ring_mutex = xSemaphoreCreateBinary();
    xSemaphoreGive(s_ring_mutex);
    ESP_LOGI(TAG, "Bell GPIO %d initialized", BELL_GPIO_PIN);
    return err;
}

static void ring_task(void *arg)
{
    uint32_t duration_ms = (uint32_t)(uintptr_t)arg;
    gpio_set_level(BELL_GPIO_PIN, 1);
    ESP_LOGI(TAG, "Bell ON for %lu ms", (unsigned long)duration_ms);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    gpio_set_level(BELL_GPIO_PIN, 0);
    ESP_LOGI(TAG, "Bell OFF");
    xSemaphoreGive(s_ring_mutex);
    vTaskDelete(NULL);
}

esp_err_t bell_controller_ring(uint32_t duration_ms)
{
    if (duration_ms == 0) {
        duration_ms = BELL_DEFAULT_DURATION_MS;
    }
    if (duration_ms > BELL_MAX_DURATION_MS) {
        ESP_LOGW(TAG, "Duration %lu ms exceeds cap, clamping to %d ms",
                 (unsigned long)duration_ms, BELL_MAX_DURATION_MS);
        duration_ms = BELL_MAX_DURATION_MS;
    }
    if (xSemaphoreTake(s_ring_mutex, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Bell already ringing, ignoring");
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t ret = xTaskCreate(ring_task, "bell_ring", 4096,
                                 (void *)(uintptr_t)duration_ms, 5, NULL);
    if (ret != pdPASS) {
        xSemaphoreGive(s_ring_mutex);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void bell_controller_panic_off(void)
{
    /* Direct register write — safe to call from panic context (no RTOS) */
    gpio_ll_set_level(&GPIO, BELL_GPIO_PIN, 0);
}
