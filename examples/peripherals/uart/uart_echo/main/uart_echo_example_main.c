
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "string.h"
#include "hal/usb_serial_jtag_ll.h"
#include "freertos/queue.h"


#define BUF_SIZE (1024)
#define RD_BUF_SIZE (BUF_SIZE)
#define ASYNC_QUEUE_SIZE 100
static QueueHandle_t usj_queue;

int usj_event;

void mytask1(void *pvParameter)
{
    while (1)
    {
        if (!uxQueueMessagesWaiting(usj_queue) && usb_serial_jtag_ll_rxfifo_data_available() )
        {
        usj_event = 42;
        if (xQueueSendToBack(usj_queue, (void * )&usj_event, 0)!= pdPASS ){
            printf("U_S_J Q full\n");
        }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}

void mytask2(void *pvParameter)
{


    while (1)
    {
        printf("♥\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
        printf("*\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}

void mytask3(void *pvParameter)
{
    uint8_t *rxbuf;
    int cnt;
    rxbuf = (uint8_t *)malloc(64);
    int rxcnt;
    while (1)
    {
        if (xQueueReceive(usj_queue, (void*)&usj_event, 0))
        {
            rxcnt = usb_serial_jtag_ll_read_rxfifo(rxbuf,64);
            cnt = (int)usb_serial_jtag_ll_write_txfifo((const uint8_t *)rxbuf, rxcnt);
            usb_serial_jtag_ll_txfifo_flush();
            // printf("Event %d\n", usj_event);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    free(rxbuf);
    vTaskDelete(NULL);
}
void app_main(void)
{
    usj_queue = xQueueCreate(ASYNC_QUEUE_SIZE, sizeof(usj_event));
    printf("U_S_J Q free spaces: %d\n", uxQueueSpacesAvailable(usj_queue));

    xTaskCreate(mytask1, "mytask1", 1024 * 5, NULL, 1, NULL);
    xTaskCreate(mytask2, "mytask2", 1024 * 5, NULL, 1, NULL);
    xTaskCreate(mytask3, "mytask3", 1024 * 5, NULL, 1, NULL);

}
