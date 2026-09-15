#pragma once
#include "FreeRTOS.h"
QueueHandle_t xQueueCreate(uint32_t len, uint32_t itemSize);
BaseType_t xQueueSend(QueueHandle_t, const void*, TickType_t);
BaseType_t xQueueSendFromISR(QueueHandle_t, const void*, BaseType_t*);
BaseType_t xQueueReceive(QueueHandle_t, void*, TickType_t);
void vQueueDelete(QueueHandle_t);
uint32_t uxQueueMessagesWaiting(QueueHandle_t);
