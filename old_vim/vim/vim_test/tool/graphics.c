#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "tool.h"

void draw_horizontal_line(int sx, int sy, int length, int color)
{
	int pos;
	int end;

	end = sx + length;

	save_cursor();
	set_cursor(sx, sy);

	for (pos = sx; pos < end; pos++)
		print("\033[1m\033[%dm.\033[0m", color);

	load_cursor();
}

void draw_vertical_line(int sx, int sy, int width, int color)
{
	int pos;
	int end;

	end = sy + width;

	save_cursor();
	set_cursor(sx, sy);

	for (pos = sy; pos < end; pos++) {
		print("\033[1m\033[%dm.\033[0m", color);
		move_down(1);
		move_left(1);
	}

	load_cursor();
}

void draw_text(int sx, int sy, const char *text, int color)
{
	save_cursor();
	set_cursor(sx, sy);

	print("\033[1m\033[%dm%s\033[0m", color, text);

	load_cursor();
}

void draw_rectangle(int sx, int sy, int length, int width, int color)
{
	draw_horizontal_line(sx, sy, length, color);
	draw_horizontal_line(sx, sy+width-1, length, color);
	draw_vertical_line(sx, sy, width, color);
	draw_vertical_line(sx+length-1, sy, width, color);
}

void draw_package(int sx, int sy, int length, int width, const char *partRef, int color)
{
	draw_rectangle(sx, sy, length, width, color);
	draw_text(sx+1, sy+1, partRef, color);
}

void draw_subcard(int sx, int sy, int row, int col, const char *partRef[], char *color_package)
{
	int r = 0;
	int c = 0;

	for (r = 0; r < row; r++) {
		for (c = 0; c < col; c++) {
			draw_package(sx+c*(LENGTH+INNER_COL_SPAN), sy+r*(WIDTH+INNER_ROW_SPAN), LENGTH, WIDTH, *(partRef+col*r+c), *(color_package+col*r+c));
		}
	}
}

void draw_lun_map(int sx, int sy, int row, int col, const char *pkg_partRef_map[], char *subcard_color_pkg)
{
	int lfs_sx, lfs_sy;
	int lbs_sx, lbs_sy;
	int rfs_sx, rfs_sy;
	int rbs_sx, rbs_sy;

	lfs_sx = sx; lfs_sy = sy;
	lbs_sx = sx; lbs_sy = sy+row*(INNER_ROW_SPAN+WIDTH)+UP_DOWN_SPAN;
	rfs_sx = sx+col*(INNER_COL_SPAN+LENGTH)+LEFT_RIGHT_SPAN; rfs_sy = sy;
	rbs_sx = rfs_sx; rbs_sy = lbs_sy;

	draw_horizontal_line(sx, lbs_sy-UP_DOWN_SPAN/2-1, rfs_sx+col*(INNER_COL_SPAN+LENGTH), WHITE);
	draw_vertical_line(rfs_sx-LEFT_RIGHT_SPAN/2-1, sy, lbs_sy+row*(INNER_ROW_SPAN+WIDTH), WHITE);

	draw_text(lfs_sx+LENGTH, lfs_sy+row*(INNER_ROW_SPAN+WIDTH), "left front side", WHITE);
	draw_text(lbs_sx+LENGTH, lbs_sy+row*(INNER_ROW_SPAN+WIDTH), "left back side", WHITE);
	draw_text(rfs_sx+LENGTH, rfs_sy+row*(INNER_ROW_SPAN+WIDTH), "right front side", WHITE);
	draw_text(rbs_sx+LENGTH, rbs_sy+row*(INNER_ROW_SPAN+WIDTH), "right back side", WHITE);

	draw_subcard(lfs_sx, lfs_sy, row, col, pkg_partRef_map, subcard_color_pkg); // print left front side
	draw_subcard(lbs_sx, lbs_sy, row, col, pkg_partRef_map+SUBCARD_PKGS, subcard_color_pkg+SUBCARD_PKGS); // print left back side
	draw_subcard(rfs_sx, rfs_sy, row, col, pkg_partRef_map+2*SUBCARD_PKGS, subcard_color_pkg+2*SUBCARD_PKGS); // print righ front side
	draw_subcard(rbs_sx, rbs_sy, row, col, pkg_partRef_map+3*SUBCARD_PKGS, subcard_color_pkg+3*SUBCARD_PKGS); // print righ back side

	return;
}

int get_pkg_color(const unsigned char *pkg, char (*phy_lun_map_color)[256])
{
	int hl_index = 0;
	int pkg_nlun = 0;
	int color;

	hl_index = ((pkg[8]== HBYTE) ? HBYTE : LBYTE);

	color = phy_lun_map_color[hl_index][pkg[pkg_nlun]];

	for (pkg_nlun = 0; pkg_nlun < PKG_LUNS; pkg_nlun++) {
		if (phy_lun_map_color[hl_index][pkg[pkg_nlun]] == RED)
			color = RED;
	}

	return color;
}


