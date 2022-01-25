#include "project_config.h"

#if CONFIG_THINGSPEAK_ENABLE

#include "reThingSpeak.h"
#include <cstring>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "rLog.h"
#include "rTypes.h"
#include "rStrings.h"
#include "esp_http_client.h"
#include "reWiFi.h"
#include "reEsp32.h"
#include "reEvents.h"
#include "reStates.h"
#include "sys/queue.h"
#include "def_consts.h"

#define API_THINGSPEAK_HOST "thingspeak.com"
#define API_THINGSPEAK_PORT 443
#define API_THINGSPEAK_SEND_PATH "/update"
#define API_THINGSPEAK_SEND_VALUES "api_key=%s&%s"

extern const char api_thingspeak_com_pem_start[] asm(CONFIG_THINGSPEAK_TLS_PEM_START);
extern const char api_thingspeak_com_pem_end[]   asm(CONFIG_THINGSPEAK_TLS_PEM_END); 

typedef enum {
  TS_OK         = 0,
  TS_ERROR_API  = 1,
  TS_ERROR_HTTP = 2
} tsSendStatus_t;

typedef struct tsChannel_t {
  const char* key;
  uint32_t interval;
  TickType_t next_send;
  uint8_t attempt;
  char* data;
  SLIST_ENTRY(tsChannel_t) next;
} tsChannel_t;
typedef struct tsChannel_t *tsChannelHandle_t;

SLIST_HEAD(tsHead_t, tsChannel_t);
typedef struct tsHead_t *tsHeadHandle_t;

typedef struct {
  const char* key;
  char* data;
} tsQueueItem_t;  

#define THINGSPEAK_QUEUE_ITEM_SIZE sizeof(tsQueueItem_t*)

TaskHandle_t _tsTask;
QueueHandle_t _tsQueue = nullptr;
tsHeadHandle_t _tsChannels = nullptr;

static const char* logTAG = "ThSp";
static const char* tsTaskName = "thing_speak";

#if CONFIG_THINGSPEAK_STATIC_ALLOCATION
StaticQueue_t _tsQueueBuffer;
StaticTask_t _tsTaskBuffer;
StackType_t _tsTaskStack[CONFIG_THINGSPEAK_STACK_SIZE];
uint8_t _tsQueueStorage [CONFIG_THINGSPEAK_QUEUE_SIZE * THINGSPEAK_QUEUE_ITEM_SIZE];
#endif // CONFIG_THINGSPEAK_STATIC_ALLOCATION

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Channel list ------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool tsChannelsInit()
{
  _tsChannels = (tsHead_t*)esp_calloc(1, sizeof(tsHead_t));
  if (_tsChannels) {
    SLIST_INIT(_tsChannels);
  };
  return (_tsChannels);
}

bool tsChannelInit(const char * tsKey, const uint32_t tsInterval)
{
  if (!_tsChannels) {
    tsChannelsInit();
  };

  if (!_tsChannels) {
    rlog_e(logTAG, "The channel list has not been initialized!");
    return false;
  };
    
  tsChannelHandle_t ctrl = (tsChannel_t*)esp_calloc(1, sizeof(tsChannel_t));
  if (!ctrl) {
    rlog_e(logTAG, "Memory allocation error for data structure!");
    return false;
  };

  ctrl->key = tsKey;
  if (tsInterval < CONFIG_THINGSPEAK_MIN_INTERVAL) {
    ctrl->interval = pdMS_TO_TICKS(CONFIG_THINGSPEAK_MIN_INTERVAL);
  } else {
    ctrl->interval = pdMS_TO_TICKS(tsInterval);
  };
  ctrl->next_send = 0;
  ctrl->attempt = 0;
  ctrl->data = nullptr;
  SLIST_NEXT(ctrl, next) = nullptr;
  SLIST_INSERT_HEAD(_tsChannels, ctrl, next);
  return true;
}

tsChannelHandle_t tsChannelFind(const char * tsKey)
{
  tsChannelHandle_t item;
  SLIST_FOREACH(item, _tsChannels, next) {
    if (strcasecmp(item->key, tsKey) == 0) {
      return item;
    }
  }
  return nullptr;
} 

void tsChannelsFree()
{
  tsChannelHandle_t item, tmp;
  SLIST_FOREACH_SAFE(item, _tsChannels, next, tmp) {
    SLIST_REMOVE(_tsChannels, item, tsChannel_t, next);
    if (item->data) free(item->data);
    free(item);
  };
  free(_tsChannels);
  _tsChannels = nullptr;
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------ Adding data to the send queue ----------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool tsSend(const char * tsKey, char * tsFields)
{
  if (_tsQueue) {
    tsQueueItem_t* item = (tsQueueItem_t*)esp_calloc(1, sizeof(tsQueueItem_t));
    if (item) {
      item->key = tsKey;
      item->data = tsFields;
      if (xQueueSend(_tsQueue, &item, CONFIG_THINGSPEAK_QUEUE_WAIT / portTICK_RATE_MS) == pdPASS) {
        return true;
      };
    };
    rloga_e("Error adding message to queue [ %s ]!", tsTaskName);
    eventLoopPostSystem(RE_SYS_THINGSPEAK_ERROR, RE_SYS_SET, false);
  };
  return false;
}

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- Call API --------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

tsSendStatus_t tsSendEx(const tsChannelHandle_t ctrl)
{
  tsSendStatus_t _result = TS_OK;
  char* get_request = nullptr;

  // Create the text of the GET request
  if (ctrl->data) {
    get_request = malloc_stringf(API_THINGSPEAK_SEND_VALUES, ctrl->key, ctrl->data);
  };

  if (get_request) {
    // Configuring request parameters
    esp_http_client_config_t cfgHttp;
    memset(&cfgHttp, 0, sizeof(cfgHttp));
    cfgHttp.method = HTTP_METHOD_GET;
    cfgHttp.host = API_THINGSPEAK_HOST;
    cfgHttp.port = API_THINGSPEAK_PORT;
    cfgHttp.path = API_THINGSPEAK_SEND_PATH;
    cfgHttp.query = get_request;
    cfgHttp.use_global_ca_store = false;
    cfgHttp.transport_type = HTTP_TRANSPORT_OVER_SSL;
    cfgHttp.cert_pem = api_thingspeak_com_pem_start;
    cfgHttp.skip_cert_common_name_check = false;
    cfgHttp.is_async = false;

    // Make a request to the API
    esp_http_client_handle_t client = esp_http_client_init(&cfgHttp);
    if (client != NULL) {
      esp_err_t err = esp_http_client_perform(client);
      if (err == ESP_OK) {
        int retCode = esp_http_client_get_status_code(client);
        if ((retCode == 200) || (retCode == 301)) {
          _result = TS_OK;
          rlog_i(logTAG, "Data sent to %s: %s", ctrl->key, get_request);
        } else {
          _result = TS_ERROR_API;
          rlog_e(logTAG, "Failed to send message, API error code: #%d!", retCode);
        };
        // Flashing system LED
        ledSysActivity();
      }
      else {
        _result = TS_ERROR_HTTP;
        rlog_e(logTAG, "Failed to complete request to thingspeak.com, error code: 0x%x!", err);
      };
      esp_http_client_cleanup(client);
    }
    else {
      _result = TS_ERROR_HTTP;
      rlog_e(logTAG, "Failed to complete request to thingspeak.com!");
    };
  };
  // Remove the request from memory
  if (get_request) free(get_request);
  return _result;
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Queue processing --------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

void tsTaskExec(void *pvParameters)
{
  tsQueueItem_t * item = nullptr;
  tsChannelHandle_t ctrl = nullptr;
  TickType_t wait_queue = portMAX_DELAY;
  static tsSendStatus_t send_status = TS_ERROR_HTTP;
  static uint32_t send_errors = 0;
  static time_t time_first_error = 0;
  
  while (true) {
    // Receiving new data
    if (xQueueReceive(_tsQueue, &item, wait_queue) == pdPASS) {
      ctrl = tsChannelFind(item->key);
      if (ctrl) {
        // Replacing channel data with new ones from the transporter
        if (item->data) {
          ctrl->attempt = 0;
          if (ctrl->data) free(ctrl->data);
          ctrl->data = item->data;
          item->data = nullptr;
        };
      } else {
        rlog_e(logTAG, "Channel # %s not found!", item->key);
      };
      // Free transporter
      if (item->data) free(item->data);
      free(item);
      item = nullptr;
    };

    // Check internet availability 
    if (statesInetIsAvailabled()) {
      ctrl = nullptr;
      wait_queue = portMAX_DELAY;
      SLIST_FOREACH(ctrl, _tsChannels, next) {
        // Sending data
        if (ctrl->data) {
          if (ctrl->next_send < xTaskGetTickCount()) {
            // Attempt to send data
            ctrl->attempt++;
            send_status = tsSendEx(ctrl);
            if (send_status == TS_OK) {
              // Calculate the time of the next dispatch in the given channel
              ctrl->next_send = xTaskGetTickCount() + ctrl->interval;
              ctrl->attempt = 0;
              if (ctrl->data) {
                free(ctrl->data);
                ctrl->data = nullptr;
              };
              // If the error counter exceeds the threshold, then a notification has been sent - send a recovery notification
              if (send_errors >= CONFIG_THINGSPEAK_ERROR_LIMIT) {
                eventLoopPostSystem(RE_SYS_THINGSPEAK_ERROR, RE_SYS_CLEAR, false, time_first_error);
              };
              time_first_error = 0;
              send_errors = 0;
            } else {
              // Increase the number of errors in a row and fix the time of the first error
              send_errors++;
              if (time_first_error == 0) {
                time_first_error = time(nullptr);
              };
              // Calculate the time of the next dispatch in the given controller
              ctrl->next_send = xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_THINGSPEAK_ERROR_INTERVAL);
              if (ctrl->attempt >= CONFIG_THINGSPEAK_MAX_ATTEMPTS) {
                ctrl->attempt = 0;
                if (ctrl->data) {
                  free(ctrl->data);
                  ctrl->data = nullptr;
                };
                rlog_e(logTAG, "Failed to send data to channel #%s!", ctrl->key);
              };
              // If the error counter has reached the threshold, send a notification
              if (send_errors == CONFIG_THINGSPEAK_ERROR_LIMIT) {
                eventLoopPostSystem(RE_SYS_THINGSPEAK_ERROR, RE_SYS_SET, false, time_first_error);
              };
            };
          };

          // Find the minimum delay before the next sending to the channel
          if (ctrl->data) {
            TickType_t send_delay = 0;
            if (ctrl->next_send > xTaskGetTickCount()) {
              send_delay = ctrl->next_send - xTaskGetTickCount();
            };
            if (send_delay < wait_queue) {
              wait_queue = send_delay;
            };
          };
        };
      };
    } else {
      // If the Internet is not available, repeat the check every second
      wait_queue = pdMS_TO_TICKS(1000); 
    };
  };
  tsTaskDelete();
}

// -----------------------------------------------------------------------------------------------------------------------
// -------------------------------------------------- Task routines ------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool tsTaskSuspend()
{
  if ((_tsTask) && (eTaskGetState(_tsTask) != eSuspended)) {
    vTaskSuspend(_tsTask);
    if (eTaskGetState(_tsTask) == eSuspended) {
      rloga_d("Task [ %s ] has been suspended", tsTaskName);
      return true;
    } else {
      rloga_e("Failed to suspend task [ %s ]!", tsTaskName);
    };
  };
  return false;  
}

bool tsTaskResume()
{
  if ((_tsTask) && (eTaskGetState(_tsTask) == eSuspended)) {
    vTaskResume(_tsTask);
    if (eTaskGetState(_tsTask) != eSuspended) {
      rloga_i("Task [ %s ] has been successfully resumed", tsTaskName);
      return true;
    } else {
      rloga_e("Failed to resume task [ %s ]!", tsTaskName);
    };
  };
  return false;  
}

bool tsTaskCreate(bool createSuspended) 
{
  if (!_tsTask) {
    if (!_tsChannels) {
      if (!tsChannelsInit()) {
        rloga_e("Failed to create a list of channels!");
        eventLoopPostSystem(RE_SYS_ERROR, RE_SYS_SET, false);
        return false;
      };
    };

    if (!_tsQueue) {
      #if CONFIG_THINGSPEAK_STATIC_ALLOCATION
      _tsQueue = xQueueCreateStatic(CONFIG_THINGSPEAK_QUEUE_SIZE, THINGSPEAK_QUEUE_ITEM_SIZE, &(_tsQueueStorage[0]), &_tsQueueBuffer);
      #else
      _tsQueue = xQueueCreate(CONFIG_THINGSPEAK_QUEUE_SIZE, THINGSPEAK_QUEUE_ITEM_SIZE);
      #endif // CONFIG_THINGSPEAK_STATIC_ALLOCATION
      if (!_tsQueue) {
        tsChannelsFree();
        rloga_e("Failed to create a queue for sending data to ThingSpeak!");
        eventLoopPostSystem(RE_SYS_ERROR, RE_SYS_SET, false);
        return false;
      };
    };
    
    #if CONFIG_THINGSPEAK_STATIC_ALLOCATION
    _tsTask = xTaskCreateStaticPinnedToCore(tsTaskExec, tsTaskName, CONFIG_THINGSPEAK_STACK_SIZE, NULL, CONFIG_THINGSPEAK_PRIORITY, _tsTaskStack, &_tsTaskBuffer, CONFIG_THINGSPEAK_CORE); 
    #else
    xTaskCreatePinnedToCore(tsTaskExec, tsTaskName, CONFIG_THINGSPEAK_STACK_SIZE, NULL, CONFIG_THINGSPEAK_PRIORITY, &_tsTask, CONFIG_THINGSPEAK_CORE); 
    #endif // CONFIG_THINGSPEAK_STATIC_ALLOCATION
    if (_tsTask == NULL) {
      vQueueDelete(_tsQueue);
      tsChannelsFree();
      rloga_e("Failed to create task for sending data to ThingSpeak!");
      eventLoopPostSystem(RE_SYS_ERROR, RE_SYS_SET, false);
      return false;
    }
    else {
      if (createSuspended) {
        rloga_i("Task [ %s ] has been successfully created", tsTaskName);
        tsTaskSuspend();
        return tsEventHandlerRegister();
      } else {
        rloga_i("Task [ %s ] has been successfully started", tsTaskName);
        eventLoopPostSystem(RE_SYS_THINGSPEAK_ERROR, RE_SYS_CLEAR, false);
        return true;
      };
    };
  };
  return false;
}

bool tsTaskDelete()
{
  tsChannelsFree();

  if (_tsQueue != NULL) {
    vQueueDelete(_tsQueue);
    _tsQueue = NULL;
  };

  if (_tsTask != NULL) {
    vTaskDelete(_tsTask);
    _tsTask = NULL;
    rloga_d("Task [ %s ] was deleted", tsTaskName);
  };
  
  return true;
}

// -----------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------- Events handlers ---------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static void tsWiFiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if (event_id == RE_WIFI_STA_PING_OK) {
    if (!_tsTask) {
      tsTaskCreate(false);
    };
  };
}

static void tsOtaEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if ((event_id == RE_SYS_OTA) && (event_data)) {
    re_system_event_data_t* data = (re_system_event_data_t*)event_data;
    if (data->type == RE_SYS_SET) {
      tsTaskSuspend();
    } else {
      tsTaskResume();
    };
  };
}

bool tsEventHandlerRegister()
{
  return eventHandlerRegister(RE_WIFI_EVENTS, RE_WIFI_STA_PING_OK, &tsWiFiEventHandler, nullptr)
      && eventHandlerRegister(RE_SYSTEM_EVENTS, RE_SYS_OTA, &tsOtaEventHandler, nullptr);
};

#endif // CONFIG_THINGSPEAK_ENABLE
