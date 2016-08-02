#ifndef __DEV_TYPE_H
#define __DEV_TYPE_H

//struct
struct subcard_size {
	int subcard_row;
	int subcard_col;
	int subcard_cnt;
};

enum device_type {
	K7F_MAP = 0,
	K7h_DUAL_MAP,
	FIJI_MAP,
	_8639_MAP,
	DEV_TOTAL
};

//extern
extern struct subcard_size sc_size;

extern const unsigned char dev_pkg_phylun_k7f[][8+PKG_ATTR];
extern const char *pkg_partRef_k7f_map[];

extern const unsigned char dev_pkg_phylun_k7h_dual[][8+PKG_ATTR];
extern const char *pkg_partRef_k7h_dual_map[];

extern const unsigned char dev_pkg_phylun_fiji[][8+PKG_ATTR];
extern const char *pkg_partRef_fiji_map[];

// function
void config_dev_type(struct subcard_size *sc_size, int dev_type);

#endif
