#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "error.h"

extern void cleanup();


/**
 * @brief      Get line length fitted in a console
 *
 * @details    Given a char array it returns the length of the array as long it's smaller or equal to the provided console, if it's bigger it only returns the width of the console - 1.
 *
 * @param      console PrintConsole.
 *
 * @return     int Line length that fits on console.
 */
static int util_get_line_length(PrintConsole* console, const char* str) {
    int lineLength = 0;
    while(*str != 0) {
        if(*str == '\n') {
            break;
        }

        lineLength++;
        if(lineLength >= console->consoleWidth - 1) {
            break;
        }

        str++;
    }

    return lineLength;
}

/**
 * @brief      Returns the number of lines required to print in a console.
 *
 * @details    Returns the number of lines required to print in a console taking into consideration the new line character.
 *
 * @param      console Console that will render the text.
 *
 * @param      str String to calculated the lines to print.
 *
 * @return     int Lines required to render the string.
 */
static int util_get_lines(PrintConsole* console, const char* str) {
    int lines = 1;
    int lineLength = 0;
    while(*str != 0) {
        if(*str == '\n') {
            lines++;
            lineLength = 0;
        } else {
            lineLength++;
            if(lineLength >= console->consoleWidth - 1) {
                lines++;
                lineLength = 0;
            }
        }

        str++;
    }

    return lines;
}
/**
 * @brief      Shows a "panic" screen showing an error.
 *
 * @details    Given a char array it renders a black screen showing the error message in the array. It waits for an input and when it receives that input it closes the program.
 *
 * @param      char* s Error message
 *
 * @return     void
 */
void error_panic(const char* s, ...) {
    ## Getting params
    va_list list;
    va_start(list, s);

    ## Generating a string with the give s format and the variable list
    char buf[1024];
    vsnprintf(buf, 1024, s, list);

    va_end(list);

    ## Blanking the displays
    gspWaitForVBlank();

    u16 width;
    u16 height;

    # Looping to set all three screen buffers to black.
    for(int i = 0; i < 2; i++) {
        # Setting all three screens (2 top + 1 bottom) to black (0) (*3 because is RGB).
        memset(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &width, &height), 0, (size_t) (width * height * 3));
        memset(gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, &width, &height), 0, (size_t) (width * height * 3));
        memset(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &width, &height), 0, (size_t) (width * height * 3));

        gfxSwapBuffers();
    }

    # We initialzate a console on the top screen.
    PrintConsole* console = consoleInit(GFX_TOP, NULL);

    const char* header = "FBI has encountered a fatal error!";
    const char* footer = "Press any button to exit.";

    # Fills the top and bottom of the screen with lines (------)
    printf("\x1b[0;0H");
    for(int i = 0; i < console->consoleWidth; i++) {
        printf("-");
    }

    printf("\x1b[%d;0H", console->consoleHeight - 1);
    for(int i = 0; i < console->consoleWidth; i++) {
        printf("-");
    }

    # Prints the header and footer centered overwriting the previous drawn lines.
    printf("\x1b[0;%dH%s", (console->consoleWidth - util_get_line_length(console, header)) / 2, header);
    printf("\x1b[%d;%dH%s", console->consoleHeight - 1, (console->consoleWidth - util_get_line_length(console, footer)) / 2, footer);

    #Prints the message vertically centered. Be aware than unlike util_get_line where it takes into account the console dimensions in this case is not taken into account the height so the bufRow could become negative.
    int bufRow = (console->consoleHeight - util_get_lines(console, buf)) / 2;
    char* str = buf;
    while(*str != 0) {
        if(*str == '\n') {
            bufRow++;
            str++;
            continue;
        } else {
            int lineLength = util_get_line_length(console, str);

            char old = *(str + lineLength);
            *(str + lineLength) = '\0';
            printf("\x1b[%d;%dH%s", bufRow, (console->consoleWidth - lineLength) / 2, str);
            *(str + lineLength) = old;

            bufRow++;
            str += lineLength;
        }
    }

    #Flushed the buffers to show the error
    gfxFlushBuffers();
    gspWaitForVBlank();

    #Waits for a key input to stop.
    while(aptMainLoop()) {
        hidScanInput();
        if(hidKeysDown() & ~KEY_TOUCH) {
            break;
        }

        gspWaitForVBlank();
    }

    #Exits the app
    cleanup();
    exit(1);
}
