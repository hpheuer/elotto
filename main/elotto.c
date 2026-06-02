// ==================================================================
//  elotto.c  –  app_main
// ==================================================================
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "sensor.h"

void app_main(void)
{
    nvs_flash_init();
    vTaskDelay(pdMS_TO_TICKS(500));  // kurz warten bis UART bereit
    elotto_run(NUM_RUNS);
}
