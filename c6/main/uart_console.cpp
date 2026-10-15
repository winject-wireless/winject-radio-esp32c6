#include "uart_console.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/lp_aon_reg.h"

static void handle(char* line)
{
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, "dl") == 0)
    {
        // The strap override esptool uses: the ROM sees a forced download
        // boot after the software reset.
        printf("OK dl\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(20));
        REG_SET_BIT(LP_AON_SYS_CFG_REG, LP_AON_FORCE_DOWNLOAD_BOOT);
        esp_restart();
    }
    else if (line[0] != '\0')
    {
        printf("ERR unknown (dl)\n");
    }
}

static void task(void* arg)
{
    (void)arg;
    char line[64];
    for (;;)
    {
        if (fgets(line, sizeof(line), stdin) != nullptr)
        {
            handle(line);
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

void uart_console_start()
{
    if (!uart_is_driver_installed(static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM)))
    {
        uart_driver_install(static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM), 256, 0, 0,
                            nullptr, 0);
    }
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
    setvbuf(stdin, nullptr, _IONBF, 0);
    xTaskCreate(task, "uart_console", 3072, nullptr, 2, nullptr);
}
