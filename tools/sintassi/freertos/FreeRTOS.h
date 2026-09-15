#pragma once
#include <cstdint>
typedef void* SemaphoreHandle_t; typedef void* TaskHandle_t; typedef void* QueueHandle_t;
typedef uint32_t TickType_t; typedef int BaseType_t;
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFF
void vTaskDelay(TickType_t);
TickType_t xTaskGetTickCount();
void vTaskDelayUntil(TickType_t*, TickType_t);
#define configTICK_RATE_HZ 1000
BaseType_t xTaskCreatePinnedToCore(void(*)(void*),const char*,uint32_t,void*,int,TaskHandle_t*,int);
void vTaskSuspend(TaskHandle_t); void vTaskResume(TaskHandle_t); void vTaskDelete(TaskHandle_t);

// L'header FreeRTOS di ESP-IDF tira dentro anche le code: qui lo replichiamo,
// perche' il codice del progetto ci conta senza includere queue.h esplicitamente.
QueueHandle_t xQueueCreate(uint32_t len, uint32_t itemSize);
BaseType_t xQueueSend(QueueHandle_t, const void*, TickType_t);
BaseType_t xQueueReceive(QueueHandle_t, void*, TickType_t);
void vQueueDelete(QueueHandle_t);
uint32_t uxQueueMessagesWaiting(QueueHandle_t);
