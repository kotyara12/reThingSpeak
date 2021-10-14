/* 
   EN: Module for sending data to thingspeak.com from ESP32
   RU: Модуль для отправки данных на thingspeak.com из ESP32
   --------------------------
   (с) 2020-2021 Разживин Александр | Razzhivin Alexander
   kotyara12@yandex.ru | https://kotyara12.ru | tg: @kotyara1971
*/

#ifndef __RE_THINGSPEAK_H__
#define __RE_THINGSPEAK_H__

#include <stddef.h>
#include <sys/types.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h" 
#include "freertos/semphr.h" 
#include "project_config.h"
#include "def_consts.h"

#ifdef __cplusplus
extern "C" {
#endif

bool tsChannelsInit();

/**
 * EN: Task management: create, suspend, resume and delete
 * RU: Управление задачей: создание, приостановка, восстановление и удаление
 **/
bool tsTaskCreate(bool createSuspended);
bool tsTaskSuspend();
bool tsTaskResume();
bool tsTaskDelete();

/**
 * EN: Adding a new channel to the list
 * RU: Добавление нового канала в список
 * 
 * @param tsKey - Channel token / Токен контроллера
 * @param tsInterval - Minimal interval / Минимальный интервал
 **/
bool tsChannelInit(const char * tsKey, const uint32_t tsInterval);

/**
 * EN: Sending data to the specified channel. The fields string will be removed after submission.
 * If little time has passed since the last data sent to the channel, the data will be queued.
 * If there is already data in the queue for this channel, it will be overwritten with new data.
 * 
 * RU: Отправка данных в заданный контроллер. Строка fields будет удалена после отправки. 
 * Если с момента последней отправки данных в контроллер прошло мало времени, то данные будут поставлены в очередь.
 * Если в очереди на данный контроллер уже есть данные, то они будут перезаписаны новыми данными.
 * 
 * @param tsId - Channel ID / Идентификатор контроллера
 * @param tsFields - Data in the format p1=... / Данные в формате p1=...
 **/
bool tsSend(const uint32_t tsId, char * tsFields);

/**
 * EN: Registering event handlers in the main event loop
 * 
 * RU: Регистрация обработчиков событий в главном цикле событий
 **/
bool tsEventHandlerRegister();

#ifdef __cplusplus
}
#endif

#endif // __RE_THINGSPEAK_H__
