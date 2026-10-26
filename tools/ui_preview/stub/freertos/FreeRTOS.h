// Host stand-ins for the bits of FreeRTOS the UI preview's code touches.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(m) ((void)(m))
#define taskEXIT_CRITICAL(m) ((void)(m))
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0xFFFFFFFFu
#define configMAX_PRIORITIES 25
typedef void *TaskHandle_t;
typedef struct hq *QueueHandle_t;
QueueHandle_t xQueueCreate(int len, int item);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t t);
void vTaskDelay(TickType_t t);
