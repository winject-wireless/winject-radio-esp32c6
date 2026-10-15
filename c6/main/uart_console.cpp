#include "uart_console.h"

#include "sdio_endpoint.h"
#include "sdio_link_slave.h"
#include "wifi.h"

#include <atomic>
#include <stdlib.h>

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/lp_aon_reg.h"

static std::atomic<bool> s_mon{false};

struct mon_sample
{
    uint32_t air, submits, nomem, slot_waits, dropq, ether_in, link_rx;
};

static mon_sample sample()
{
    wifi& w = wifi::instance();
    sdio_link_slave_stats_t ls;
    sdio_link_slave_get_stats(&ls);
    return {w.tx().air_pkt(),         w.tx().submits(),
            w.tx().nomem(),           w.tx().slot_waits(),
            w.tx().dropped_tx_queue(), sdio_endpoint::instance().tx_ether_pkt(),
            ls.rx_msgs};
}

// Once a second: the inject path's counters as rates, queue state, heap, and
// the five busiest tasks (IDLE = free CPU).
static void mon_task(void* arg)
{
    (void)arg;
    constexpr int k_max = 32;
    static TaskStatus_t a[k_max], b[k_max];
    TaskStatus_t* prev = a;
    TaskStatus_t* cur = b;
    configRUN_TIME_COUNTER_TYPE prev_total = 0, cur_total = 0;
    UBaseType_t prev_n = uxTaskGetSystemState(prev, k_max, &prev_total);
    mon_sample last = sample();
    while (s_mon.load())
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const mon_sample now = sample();
        const UBaseType_t n = uxTaskGetSystemState(cur, k_max, &cur_total);
        const uint32_t dt = static_cast<uint32_t>(cur_total - prev_total);
        char top[160] = "";
        size_t len = 0;
        bool used[k_max] = {};
        for (int k = 0; k < 5 && dt > 0; k++)
        {
            int best = -1;
            uint32_t best_d = 0;
            for (UBaseType_t i = 0; i < n; i++)
            {
                if (used[i])
                {
                    continue;
                }
                uint32_t before = 0;
                for (UBaseType_t j = 0; j < prev_n; j++)
                {
                    if (prev[j].xHandle == cur[i].xHandle)
                    {
                        before = static_cast<uint32_t>(prev[j].ulRunTimeCounter);
                        break;
                    }
                }
                const uint32_t d = static_cast<uint32_t>(cur[i].ulRunTimeCounter) - before;
                if (best < 0 || d > best_d)
                {
                    best = static_cast<int>(i);
                    best_d = d;
                }
            }
            if (best < 0)
            {
                break;
            }
            used[best] = true;
            len += snprintf(top + len, sizeof(top) - len, " %s:%u", cur[best].pcTaskName,
                            static_cast<unsigned>(static_cast<uint64_t>(best_d) * 100 / dt));
            if (len >= sizeof(top))
            {
                break;
            }
        }
        wifi& w = wifi::instance();
        printf("M in=%lu air=%lu submit=%lu nomem=%lu slotwait=%lu dropq=%lu link_rx=%lu "
               "q=%u infl=%u heap=%u cpu%%%s\n",
               static_cast<unsigned long>(now.ether_in - last.ether_in),
               static_cast<unsigned long>(now.air - last.air),
               static_cast<unsigned long>(now.submits - last.submits),
               static_cast<unsigned long>(now.nomem - last.nomem),
               static_cast<unsigned long>(now.slot_waits - last.slot_waits),
               static_cast<unsigned long>(now.dropq - last.dropq),
               static_cast<unsigned long>(now.link_rx - last.link_rx),
               static_cast<unsigned>(w.tx().queue_size()),
               static_cast<unsigned>(w.tx().in_flight()),
               static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)), top);
        fflush(stdout);
        last = now;
        TaskStatus_t* t = prev;
        prev = cur;
        cur = t;
        prev_n = n;
        prev_total = cur_total;
    }
    vTaskDelete(nullptr);
}

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
    else if (strncmp(line, "mon ", 4) == 0)
    {
        const bool on = atoi(line + 4) != 0;
        if (on && !s_mon.exchange(true))
        {
            xTaskCreate(mon_task, "mon", 4096, nullptr, 3, nullptr);
        }
        else if (!on)
        {
            s_mon.store(false);
        }
        printf("OK mon=%d\n", on ? 1 : 0);
    }
    else if (line[0] != '\0')
    {
        printf("ERR unknown (dl, mon 0|1)\n");
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
