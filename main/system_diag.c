/*
 * System Diagnostics Module
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"

char* system_diag_get_json(void)
{
    cJSON *root = cJSON_CreateObject();

    // Memory - Internal RAM
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    cJSON *ram = cJSON_CreateObject();
    cJSON_AddNumberToObject(ram, "free", internal_free);
    cJSON_AddNumberToObject(ram, "total", internal_total);
    cJSON_AddNumberToObject(ram, "min_free", internal_min);
    cJSON_AddNumberToObject(ram, "used_pct", internal_total > 0 ? (internal_total - internal_free) * 100 / internal_total : 0);
    cJSON_AddItemToObject(root, "ram", ram);

    // Memory - PSRAM
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

    cJSON *psram = cJSON_CreateObject();
    cJSON_AddNumberToObject(psram, "free", psram_free);
    cJSON_AddNumberToObject(psram, "total", psram_total);
    cJSON_AddNumberToObject(psram, "min_free", psram_min);
    if (psram_total > 0) {
        cJSON_AddNumberToObject(psram, "used_pct", (psram_total - psram_free) * 100 / psram_total);
    }
    cJSON_AddItemToObject(root, "psram", psram);

    // Task list with CPU usage
#if configUSE_TRACE_FACILITY && configGENERATE_RUN_TIME_STATS
    cJSON *tasks = cJSON_CreateArray();
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    TaskStatus_t *task_array = malloc(task_count * sizeof(TaskStatus_t));

    if (task_array) {
        uint32_t total_runtime;
        task_count = uxTaskGetSystemState(task_array, task_count, &total_runtime);

        for (int i = 0; i < task_count && i < 20; i++) {
            cJSON *task = cJSON_CreateObject();
            cJSON_AddStringToObject(task, "name", task_array[i].pcTaskName);
            cJSON_AddNumberToObject(task, "prio", task_array[i].uxCurrentPriority);
            cJSON_AddNumberToObject(task, "stack", task_array[i].usStackHighWaterMark);
            // core ID not available in this config
            uint32_t cpu = (total_runtime > 0) ? (task_array[i].ulRunTimeCounter * 100) / total_runtime : 0;
            cJSON_AddNumberToObject(task, "cpu", cpu);
            cJSON_AddItemToArray(tasks, task);
        }
        free(task_array);
    }
    cJSON_AddItemToObject(root, "tasks", tasks);
#endif

    cJSON_AddNumberToObject(root, "uptime_ms", (uint32_t)(esp_timer_get_time() / 1000));

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}
