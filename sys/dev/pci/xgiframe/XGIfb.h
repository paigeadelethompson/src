#ifndef _NETBSD_XGIFB
#define _NETBSD_XGIFB

/*
 * NetBSD port of the Linux xgifb driver headers.  Changes from upstream:
 *  - the fbdev <linux/fb.h> dependency is gone; the struct fb_info member
 *    is dropped since nothing in the ported mode-setting code uses it
 *  - __iomem qualifiers removed (plain pointers are correct on evbarm)
 *  - phys_addr_t comes from sys/types.h
 */

#include <sys/cdefs.h>
#include <sys/types.h>

#include "xgiframe_compat.h"
#include "vgatypes.h"
#include "vb_struct.h"

enum xgifb_display_type {
	XGIFB_DISP_NONE = 0,
	XGIFB_DISP_CRT,
	XGIFB_DISP_LCD,
	XGIFB_DISP_TV,
};

#define HASVB_NONE	    0x00
#define HASVB_301	    0x01
#define HASVB_LVDS	    0x02
#define HASVB_TRUMPION	    0x04
#define HASVB_LVDS_CHRONTEL 0x10
#define HASVB_302	    0x20
#define HASVB_CHRONTEL	    0x80

enum XGI_CHIP_TYPE {
	XG40 = 32,
	XG42,
	XG20 = 48,
	XG21,
	XG27,
};

enum xgi_tvtype {
	TVMODE_NTSC = 0,
	TVMODE_PAL,
	TVMODE_HIVISION,
	TVTYPE_PALM,
	TVTYPE_PALN,
	TVTYPE_NTSCJ,
	TVMODE_TOTAL
};

/*
 * Video bridge chip types.  This enum lived in vgatypes.h upstream; it is
 * defined here because vgatypes.h is included before this file and the
 * bridge identifiers are only referenced from here and the ported code.
 * Keep it in exactly one place -- it is referenced by vb_setmode.c.
 */
enum XGI_VB_CHIP_TYPE {
	VB_CHIP_Legacy = 0,
	VB_CHIP_301,
	VB_CHIP_301B,
	VB_CHIP_301LV,
	VB_CHIP_302,
	VB_CHIP_302B,
	VB_CHIP_302LV,
	VB_CHIP_301C,
	VB_CHIP_302ELV,
	VB_CHIP_UNKNOWN,		/* other video bridge, or no bridge */
	MAX_VB_CHIP
};

enum xgi_tv_plug {
	TVPLUG_UNKNOWN = 0,
	TVPLUG_COMPOSITE = 1,
	TVPLUG_SVIDEO = 2,
	TVPLUG_COMPOSITE_AND_SVIDEO = 3,
	TVPLUG_SCART = 4,
	TVPLUG_YPBPR_525i = 5,
	TVPLUG_YPBPR_525P = 6,
	TVPLUG_YPBPR_750P = 7,
	TVPLUG_YPBPR_1080i = 8,
	TVPLUG_TOTAL
};

struct xgifb_video_info {
	struct xgi_hw_device_info hw_info;
	struct vb_device_info dev_info;

	int mode_idx;
	int rate_idx;

	u32 pseudo_palette[17];

	int           chip_id;
	unsigned int  video_size;
	phys_addr_t   video_base;
	void *video_vbase;
	phys_addr_t   mmio_base;
	unsigned long mmio_size;
	void *mmio_vbase;
	unsigned long vga_base;
	int mtrr;

	int    video_bpp;
	int    video_cmap_len;
	int    video_width;
	int    video_height;
	int    video_vwidth;
	int    video_vheight;
	int    org_x;
	int    org_y;
	int    video_linelength;
	unsigned int refresh_rate;

	enum xgifb_display_type display2; /* the second display output type */
	bool display2_force;
	unsigned char hasVB;
	unsigned char TV_type;
	unsigned char TV_plug;

	struct XGI21_LVDSCapStruct lvds_data;

	enum XGI_CHIP_TYPE chip;
	unsigned char revision_id;

	unsigned short DstColor;
	unsigned long  XGI310_AccelDepth;
	unsigned long  CommandReg;

	unsigned int   pcibus;
	unsigned int   pcislot;
	unsigned int   pcifunc;

	unsigned short subsysvendor;
	unsigned short subsysdevice;

	char reserved[236];
};

#endif /* _NETBSD_XGIFB */
