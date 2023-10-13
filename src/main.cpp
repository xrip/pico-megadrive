/* See LICENSE file for license details */

/* Standard library includes */

#include <pico/stdlib.h>
#include <hardware/clocks.h>
#include <pico/multicore.h>
#include "hardware/vreg.h"
#include "hardware/flash.h"

/* Gwenesis Emulator */
extern "C" {
#include "gwenesis/cpus/M68K/m68k.h"
#include "gwenesis/sound/z80inst.h"
#include "gwenesis/bus/gwenesis_bus.h"
#include "gwenesis/io/gwenesis_io.h"
#include "gwenesis/vdp/gwenesis_vdp.h"
#include "gwenesis/savestate/gwenesis_savestate.h"
}
extern "C" {
#include "vga.h"
}

#include "nespad.h"
#include "ps2kbd_mrmltr.h"
#include "f_util.h"
#include "ff.h"

#pragma GCC optimize("Ofast")

#ifndef OVERCLOCKING
#define OVERCLOCKING 270
#endif

#define FLASH_TARGET_OFFSET (900 * 1024)
static constexpr uintptr_t rom = (XIP_BASE + FLASH_TARGET_OFFSET);
char rom_filename[1] = { 0 };
static FATFS fs;
struct semaphore vga_start_semaphore;
static uint8_t SCREEN[240][320];

enum input_device {
    KEYBOARD,
    GAMEPAD1,
    GAMEPAD2,
};

// SETTINGS
bool show_fps = true;
bool limit_fps = true;
bool interlace = (OVERCLOCKING < 366);
bool frameskip = (OVERCLOCKING < 400);
bool flash_line = true;
bool flash_frame = true;
uint8_t player_1_input = GAMEPAD1;
uint8_t player_2_input = KEYBOARD;

bool reboot = false;



typedef struct  __attribute__((__packed__)) {
    bool a: 1;
    bool b: 1;
    bool c: 1;
    bool x: 1;
    bool y: 1;
    bool z: 1;
    bool mode: 1;
    bool start: 1;
    bool right: 1;
    bool left: 1;
    bool up: 1;
    bool down: 1;
} input_bits_t;

static input_bits_t keyboard_bits = {};
static input_bits_t gamepad1_bits = {};
static input_bits_t gamepad2_bits = {};

void nespad_tick() {
    nespad_read();

    gamepad1_bits.a = (nespad_state & DPAD_A) != 0;
    gamepad1_bits.b = (nespad_state & DPAD_B) != 0;
    gamepad1_bits.c = (nespad_state & DPAD_SELECT) != 0;
    gamepad1_bits.start = (nespad_state & DPAD_START) != 0;
    gamepad1_bits.up = (nespad_state & DPAD_UP) != 0;
    gamepad1_bits.down = (nespad_state & DPAD_DOWN) != 0;
    gamepad1_bits.left = (nespad_state & DPAD_LEFT) != 0;
    gamepad1_bits.right = (nespad_state & DPAD_RIGHT) != 0;

    gamepad2_bits.a = (nespad_state2 & DPAD_A) != 0;
    gamepad2_bits.b = (nespad_state2 & DPAD_B) != 0;
    gamepad2_bits.c = (nespad_state2 & DPAD_SELECT) != 0;
    gamepad2_bits.start = (nespad_state2 & DPAD_START) != 0;
    gamepad2_bits.up = (nespad_state2 & DPAD_UP) != 0;
    gamepad2_bits.down = (nespad_state2 & DPAD_DOWN) != 0;
    gamepad2_bits.left = (nespad_state2 & DPAD_LEFT) != 0;
    gamepad2_bits.right = (nespad_state2 & DPAD_RIGHT) != 0;
}

static bool isInReport(hid_keyboard_report_t const *report, const unsigned char keycode) {
    for (unsigned char i: report->keycode) {
        if (i == keycode) {
            return true;
        }
    }
    return false;
}

void
__not_in_flash_func(process_kbd_report)(hid_keyboard_report_t const *report, hid_keyboard_report_t const *prev_report) {
    /* printf("HID key report modifiers %2.2X report ", report->modifier);
    for (unsigned char i: report->keycode)
        printf("%2.2X", i);
    printf("\r\n");
     */
    keyboard_bits.c = isInReport(report, HID_KEY_ENTER);
    keyboard_bits.mode = isInReport(report, HID_KEY_BACKSPACE);
    keyboard_bits.a = isInReport(report, HID_KEY_A);
    keyboard_bits.b = isInReport(report, HID_KEY_S);
    keyboard_bits.start = isInReport(report, HID_KEY_D);
    keyboard_bits.x = isInReport(report, HID_KEY_Z);
    keyboard_bits.y = isInReport(report, HID_KEY_X);
    keyboard_bits.z = isInReport(report, HID_KEY_C);
    keyboard_bits.up = isInReport(report, HID_KEY_ARROW_UP);
    keyboard_bits.down = isInReport(report, HID_KEY_ARROW_DOWN);
    keyboard_bits.left = isInReport(report, HID_KEY_ARROW_LEFT);
    keyboard_bits.right = isInReport(report, HID_KEY_ARROW_RIGHT);
    //-------------------------------------------------------------------------
}

Ps2Kbd_Mrmltr ps2kbd(
        pio1,
        0,
        process_kbd_report);


int start_time = 0;
int frame, frame_cnt = 0;
int frame_timer_start = 0;


/**
 * Load a .gb rom file in flash from the SD card
 */
void load_cart_rom_file(char *filename) {
    FIL fil;
    FRESULT fr;

    size_t bufsize = sizeof(SCREEN)&0xfffff000;
    BYTE *buffer = (BYTE *) SCREEN;
    auto ofs = FLASH_TARGET_OFFSET;
    printf("Writing %s rom to flash %x\r\n", filename, ofs);
    fr = f_open(&fil, filename, FA_READ);

    UINT bytesRead;
    if (fr == FR_OK) {
        for (;;) {
            gpio_put(PICO_DEFAULT_LED_PIN, true);
            fr = f_read(&fil, buffer, bufsize, &bytesRead);
            if (fr == FR_OK) {
                if (bytesRead == 0) {
                    break;
                }
                // SWAP LO<>HI
                for (int i = 0; i < bufsize; i += 2) {
                    unsigned char temp = buffer[i];
                    buffer[i] = buffer[i + 1];
                    buffer[i + 1] = temp;
                }
                printf("Flashing %d bytes to flash address %x\r\n", bytesRead, ofs);

                printf("Erasing...");
                gpio_put(PICO_DEFAULT_LED_PIN, false);
                // Disable interupts, erase, flash and enable interrupts
                uint32_t ints = save_and_disable_interrupts();

                flash_range_erase(ofs, bufsize);
                printf("  -> Flashing...\r\n");
                flash_range_program(ofs, buffer, bufsize);
                restore_interrupts(ints);
                ofs += bufsize;
            } else {
                printf("Error reading rom: %d\n", fr);
                break;
            }
        }


        f_close(&fil);
    }
}

uint16_t fileselector_display_page(char filenames[60][256], uint16_t page_number) {
    clrScr(0);
    char footer[80];
    const int files_in_page = 29;
    sprintf(footer, "=================== PAGE #%i -> NEXT PAGE / <- PREV. PAGE ====================", page_number);
    draw_text(footer, 0, files_in_page, 1, 11);

    DIR directory;
    FILINFO file;
    FRESULT result = f_mount(&fs, "", 1);
    if (FR_OK != result) {
        printf("E f_mount error: %s (%d)\n", FRESULT_str(result), result);
        return 0;
    }

    /* clear the filenames array */
    for (uint8_t ifile = 0; ifile < 28; ifile++) {
        memset(&filenames[ifile][0], 0, 256);
    }

    uint16_t total_files = 0;
    result = f_findfirst(&directory, &file, "SEGA\\", "*.*");

    /* skip the first N pages */
    if (page_number > 0) {
        while (total_files < page_number * files_in_page && result == FR_OK && file.fname[0]) {
            total_files++;
            result = f_findnext(&directory, &file);
        }
    }

    /* store the filenames of this page */
    total_files = 0;
    while (total_files < files_in_page && result == FR_OK && file.fname[0]) {
        strcpy(filenames[total_files], file.fname);
        total_files++;
        result = f_findnext(&directory, &file);
    }
    f_closedir(&directory);

    for (uint8_t ifile = 0; ifile < total_files; ifile++) {
        char pathname[255];
        uint8_t color = 0x0b;
        sprintf(pathname, "NES\\%s", filenames[ifile]);

        if (strcmp(pathname, rom_filename) != 0) {
            color = 0x0F;
        }
        draw_text(filenames[ifile], 0, ifile, color, 0);
    }
    return total_files;
}

void fileselector() {
    uint16_t page_number = 0;
    char filenames[60][256];

    printf("Selecting ROM\r\n");

    /* display the first page with up to 22 rom files */
    uint16_t total_files = fileselector_display_page(filenames, page_number);

    /* select the first rom */
    uint8_t current_file = 0;

    uint8_t color = 0x0b;
    draw_text(filenames[current_file], 0, current_file, color, 1);

    while (true) {
        char pathname[255];
        sprintf(pathname, "SEGA\\%s", filenames[current_file]);

        if (strcmp(pathname, rom_filename) != 0) {
            color = 0x0f;
        } else {
            color = 0x0b;
        }
#if USE_PS2_KBD
        ps2kbd.tick();
#endif

#if USE_NESPAD
        nespad_tick();
#endif
        sleep_ms(33);
#if USE_NESPAD
        nespad_tick();
#endif
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
        if (keyboard_bits.c || gamepad1_bits.c) {
            gpio_put(PICO_DEFAULT_LED_PIN, true);
            break;
        }

        if (keyboard_bits.start || gamepad1_bits.start || gamepad1_bits.a || gamepad1_bits.b) {
            /* copy the rom from the SD card to flash and start the game */
            load_cart_rom_file(pathname);
            break;
        }
        if (keyboard_bits.down || gamepad1_bits.down) {
            /* select the next rom */
            draw_text(filenames[current_file], 0, current_file, color, 0x0);
            current_file++;
            if (current_file >= total_files)
                current_file = 0;
            draw_text(filenames[current_file], 0, current_file, color, 0x1);
            sleep_ms(150);
        }
        if (keyboard_bits.up || gamepad1_bits.up) {
            /* select the previous rom */
            draw_text(filenames[current_file], 0, current_file, color, 0x00);
            if (current_file == 0) {
                current_file = total_files - 1;
            } else {
                current_file--;
            }
            draw_text(filenames[current_file], 0, current_file, color, 0x1);
            sleep_ms(150);
        }
        if (keyboard_bits.right || gamepad1_bits.right) {
            /* select the next page */
            page_number++;
            total_files = fileselector_display_page(filenames, page_number);
            if (total_files == 0) {
                /* no files in this page, go to the previous page */
                page_number--;
                total_files = fileselector_display_page(filenames, page_number);
            }
            /* select the first file */
            current_file = 0;
            draw_text(filenames[current_file], 0, current_file, color, 1);
            sleep_ms(150);
        }
        if ((keyboard_bits.left || gamepad1_bits.left) && page_number > 0) {
            /* select the previous page */
            page_number--;
            total_files = fileselector_display_page(filenames, page_number);
            /* select the first file */
            current_file = 0;
            draw_text(filenames[current_file], 0, current_file, color, 1);
            sleep_ms(150);
        }
        tight_loop_contents();
    }
}

/* Clocks and synchronization */
/* system clock is video clock */
int system_clock;


unsigned int lines_per_frame = LINES_PER_FRAME_NTSC; //262; /* NTSC: 262, PAL: 313 */
unsigned int scan_line;
unsigned int frame_counter = 0;
unsigned int drawFrame = 1;

extern unsigned char gwenesis_vdp_regs[0x20];
extern unsigned int gwenesis_vdp_status;
extern unsigned int screen_width, screen_height;
extern int hint_pending;

#define X2(a) (a | (a << 8))
#define CHECK_BIT(var, pos) (((var)>>(pos)) & 1)


enum menu_type_e {
    EMPTY,
    INT,
    TEXT,
    ARRAY,

    SAVE,
    LOAD,
    RESET,
    RETURN,
};

typedef struct __attribute__((__packed__)) {
    const char *text;
    menu_type_e type;
    const void *value;
    uint8_t max_value;
    char value_list[5][10];
} MenuItem;

#define MENU_ITEMS_NUMBER 12
const MenuItem menu_items[MENU_ITEMS_NUMBER] = {
        {"Player 1: %s",        ARRAY, &player_1_input, 2, {"Keyboard ", "Gamepad 1", "Gamepad 2"}},
        //{"Player 2: %s",        ARRAY, &player_2_input, 2, {"Keyboard ", "Gamepad 1", "Gamepad 2"}},
        {""},
        {"Flash line: %s",      ARRAY, &flash_line,     1, {"NO ",       "YES"}},
        {"Flash frame: %s",     ARRAY, &flash_frame,    1, {"NO ",       "YES"}},
        {""},
        {"Interlace mode: %s",      ARRAY, &interlace,     1, {"NO ",       "YES"}},
        {"Frameskip: %s",     ARRAY, &frameskip,  1, {"NO ",       "YES"}},
        {"Limit fps: %s",     ARRAY, &limit_fps,    1, {"NO ",       "YES"}},
        {"Show fps: %s",     ARRAY, &show_fps,    1, {"NO ",       "YES"}},
        {""},
        {"Reset to ROM select", RESET},
        {"Return to game",      RETURN}
};


void menu() {
    bool exit = false;
    clrScr(0);
    setVGAmode(VGA640x480_text_80_30);

    char footer[80];
    sprintf(footer, ":: %s %s build %s %s ::", PICO_PROGRAM_NAME, PICO_PROGRAM_VERSION_STRING, __DATE__, __TIME__);
    draw_text(footer, (sizeof(footer) - strlen(footer)) >> 1, 0, 11, 1);
    int current_item = 0;

    while (!exit) {
        ps2kbd.tick();
        nespad_read();
        sleep_ms(25);
        nespad_read();

        if ((nespad_state & DPAD_DOWN || keyboard_bits.down) != 0) {
            current_item = (current_item + 1) % MENU_ITEMS_NUMBER;
            if (menu_items[current_item].type == EMPTY)
                current_item++;
        }

        if ((nespad_state & DPAD_UP || keyboard_bits.up) != 0) {
            current_item = (current_item - 1 + MENU_ITEMS_NUMBER) % MENU_ITEMS_NUMBER;
            if (menu_items[current_item].type == EMPTY)
                current_item--;
        }

        for (int i = 0; i < MENU_ITEMS_NUMBER; i++) {
            uint8_t y = i + ((30 - MENU_ITEMS_NUMBER) >> 1);
            uint8_t x = 30;
            uint8_t color = 0xFF;
            uint8_t bg_color = 0x00;
            if (current_item == i) {
                color = 0x01;
                bg_color = 0xFF;
            }

            const MenuItem *item = &menu_items[i];

            if (i == current_item) {
                switch (item->type) {
                    case INT:
                    case ARRAY:
                        if (item->max_value != 0) {
                            auto *value = (uint8_t *) item->value;

                            if ((nespad_state & DPAD_RIGHT || keyboard_bits.right) && *value < item->max_value) {
                                (*value)++;
                            }

                            if ((nespad_state & DPAD_LEFT || keyboard_bits.left) && *value > 0) {
                                (*value)--;
                            }
                        }
                        break;
                    case SAVE:
                        if (nespad_state & DPAD_START || keyboard_bits.start) {
                            //save(rom_filename);
                            exit = true;
                        }
                        break;
                    case LOAD:
                        if (nespad_state & DPAD_START || keyboard_bits.start) {
                            //load(rom_filename);
                            exit = true;
                        }
                        break;
                    case RETURN:
                        if (nespad_state & DPAD_START || keyboard_bits.start)
                            exit = true;
                        break;
                    case RESET:
                        if (nespad_state & DPAD_START || keyboard_bits.start) {
                            reboot = true;
                            exit = true;
                        }
                        break;
                }

            }
            static char result[80];

            switch (item->type) {
                case INT:
                    sprintf(result, item->text, *(uint8_t *) item->value);
                    break;
                case ARRAY:
                    sprintf(result, item->text, item->value_list[*(uint8_t *) item->value]);
                    break;
                case TEXT:
                    sprintf(result, item->text, item->value);
                    break;
                default:
                    sprintf(result, "%s", item->text);
            }

            draw_text(result, x, y, color, bg_color);
        }

        sleep_ms(100);
    }

    setVGA_color_flash_mode(flash_line, flash_frame);
    setVGAmode(VGA640x480div2);
}

extern unsigned short button_state[3];

void gwenesis_io_get_buttons() {
    nespad_tick();
    ps2kbd.tick();

    input_bits_t player1_state = {};
    input_bits_t player2_state = {};

    switch (player_1_input) {
        case 0:
            player1_state = keyboard_bits;
            break;
        case 1:
            player1_state = gamepad1_bits;
            break;
        case 2:
            player1_state = gamepad2_bits;
            break;
    }

    switch (player_2_input) {
        case 0:
            player2_state = keyboard_bits;
            break;
        case 1:
            player2_state = gamepad1_bits;
            break;
        case 2:
            player2_state = gamepad2_bits;
            break;
    }


    button_state[0] = player1_state.left << PAD_LEFT |
                      player1_state.right << PAD_RIGHT |
                      player1_state.up << PAD_UP |
                      player1_state.down << PAD_DOWN |
                      player1_state.start << PAD_S |
                      player1_state.a << PAD_A |
                      player1_state.b << PAD_B |
                      player1_state.c << PAD_C;

    button_state[0] = ~button_state[0];

/*    button_state[2] = player2_state.left << PAD_LEFT |
                      player2_state.right << PAD_RIGHT |
                      player2_state.up << PAD_UP |
                      player2_state.down << PAD_DOWN |
                      player2_state.start << PAD_S |
                      player2_state.a << PAD_A |
                      player2_state.b << PAD_B |
                      player2_state.c << PAD_C;

    button_state[2] = ~button_state[2];*/

    if ((gamepad1_bits.start && gamepad1_bits.c) || (keyboard_bits.start && keyboard_bits.mode)) {
        menu();
    }
}

/* Renderer loop on Pico's second core */
void __time_critical_func(render_core)() {
    initVGA();
    auto *buffer = reinterpret_cast<uint8_t *>(&SCREEN[0][0]);
    setVGAbuf(buffer, 320, 240);
    uint8_t *text_buf = buffer + 1000;
    setVGA_text_buf(text_buf, &text_buf[80 * 30]);
    setVGA_bg_color(0);
    setVGAbuf_pos(0, 0);
    setVGA_color_flash_mode(flash_line, flash_frame);

    sem_acquire_blocking(&vga_start_semaphore);
}
char fps_text[3] = "99";
extern uint8_t fnt8x16[];
void __inline draw_fps(const char fps[3],uint8_t y, uint8_t color) {
    for (uint8_t x = 0; x < 3; x++) {
        uint8_t glyph_col = fnt8x16[(fps[x] << 4) + y];

        for (uint8_t bit = 0; bit < 8; bit++)
            if ((glyph_col >> bit) & 1)
                SCREEN[y][( 8*2)+8 * x + bit] = color;
    }
}

void emulate() {
    int hint_counter;

    while (!reboot) {
        /* Eumulator loop */
        hint_counter = gwenesis_vdp_regs[10];

        screen_height = REG1_PAL ? 240 : 224;
        screen_width = REG12_MODE_H40 ? 320 : 256;
        lines_per_frame = REG1_PAL ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
        setVGAbuf_pos(screen_width != 320 ? 32 : 0, screen_height != 240 ? 8 : 0);
        gwenesis_vdp_render_config();

        /* Reset the difference clocks and audio index */
        system_clock = 0;

        scan_line = 0;

        while (scan_line < lines_per_frame) {
            /* CPUs */
            m68k_run(system_clock + VDP_CYCLES_PER_LINE);

            /* Video */
            if (drawFrame) {
/*                if (show_fps && scan_line < 16)
                    draw_fps(fps_text, scan_line, 1);*/
                // Interlace mode
                if (!interlace || (frame % 2 == 0 && scan_line % 2) || scan_line % 2 == 0) {
                    gwenesis_vdp_set_buffer(&SCREEN[scan_line][0]);
                    gwenesis_vdp_render_line(scan_line); /* render scan_line */
                }
            }

            // On these lines, the line counter interrupt is reloaded
            if ((scan_line == 0) || (scan_line > screen_height)) {
                hint_counter = REG10_LINE_COUNTER;

            }

            // interrupt line counter
            if (--hint_counter < 0) {
                if ((REG0_LINE_INTERRUPT != 0) && (scan_line <= screen_height)) {
                    hint_pending = 1;
                    if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0)
                        m68k_update_irq(4);
                }
                hint_counter = REG10_LINE_COUNTER;
            }

            scan_line++;

            // vblank begin at the end of last rendered line
            if (scan_line == screen_height) {
                if (REG1_VBLANK_INTERRUPT != 0) {
                    gwenesis_vdp_status |= STATUS_VIRQPENDING;
                    m68k_set_irq(6);
                }
            }

            if (scan_line == (screen_height + 1)) {
                // FRAMESKIP every 3rd frame
                if (frameskip && frame % 3 == 0) {
                    drawFrame = 0;
                } else {
                    drawFrame = 1;
                }

/*                if (show_fps && frame >= 60) {
                    uint64_t end_time;
                    uint32_t diff;
                    uint8_t fps;
                    end_time = time_us_64();
                    diff = end_time - start_time;
                    fps = ((uint64_t) frame * 1000 * 1000) / diff;
                    char fps_text[3];
                    sprintf(fps_text, "%i ", fps);
                    draw_text(fps_text, 77, 0, 0xFF, 0x00);
                    frame = 0;
                    start_time = time_us_64();
                }*/
                if (show_fps && frame >= 60) {
                    uint64_t end_time;
                    uint32_t diff;
                    uint8_t fps;
                    end_time = time_us_64();
                    diff = end_time - start_time;
                    fps = ((uint64_t) frame * 1000 * 1000) / diff;

                    sprintf(fps_text, "%i", fps);
                    //draw_text(fps_text, 77, 0, 0xFF, 0x00);
                    frame = 0;
                    start_time = time_us_64();
                }

                frame++;
                if (limit_fps) {
                    frame_cnt++;
                    if (frame_cnt == 6) {
                        while (time_us_64() - frame_timer_start < 16667 * 6);  // 60 Hz
                        frame_timer_start = time_us_64();
                        frame_cnt = 0;
                    }
                }
            }

            system_clock += VDP_CYCLES_PER_LINE;
        }

        // reset m68k cycles to the begin of next frame cycle
        m68k.cycles -= system_clock;

        /* copy audio samples for DMA */
        //gwenesis_sound_submit();
    }
    reboot = false;
}

int main() {
    hw_set_bits(&vreg_and_chip_reset_hw->vreg, VREG_AND_CHIP_RESET_VREG_VSEL_BITS);
    sleep_ms(33);

    set_sys_clock_khz(OVERCLOCKING * 1000, true);

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    for (int i = 0; i < 6; i++) {
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }

#if !NDEBUG
    stdio_init_all();
#endif

    ps2kbd.init_gpio();
    nespad_begin(clock_get_hz(clk_sys) / 1000, NES_GPIO_CLK, NES_GPIO_DATA, NES_GPIO_LAT);

    sem_init(&vga_start_semaphore, 0, 1);
    multicore_launch_core1(render_core);
    sem_release(&vga_start_semaphore);




    while (true) {
        sleep_ms(50);
        setVGAmode(VGA640x480_text_80_30);
        fileselector();
        memset(SCREEN, 0, sizeof(SCREEN));
        setVGAmode(VGA640x480div2);

        load_cartridge(rom);
        power_on();
        reset_emulation();

        emulate();
    }
}