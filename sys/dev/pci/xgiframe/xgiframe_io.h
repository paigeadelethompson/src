/*	$NetBSD$
 *
 * xgiframe_io.h -- register access shim for the ported XGI Volari code.
 *
 * The upstream xgifb driver uses the x86 I/O port primitives outb()/inb().
 * On evbarm there are no such instructions: PCI I/O space is decoded into a
 * memory window and reached through the ordinary bus space accessors.
 *
 * vb_util.c and the ported mode-setting code are therefore compiled against
 * these wrappers instead, so the register sequences themselves can be used
 * unmodified.
 */

#ifndef _XGIFRAME_IO_H_
#define	_XGIFIFRAME_IO_H_

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/param.h>		/* delay() */

#include <dev/pci/pcivar.h>	/* pulls in sys/bus.h: bus_space_* */

/*
 * Linux mdelay() takes milliseconds.  NetBSD has no mdelay(), and delay()
 * takes microseconds, so wrap it to preserve the ported call sites' meaning.
 */
static inline void
mdelay_us(unsigned int msec)
{
	delay(msec * 1000);
}

/*
 * Handle for the chip's I/O window (BAR2, relocated).  Set once by the
 * attach code; the ported code reaches registers through bare port numbers
 * derived from BaseAddr, so the handle is kept here rather than threaded
 * through every call.
 */
extern bus_space_handle_t xgiframe_ioh;
extern bus_space_tag_t	xgiframe_iot;

/*
 * Bus address of the first byte covered by xgiframe_ioh (i.e. the BAR's
 * physical address, 0x1000 on the T5325).
 *
 * The ported code addresses registers as *absolute* I/O port numbers -- the
 * same convention x86's outb()/inb() use -- so xgifb_info->vga_base and every
 * dev_info->P3xx offset is computed from the BAR's physical address.  But a
 * bus_space_handle is not a port number: mvsoc_bs_map() returns a kernel
 * virtual address (uvm_km_alloc + page offset), and the accessors index off it
 * directly (generic_bs_w_1 is "strb r3, [r1, r2]").  Passing an absolute port
 * number straight through would therefore address handle + 0x1044, several
 * pages past the 128-byte mapping, i.e. unrelated kernel memory.
 *
 * So the shim translates: the offset within a bus_space region is always
 * (port - base).
 */
extern bus_addr_t xgiframe_iobase;

#define	XGIFRAME_IOOFF(port)						\
	((bus_addr_t)((port) - xgiframe_iobase))

static inline void
xgiframe_outb(bus_addr_t port, uint8_t val)
{
	bus_space_write_1(xgiframe_iot, xgiframe_ioh,
	    XGIFRAME_IOOFF(port), val);
}

static inline uint8_t
xgiframe_inb(bus_addr_t port)
{
	return bus_space_read_1(xgiframe_iot, xgiframe_ioh,
	    XGIFRAME_IOOFF(port));
}

/*
 * The ported sources name the primitives outb()/inb().  Provide them as
 * macros over the shim so those files need no other edits.
 */
#define	outb(val, port)	xgiframe_outb((bus_addr_t)(port), (uint8_t)(val))
#define	inb(port)		xgiframe_inb((bus_addr_t)(port))

#endif /* _XGIFRAME_IO_H_ */
