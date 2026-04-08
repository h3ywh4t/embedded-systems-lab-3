/*
 * tetris_task.c
 *
 *  Created on: Dec 22, 2025
 *      Author: zinch
 */


#include "tetris_task.h"

#include <stdint.h>
#include <string.h>

#include "oled.h"
#include "app_queues.h"
#include "rng.h"

/* -------------------- Поле -------------------- */
#define BOARD_W        10
#define BOARD_H        20

#define CELL_PX        6
#define CELL_FILL      5

#define FIELD_X        2
#define FIELD_Y        2

#define FIELD_W_PX     (BOARD_W * CELL_PX)
#define FIELD_H_PX     (BOARD_H * CELL_PX)

#define BORDER_X1      (FIELD_X - 1)
#define BORDER_Y1      (FIELD_Y - 1)
#define BORDER_X2      (FIELD_X + FIELD_W_PX)
#define BORDER_Y2      (FIELD_Y + FIELD_H_PX)

/* -------------------- Тайминги -------------------- */
#define LOOP_DELAY_MS          20

#define FALL_MS_START          600
#define FALL_MS_MIN            100
#define FALL_MS_LEVEL_STEP     50
#define LINES_PER_LEVEL        10

/* -------------------- Tick -------------------- */
static inline uint32_t rtos_tick(void) {
#if (osCMSIS < 0x20000U)
    return osKernelSysTick();
#else
    return osKernelGetTickCount();
#endif
}

/* -------------------- Очередь: принять 1 байт -------------------- */
static inline uint8_t queue_try_recv_u8(uint8_t *out) {
#if (osCMSIS < 0x20000U)
    osEvent ev = osMessageGet(myKbdQueueHandle, 0);
    if (ev.status == osEventMessage) {
        *out = (uint8_t)(ev.value.v);
        return 1;
    }
    return 0;
#else
    return (osMessageQueueGet(myKbdQueueHandle, out, NULL, 0) == osOK) ? 1 : 0;
#endif
}

/* -------------------- RNG -------------------- */
static uint32_t rng_state = 0x12345678u;
static uint32_t rng_next(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static uint32_t hw_rand_u32(void)
{
    uint32_t v = 0;
    if (HAL_RNG_GenerateRandomNumber(&hrng, &v) == HAL_OK) {
        return v;
    }
    return osKernelSysTick() ^ 0xA5A5A5A5u;
}

/* -------------------- Тетромино -------------------- */
typedef enum {
    PIECE_I = 0, PIECE_J, PIECE_L, PIECE_O, PIECE_S, PIECE_T, PIECE_Z, PIECE_COUNT
} piece_t;

static const uint16_t PIECES[PIECE_COUNT][4] = {
    { 0x0F00, 0x2222, 0x00F0, 0x4444 }, /* I */
    { 0x8E00, 0x6440, 0x0E20, 0x44C0 }, /* J */
    { 0x2E00, 0x4460, 0x0E80, 0xC440 }, /* L */
    { 0x6600, 0x6600, 0x6600, 0x6600 }, /* O */
    { 0x6C00, 0x4620, 0x06C0, 0x8C40 }, /* S */
    { 0x0E40, 0x4C40, 0x4E00, 0x4640 }, /* T */
    { 0xC600, 0x2640, 0x0C60, 0x4C80 }  /* Z */
};

static inline uint8_t mask_cell(uint16_t mask, uint8_t x, uint8_t y) {
    return (mask & (0x8000u >> (y * 4u + x))) ? 1u : 0u;
}

/* -------------------- Состояние игры -------------------- */
static uint8_t board[BOARD_H][BOARD_W];

typedef struct {
    uint8_t type;
    uint8_t rot;
    int8_t  x;
    int8_t  y;
} active_piece_t;

static active_piece_t cur;

static uint32_t score = 0;
static uint32_t lines_total = 0;

static uint32_t fall_ms = FALL_MS_START;
static uint32_t last_fall_tick = 0;

/* -------------------- Utils -------------------- */
static void u32_to_dec(char *dst, uint32_t v) {
    char tmp[11];
    uint8_t i = 0;

    if (v == 0) {
        dst[0] = '0';
        dst[1] = '\0';
        return;
    }

    while (v > 0 && i < sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    for (uint8_t j = 0; j < i; j++) {
        dst[j] = tmp[i - 1u - j];
    }
    dst[i] = '\0';
}

static void set_fall_speed_from_level(void) {
    uint32_t level = (lines_total / LINES_PER_LEVEL);
    int32_t ms = (int32_t)FALL_MS_START - (int32_t)level * (int32_t)FALL_MS_LEVEL_STEP;
    if (ms < (int32_t)FALL_MS_MIN) ms = FALL_MS_MIN;
    fall_ms = (uint32_t)ms;
}

static void board_clear(void) {
    memset(board, 0, sizeof(board));
}

/* -------------------- Коллизии -------------------- */
static uint8_t collides(uint8_t type, uint8_t rot, int8_t px, int8_t py) {
    uint16_t m = PIECES[type][rot & 3u];

    for (uint8_t y = 0; y < 4; y++) {
        for (uint8_t x = 0; x < 4; x++) {
            if (!mask_cell(m, x, y)) continue;

            int16_t bx = (int16_t)px + (int16_t)x;
            int16_t by = (int16_t)py + (int16_t)y;

            if (bx < 0 || bx >= BOARD_W) return 1;
            if (by < 0 || by >= BOARD_H) return 1;
            if (board[by][bx]) return 1;
        }
    }
    return 0;
}

/* -------------------- Игровые действия -------------------- */
static void spawn_piece(void) {
    cur.type = (uint8_t)(hw_rand_u32() % PIECE_COUNT);
    cur.rot  = 0;
    cur.x    = (int8_t)((BOARD_W - 4) / 2);
    cur.y    = 0;

    /* "сразу падает" */
    last_fall_tick = rtos_tick() - fall_ms;

    /* проигрыш => новая игра */
    if (collides(cur.type, cur.rot, cur.x, cur.y)) {
        board_clear();
        score = 0;
        lines_total = 0;
        set_fall_speed_from_level();

        cur.type = (uint8_t)(hw_rand_u32() % PIECE_COUNT);
        cur.rot  = 0;
        cur.x    = (int8_t)((BOARD_W - 4) / 2);
        cur.y    = 0;
        last_fall_tick = rtos_tick() - fall_ms;
    }
}

static void lock_piece(void) {
    uint16_t m = PIECES[cur.type][cur.rot & 3u];
    for (uint8_t y = 0; y < 4; y++) {
        for (uint8_t x = 0; x < 4; x++) {
            if (!mask_cell(m, x, y)) continue;

            int16_t bx = (int16_t)cur.x + (int16_t)x;
            int16_t by = (int16_t)cur.y + (int16_t)y;

            if (bx >= 0 && bx < BOARD_W && by >= 0 && by < BOARD_H) {
                board[by][bx] = 1;
            }
        }
    }
}

static uint8_t clear_lines(void) {
    uint8_t cleared = 0;

    for (int16_t y = BOARD_H - 1; y >= 0; y--) {
        uint8_t full = 1;
        for (uint8_t x = 0; x < BOARD_W; x++) {
            if (!board[y][x]) { full = 0; break; }
        }

        if (full) {
            cleared++;
            for (int16_t yy = y; yy > 0; yy--) {
                memcpy(board[yy], board[yy - 1], BOARD_W);
            }
            memset(board[0], 0, BOARD_W);
            y++; /* проверить строку снова */
        }
    }

    if (cleared) {
        lines_total += cleared;
        switch (cleared) {
            case 1: score += 100; break;
            case 2: score += 300; break;
            case 3: score += 500; break;
            default: score += 800; break;
        }
        set_fall_speed_from_level();
    }

    return cleared;
}

static uint8_t try_move(int8_t dx, int8_t dy) {
    int8_t nx = (int8_t)(cur.x + dx);
    int8_t ny = (int8_t)(cur.y + dy);
    if (!collides(cur.type, cur.rot, nx, ny)) {
        cur.x = nx;
        cur.y = ny;
        return 1;
    }
    return 0;
}

static uint8_t try_rotate(void) {
    uint8_t nr = (uint8_t)((cur.rot + 1u) & 3u);
    if (!collides(cur.type, nr, cur.x, cur.y)) {
        cur.rot = nr;
        return 1;
    }
    return 0; /* wall kick не делаем */
}

static void hard_drop(void) {
    while (try_move(0, 1)) { }
    lock_piece();
    clear_lines();
    spawn_piece();
}

/* -------------------- Рендер -------------------- */
static void draw_filled_cell(uint8_t cx, uint8_t cy) {
    uint8_t px = (uint8_t)(FIELD_X + cx * CELL_PX);
    uint8_t py = (uint8_t)(FIELD_Y + cy * CELL_PX);

    for (uint8_t dy = 0; dy < CELL_FILL; dy++) {
        for (uint8_t dx = 0; dx < CELL_FILL; dx++) {
            oled_DrawPixel((uint8_t)(px + dx), (uint8_t)(py + dy), White);
        }
    }
}

static void render(void) {
    oled_Fill(Black);

    oled_DrawSquare(BORDER_X1, BORDER_X2, BORDER_Y1, BORDER_Y2, White);

    for (uint8_t y = 0; y < BOARD_H; y++) {
        for (uint8_t x = 0; x < BOARD_W; x++) {
            if (board[y][x]) draw_filled_cell(x, y);
        }
    }

    {
        uint16_t m = PIECES[cur.type][cur.rot & 3u];
        for (uint8_t y = 0; y < 4; y++) {
            for (uint8_t x = 0; x < 4; x++) {
                if (!mask_cell(m, x, y)) continue;

                int16_t bx = (int16_t)cur.x + (int16_t)x;
                int16_t by = (int16_t)cur.y + (int16_t)y;

                if (bx >= 0 && bx < BOARD_W && by >= 0 && by < BOARD_H) {
                    draw_filled_cell((uint8_t)bx, (uint8_t)by);
                }
            }
        }
    }

    /* Score: правый верх */
    {
        char num[12];
        char s[16];

        u32_to_dec(num, score);

        s[0] = 'S'; s[1] = ':'; s[2] = '\0';
        strncat(s, num, sizeof(s) - 3);

        uint8_t len = (uint8_t)strlen(s);
        uint8_t wpx = (uint8_t)(len * Font_7x10.FontWidth);
        uint8_t sx = (wpx <= OLED_WIDTH) ? (uint8_t)(OLED_WIDTH - wpx) : 0;

        oled_SetCursor(sx, 0);
        oled_WriteString(s, Font_7x10, White);
    }

    oled_UpdateScreen();
}

/* -------------------- Главная задача тетриса -------------------- */
void Tetris_Task(void const *argument) {
    (void)argument;

    /* OLED init внутри задачи => задержки/работа после старта планировщика */
    (void)oled_Init();

    rng_state ^= (rtos_tick() | 1u) ^ 0xA5A5A5A5u;

    board_clear();
    score = 0;
    lines_total = 0;
    set_fall_speed_from_level();
    spawn_piece();

    render();

    for (;;) {
        uint32_t now = rtos_tick();
        uint8_t dirty = 0;

        /* 1) обработать ВСЕ события клавиш из очереди */
        uint8_t key;
        while (queue_try_recv_u8(&key)) {
            if (key == 2) {                /* left */
                if (try_move(-1, 0)) dirty = 1;
            } else if (key == 8) {         /* right */
                if (try_move(1, 0)) dirty = 1;
            } else if (key == 6) {         /* rotate */
                if (try_rotate()) dirty = 1;
            } else if (key == 5) {         /* soft drop */
                if (!try_move(0, 1)) {
                    lock_piece();
                    clear_lines();
                    spawn_piece();
                }
                last_fall_tick = now;
                dirty = 1;
            } else if (key == 4) {         /* hard drop */
                hard_drop();
                dirty = 1;
            }
        }

        /* 2) гравитация */
        if ((now - last_fall_tick) >= fall_ms) {
            last_fall_tick = now;

            if (!try_move(0, 1)) {
                lock_piece();
                clear_lines();
                spawn_piece();
            }
            dirty = 1;
        }

        if (dirty) {
            render();
        }

        osDelay(LOOP_DELAY_MS);
    }
}
