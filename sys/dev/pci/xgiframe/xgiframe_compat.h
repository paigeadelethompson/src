/*	$NetBSD$
 *
 * xgiframe_compat.h -- small shims bridging the ported XGI Volari code to
 *	NetBSD kernel facilities.
 *
 * Only things that are NOT already supplied by the ported headers live
 * here.  In particular the SiS_* table element types come from vstruct.h,
 * the mode-flag and display constants from initdef.h and XGIfb.h -- do not
 * duplicate them: the table initialisers in vb_table.h are positional, so a
 * duplicate or revalued definition silently corrupts every entry after it.
 */

#ifndef _XGIFRAME_COMPAT_H_
#define	_XGIFRAME_COMPAT_H_

#include <sys/cdefs.h>
#include <sys/types.h>

/*
 * Linux fixed-width typedefs used by the ported sources.  NetBSD uses the
 * _t suffixed names.
 */
typedef uint8_t		u8;
typedef uint16_t	u16;
typedef uint32_t	u32;
typedef uint64_t	u64;

/*
 * Linux phys_addr_t.  Used by struct xgifb_video_info for the BAR addresses.
 */
typedef paddr_t		phys_addr_t;

/*
 * From the upstream SiS framebuffer driver's vgatypes.h, which vstruct.h
 * depends on but does not carry.  SISIOMEMTYPE is __iomem there; on evbarm
 * the framebuffer is an ordinary pointer, so it expands to nothing.
 */
#ifndef SISIOMEMTYPE
#define	SISIOMEMTYPE
#endif

#ifndef SISIOADDRESS
typedef unsigned long	SISIOADDRESS;
#endif

/*
 * Linux ARRAY_SIZE(); NetBSD spells it __arraycount.
 */
#ifndef ARRAY_SIZE
#define	ARRAY_SIZE(a)	__arraycount(a)
#endif

/*
 * LCD panel types (enum _SIS_LCD_TYPE) from the upstream SiS driver's sis.h,
 * copied verbatim.  Referenced by XGINew_SenseLCD() in the ported code.
 */
#include "xgiframe_lcdtype.h"

/*
 * Debug tracing.  The build generates opt_xgiframe.h from the "defflag" line
 * in files.pci, but config only emits a #define when the option is actually
 * enabled -- with it off the macro is never defined at all.  Provide a
 * default so the driver can reference it unconditionally.
 */
#ifndef XGIFRAME_DEBUG
#define	XGIFRAME_DEBUG 0
#endif

/*
 * Debug hook used by the ported sources (vb_init.c).  Defined in xgiframe.c.
 */
void xgifb_debug(const char *);

#endif /* _XGIFRAME_COMPAT_H_ */
