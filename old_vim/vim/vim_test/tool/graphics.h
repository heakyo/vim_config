#ifndef __GRAPHICS_H
#define __GRAPHICS_H

/**********definition***********/
#define move_up(n)      do { printf("\033[%dA", n); } while(0)  // move cursor up n lines
#define move_down(n)    do { printf("\033[%dB", n); } while(0)  // move cursor down n lines
#define move_right(n)   do { printf("\033[%dC", n); } while(0)  // move cursor right n column
#define move_left(n)    do { printf("\033[%dD", n); } while(0)  // move cursor left n column
#define moveto_head()   do { printf("\r"); } while(0)           // move cursor to line head
#define clear_line()    do { printf("\033[K"); } while(0)       // clear display from cursor to line tail

#define save_cursor()   do { printf("\033[s"); } while(0)       // save current cursor location
#define load_cursor()   do { printf("\033[u"); } while(0)       // restore cursor location saved by save_cursor()

#define clear_screen()  do { printf("\033[2J"); } while(0)      // clear all display on the screen
#define hide_cursor     do { printf("\033[?25l"); } while(0)    // hide cursor
#define show_cursor     do { printf("\033[?25h"); } while(0)    // show cursor
#define set_cursor(x,y) do { printf("\033[%d;%dH", y, x); } while(0)    // set cursor to coordinate [x, y]
#define reset_cursor()  set_cursor(0,0)

#define LENGTH 5
#define WIDTH 3
#define PKG_ATTR 1
#define PKG_LUNS 8
#define SUBCARD_ROW sc_size.subcard_row
#define SUBCARD_COL sc_size.subcard_col
#define SUBCARD_CNT sc_size.subcard_cnt
#define SUBCARD_PKGS (SUBCARD_ROW * SUBCARD_COL)
#define TOTAL_PKG_CNT (SUBCARD_CNT * SUBCARD_PKGS)
#define UP_DOWN_SPAN 3
#define LEFT_RIGHT_SPAN 8
#define INNER_COL_SPAN 2
#define INNER_ROW_SPAN 1
#define LUN_MAP_SX 2
#define LUN_MAP_SY 2

/**********struct***********/
enum color {
	BLACK = 30,
	RED,
	GREEN,
	YELLOW,
	BLUE,
	PURPLE,
	DARK_GREEN,
	WHITE,
	COLOR_TOTAL
};

enum high_low_byte {
	LBYTE = 0,
	HBYTE = 1,
	BYTE_TOTLE
};

/*********function declaration***************/
void draw_horizontal_line(int sx, int sy, int length, int color);
void draw_vertical_line(int sx, int sy, int width, int color);
void draw_text(int sx, int sy, const char *text, int color);
void draw_rectangle(int sx, int sy, int length, int width, int color);
void draw_package(int sx, int sy, int length, int width, const char *partRef, int color);
void draw_subcard(int sx, int sy, int row, int col, const char *partRef[], char *color_package);
void draw_lun_map(int sx, int sy, int row, int col, const char *pkg_partRef_map[], char *subcard_color_pkg);
int get_pkg_color(const unsigned char *pkg, char (*phy_lun_map_color)[256]);

#endif
