#include "kbd_task.h"

#include <stdint.h>
#include "kb.h"
#include "app_queues.h"

#define KBD_POLL_MS        20

/* Автоповтор (делаем в kbd task) */
#define REP_DELAY_MS       220
#define REP_RATE_MS         80

static inline uint32_t rtos_tick(void) {
#if (osCMSIS < 0x20000U)
    return osKernelSysTick();
#else
    return osKernelGetTickCount();
#endif
}

static void queue_send_u8(uint8_t v) {
#if (osCMSIS < 0x20000U)
    (void)osMessagePut(myKbdQueueHandle, (uint32_t)v, 0);
#else
    (void)osMessageQueuePut(myKbdQueueHandle, &v, 0, 0);
#endif
}

void Kbd_Task(void const *argument) {
    (void)argument;

    /* Инициализация клавиатуры */
    (void)Set_Keyboard();

    uint8_t last_key = 0;
    uint32_t key_down_tick = 0;
    uint32_t last_rep_tick = 0;

    for (;;) {
        uint32_t now = rtos_tick();
        uint8_t key = KBD_GetKey(); /* 0 если ничего */

        if (key != last_key) {
            last_key = key;
            key_down_tick = now;
            last_rep_tick = now;

            if (key != 0) {
                queue_send_u8(key);   /* событие нажатия */
            }
        } else {
            /* удержание => автоповтор для 4/6/5 */
            if (key == 4 || key == 6 || key == 5) {
                if ((now - key_down_tick) >= REP_DELAY_MS &&
                    (now - last_rep_tick) >= REP_RATE_MS) {
                    last_rep_tick = now;
                    queue_send_u8(key);
                }
            }
        }

        osDelay(KBD_POLL_MS);
    }
}
