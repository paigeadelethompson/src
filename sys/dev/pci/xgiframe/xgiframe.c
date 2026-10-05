/*	$NetBSD$
 *
 * xgiframe.c -- XGI Volari framebuffer console for NetBSD/evbarm
 *
 * Copyright (c) 2026
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * NetBSD glue for the ported XGI Volari mode-setting engine.
 *
 * The register sequences, mode tables and DVI bring-up are the upstream
 * Linux xgifb driver (Arnaud Patard), ported into this directory.  This file
 * replaces only its fbdev layer: PCI attach, memory mapping, mode selection
 * and wsdisplay/rasops integration.
 *
 * Why this driver instead of vga(4): the Volari is programmed through the
 * legacy VGA register window at BAR2 + 0x30.  On x86 the generic vga driver
 * works because the card's BIOS Option ROM has already put the chip into a
 * known mode -- VGA_POST in sys/dev/pci/vga_pci.c, which is x86-only.  On
 * evbarm there is no POST, so vga attaches (it matches on PCI class code) but
 * cannot program the chip.  XGIInitNew() and XGISetModeNew() do that work
 * here, including XGI_DisplayOn()'s digital output enable, which is what
 * drives this machine's DVI port -- its only display connector.
 */

#include <sys/cdefs.h>

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/ioctl.h>
#include <sys/kauth.h>
#include <sys/errno.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcidevs.h>
#include <dev/pci/pciio.h>
#include <dev/pci/wsdisplay_pci.h>


#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wsdisplayvar.h>
#include <dev/wscons/wsdisplay_vconsvar.h>
#include <dev/rasops/rasops.h>

#include "xgiframe_compat.h"
#include "xgiframe_io.h"

#include "XGIfb.h"
#include "vb_def.h"
#include "vb_struct.h"
#include "vb_util.h"
#include "vb_init.h"
#include "vb_setmode.h"

/*
 * The ported code reaches the chip's registers through bare port numbers
 * derived from xgifb_info->vga_base, so the mapped handles live here.
 */
bus_space_handle_t xgiframe_ioh;
bus_space_tag_t	xgiframe_iot;

/*
 * Bus address corresponding to xgiframe_ioh, so the I/O shim can turn the
 * ported code's absolute I/O port numbers into region-relative offsets.
 */
bus_addr_t xgiframe_iobase;

/* Debug hook used by the ported sources. */
void xgifb_debug(const char *);

void
xgifb_debug(const char *fmt)
{
	if (XGIFRAME_DEBUG)
		printf("%s", fmt);
}

/*
 * Publish the register file at the relocated VGA window.
 *
 * Ported verbatim from XGI_main_26.c.  The ported sources (vb_init.c,
 * vb_setmode.c) index dev_info->P3c4, XGIPART1..5 and so on to reach the
 * hardware, so this has to run before any of them touch the chip.  BaseAddr
 * is xgifb_info->vga_base, which xgiframe_chip_init() has already set to
 * BAR2 + 0x30 -- the same relocation upstream does at XGI_main_26.c:1684.
 */
void
XGIRegInit(struct vb_device_info *XGI_Pr, unsigned long BaseAddr)
{
	XGI_Pr->P3c4 = BaseAddr + 0x14;
	XGI_Pr->P3d4 = BaseAddr + 0x24;
	XGI_Pr->P3c0 = BaseAddr + 0x10;
	XGI_Pr->P3ce = BaseAddr + 0x1e;
	XGI_Pr->P3c2 = BaseAddr + 0x12;
	XGI_Pr->P3cc = BaseAddr + 0x1c;
	XGI_Pr->P3ca = BaseAddr + 0x1a;
	XGI_Pr->P3c6 = BaseAddr + 0x16;
	XGI_Pr->P3c7 = BaseAddr + 0x17;
	XGI_Pr->P3c8 = BaseAddr + 0x18;
	XGI_Pr->P3c9 = BaseAddr + 0x19;
	XGI_Pr->P3da = BaseAddr + 0x2A;
	XGI_Pr->Part0Port = BaseAddr + XGI_CRT2_PORT_00;
	/* Digital video interface registers (LCD) */
	XGI_Pr->Part1Port = BaseAddr + SIS_CRT2_PORT_04;
	/* 301 TV Encoder registers */
	XGI_Pr->Part2Port = BaseAddr + SIS_CRT2_PORT_10;
	/* 301 Macrovision registers */
	XGI_Pr->Part3Port = BaseAddr + SIS_CRT2_PORT_12;
	/* 301 VGA2 (and LCD) registers */
	XGI_Pr->Part4Port = BaseAddr + SIS_CRT2_PORT_14;
	/* 301 palette address port registers */
	XGI_Pr->Part5Port = BaseAddr + SIS_CRT2_PORT_14 + 2;
}

/*
 * Default console mode.  Mode 0x4a is the driver's 1024x768x16 entry in
 * XGIbios_mode[] (see XGI_main.h).
 *
 * The depth here is not free choice: XGIbios_mode[][].bpp is the authority
 * for what XGISetModeNew() actually programs into the chip (upstream likewise
 * takes bits_per_pixel from this table, XGI_main_26.c "case 16:").  Mode 0x4a
 * is 16bpp, so the chip ends up in 16bpp with a 2048-byte line length
 * (line_length = xres * bpp >> 6, programmed into CR13/SR0E) and the rasops
 * layer below has to agree or the console renders at half the real line width.
 *
 * 16bpp is also the right depth for a machine with no VGA POST: it bypasses
 * the DAC entirely, so we do not have to guess at a palette that nothing on
 * this board has ever programmed.
 */
#define	XGI_MODE_DEFAULT	0x4a
#define	XGI_DEFAULT_WIDTH	1024
#define	XGI_DEFAULT_HEIGHT	768
#define	XGI_DEFAULT_DEPTH	16	/* RGB565: r:11/5 g:5/6 b:0/5 */
#define	XGI_DEFAULT_STRIDE	(XGI_DEFAULT_WIDTH * 2)

/*
 * Bytes mapped from the base of BAR2, the legacy register window.
 *
 * BAR2's size mask only claims 128 bytes, but the ported sequences address
 * well past that: the largest offset reached from the BAR base is 0x123 (the
 * CRT index port P3d4 with index 0xcf; P3c4 reaches index 0x9c and P3ce index
 * 0x9b).  Upstream runs those accesses as flat x86 I/O ports, where a port
 * past the end of a BAR simply reads back 0xff, but a bus_space_handle is a
 * bounded mapping -- an offset outside it is unrelated kernel memory, so the
 * mapping has to cover what the code touches.  4 KB covers the ported code
 * with room to spare and sits well inside the Kirkwood's 1 MB PEX0_IO window.
 */
#define	XGI_REG_MAP_SIZE	0x1000

struct xgiframe_softc {
	device_t sc_dev;

	pci_chipset_tag_t sc_pc;
	pcitag_t sc_pcitag;

	bus_space_tag_t sc_iot;
	bus_space_tag_t sc_memt;
	bus_space_handle_t sc_ioh;
	bus_space_handle_t sc_fbh;

	bus_addr_t sc_ioaddr;
	bus_size_t sc_iosize;
	bus_addr_t sc_fbaddr;
	bus_size_t sc_fbsize;

	/*
	 * Sizes actually handed to bus_space_map(), which are not always the
	 * sizes the BARs claim: see the register window and the clamp to the
	 * PEX0_MEM window in xgiframe_attach().  These must be used for the
	 * matching bus_space_unmap().
	 */
	bus_size_t sc_iomapsize;
	bus_size_t sc_fbmapsize;

	uint8_t *sc_fb;

	/*
	 * XGIInitNew() and XGISetModeNew() both take a full xgifb_video_info
	 * but only ever read through it, never write.  Still, one shared
	 * instance per device keeps the ported code's expectations intact if
	 * that ever changes.  sc_xgi is deliberately the single source of
	 * vga_base, which xgiframe_ioh/xgiframe_iobase are derived from.
	 */
	struct xgifb_video_info sc_xgi;

	int sc_width, sc_height, sc_depth, sc_stride;
	int sc_mode;

	struct vcons_screen sc_console_screen;
	struct wsscreen_descr sc_defaultscreen_descr;
	const struct wsscreen_descr *sc_screens[1];
	struct wsscreen_list sc_screenlist;
	struct vcons_data sc_vd;
};

static int	xgiframe_match(device_t, cfdata_t, void *);
static void	xgiframe_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(xgiframe, sizeof(struct xgiframe_softc),
    xgiframe_match, xgiframe_attach, NULL, NULL);

static int	xgiframe_ioctl(void *, void *, u_long, void *, int, struct lwp *);
static paddr_t	xgiframe_mmap(void *, void *, off_t, int);
static void	xgiframe_init_screen(void *, struct vcons_screen *, int, long *);

static int	xgiframe_chip_init(struct xgiframe_softc *);
static int	xgiframe_set_mode(struct xgiframe_softc *, int, int, int);
static void	xgiframe_cr_set(struct xgifb_video_info *, uint8_t, uint8_t);
static void	xgiframe_probe_straps(struct xgiframe_softc *);
static void	xgiframe_probe_chip(struct xgiframe_softc *);
static void	xgiframe_probe_windows(struct xgiframe_softc *);
static void	xgiframe_probe_mode(struct xgiframe_softc *);

static struct wsdisplay_accessops xgiframe_accessops = {
	xgiframe_ioctl,
	xgiframe_mmap,
	NULL,	/* alloc_screen */
	NULL,	/* free_screen */
	NULL,	/* show_screen */
	NULL,	/* load_font */
	NULL,	/* pollc */
	NULL,	/* scroll */
};

static int
xgiframe_match(device_t parent, cfdata_t match, void *aux)
{
	struct pci_attach_args *pa = (struct pci_attach_args *)aux;

	if (PCI_VENDOR(pa->pa_id) != PCI_VENDOR_XGI)
		return (0);

	if (PCI_CLASS(pa->pa_class) != PCI_CLASS_DISPLAY)
		return (0);

	/*
	 * Deliberately narrower than the Linux module's pci_device_id table,
	 * which also listed the XG20 family.  xgiframe_chip_init() below only
	 * knows how to translate a device ID into a chip type for the Z11
	 * (XG27); accepting the others here would attach, print an error and
	 * return ENODEV.  Matching only what we can drive is better than
	 * claiming a device and then failing on it.
	 *
	 * 0x0027 = Z11/Z11M, the part on the HP T5325.
	 */
	return (PCI_PRODUCT(pa->pa_id) == PCI_PRODUCT_XGI_VOLARI_Z11);
}

/*
 * Identify the silicon and run the upstream init sequence.  The handles must
 * already be mapped and published via xgiframe_ioh/xgiframe_iot.
 */
static int
xgiframe_chip_init(struct xgiframe_softc *sc)
{
	struct xgifb_video_info *xi = &sc->sc_xgi;
	struct xgi_hw_device_info *hw = &xi->hw_info;
	pcireg_t id;

	/* Registers are at the relocated VGA window: BAR2 + 0x30. */
	xi->vga_base = sc->sc_ioaddr + 0x30;
	xi->video_base = sc->sc_fbaddr;
	xi->video_size = sc->sc_fbsize;
	xi->video_vbase = sc->sc_fb;

	hw->pjVideoMemoryAddress = sc->sc_fb;
	hw->ulVideoMemorySize = sc->sc_fbsize;

	/*
	 * Chip identification.  Upstream switches on the PCI device ID and,
	 * for the 0x020 part, further probes GPIO to tell XG20 from XG21.
	 * This driver only supports the 0x027 (Z11) silicon present on the
	 * HP T5325, which maps directly to chip type XG27.
	 */
	id = pci_conf_read(sc->sc_pc, sc->sc_pcitag, PCI_ID_REG);
	switch (PCI_PRODUCT(id)) {
	case PCI_PRODUCT_XGI_VOLARI_Z11:
		xi->chip = XG27;
		break;
	default:
		aprint_error_dev(sc->sc_dev,
		    "XGI device 0x%03x not supported by this driver\n",
		    (unsigned int)PCI_PRODUCT(id));
		return (ENODEV);
	}

	hw->jChipType = xi->chip;
	hw->jChipRevision = 0;

	/* No external video bridge; the DVI output is driven directly. */
	hw->ulExternalChip = 0;
	hw->ujVBChipID = VB_CHIP_UNKNOWN;
	hw->ulCRT2LCDType = 0;

	xi->display2 = XGIFB_DISP_NONE;
	xi->display2_force = false;
	xi->hasVB = HASVB_NONE;

	/*
	 * Take the strap reading before XGIInitNew() resets the extended
	 * registers, so it is on the console even if init later dies.
	 */
	xgiframe_probe_straps(sc);

	if (XGIInitNew(xi) == 0) {
		aprint_error_dev(sc->sc_dev, "XGIInitNew failed\n");
		return (ENODEV);
	}

	return (0);
}

static int
xgiframe_set_mode(struct xgiframe_softc *sc, int mode_no, int width, int height)
{
	struct xgifb_video_info *xi = &sc->sc_xgi;
	struct xgi_hw_device_info *hw = &xi->hw_info;
	bus_addr_t sr_index = xi->vga_base + 0x14;
	unsigned int linelen;

	if (XGISetModeNew(xi, hw, (unsigned short)mode_no) == 0) {
		aprint_error_dev(sc->sc_dev, "mode 0x%x (%dx%d) failed\n",
		    mode_no, width, height);
		return (EINVAL);
	}

	/*
	 * Program the scanline pitch from the fbdev line length.
	 *
	 * Upstream does this in the framebuffer glue, *after* XGISetModeNew():
	 * XGISetModeNew only adjusts the offset registers via XGI_SetCRT1Offset(),
	 * and the values it leaves in SR0E do not match the pitch rasops
	 * expects.  Without this the chip's per-row offset is ~0x6100 instead
	 * of 256, so each scanline consumes nearly the whole framebuffer and
	 * only a small centred region is ever painted -- exactly the symptom
	 * this driver has shown.  rasops16 uses one 8-byte unit per line_length
	 * count, so 256 == 2048 bytes == sc_stride.
	 */
	linelen = (width * XGI_DEFAULT_DEPTH) >> 6;   /* (1024 * 16) / 64 = 256 */
	xgiframe_cr_set(xi, 0x13, linelen & 0xff);
	xgiframe_outb(sr_index, 0x0e);
	xgiframe_outb(sr_index + 1, (linelen & 0xff00) >> 8);

	sc->sc_width = width;
	sc->sc_height = height;

	return (0);
}

/*
 * Probe both BAR windows and print what comes back.
 *
 * This has to happen before anything programs the chip, so it observes the
 * state firmware left behind.
 *
 * A read that answers 0xff is not a value: on PCIe an access the card does
 * not decode completes with an unsupported-request status and a CPU read
 * returns all ones.  Telling "unclaimed" apart from "the card answered 0xff"
 * matters because the ported code *reads* CR33 and CR38 to choose a refresh
 * rate (XGI_GetRatePtrCRT2, vb_setmode.c:5120) and a connector
 * (vb_setmode.c:5495), and getting 0xff from either does not fail -- it
 * silently selects the wrong table entry and the wrong output.  A framebuffer
 * read is included so the memory and I/O paths can be compared directly.
 */
static void
xgiframe_probe_windows(struct xgiframe_softc *sc)
{
	static const uint8_t offs[] = {
		0x00,	/* attribute controller data */
		0x10,	/* attribute index / CRTC data */
		0x14,	/* sequencer index (P3c4) */
		0x20,	/* sequencer data (P3d4) */
		0x24,	/* CRTC index (P3d4) */
		0x2a,	/* input status 1 (P3da) */
		0x44,	/* sequencer index + 0x10, i.e. P3d4 by our layout */
		0x54,	/* CRTC index + 0x10 */
	};
	unsigned int i;
	uint32_t fb[4];

	printf("%s: BAR2 (0x%lx) reads:", __func__,
	    (unsigned long)sc->sc_ioaddr);
	for (i = 0; i < __arraycount(offs); i++)
		printf(" [%02x]=%02x", offs[i],
		    bus_space_read_1(sc->sc_iot, sc->sc_ioh, offs[i]));
	printf("\n");

	for (i = 0; i < __arraycount(fb); i++)
		fb[i] = bus_space_read_4(sc->sc_memt, sc->sc_fbh, i * 4);
	printf("%s: BAR0 (0x%lx) reads: %08lx %08lx %08lx %08lx\n", __func__,
	    (unsigned long)sc->sc_fbaddr,
	    (unsigned long)fb[0], (unsigned long)fb[1],
	    (unsigned long)fb[2], (unsigned long)fb[3]);
}

/*
 * Read a CRT register: select the index, then read the data port.
 *
 * The data port is index + 1, not index + 0x10 -- that is what the ported
 * xgifb_reg_get()/xgifb_reg_set() do (vb_util.c:27-34), and using anything else
 * silently reads a different register.
 */
static uint8_t
xgiframe_cr_get(struct xgifb_video_info *xi, uint8_t index)
{
	bus_addr_t cr_index = xi->vga_base + 0x24;

	xgiframe_outb(cr_index, index);
	return xgiframe_inb(cr_index + 1);
}

static void
xgiframe_cr_set(struct xgifb_video_info *xi, uint8_t index, uint8_t val)
{
	bus_addr_t cr_index = xi->vga_base + 0x24;

	xgiframe_outb(cr_index, index);
	xgiframe_outb(cr_index + 1, val);
}

static uint8_t
xgiframe_sr_get(struct xgifb_video_info *xi, uint8_t index)
{
	bus_addr_t sr_index = xi->vga_base + 0x14;

	xgiframe_outb(sr_index, index);
	return xgiframe_inb(sr_index + 1);
}

/*
 * Read back the mode the chip actually ended up in.
 *
 * Everything about a working display is decided by XGI_SetCRT1Group()
 * (vb_setmode.c:5401), and one third of it sits behind a single guard:
 *
 *	RefreshRateTableIndex = XGI_GetRatePtrCRT2(...);
 *	if (RefreshRateTableIndex != 0xFFFF) {
 *		XGI_SetSync(...); XGI_SetCRT1CRTC(...); XGI_SetCRT1DE(...);
 *		XGI_SetCRT1Offset(...); XGI_SetCRT1VCLK(...);
 *	}
 *
 * When that index is 0xFFFF none of the timing, the memory offset or the
 * pixel clock is programmed at all, and the panel is left scanning whatever
 * the reset state produced -- a signal, but at the wrong rate, so the monitor
 * rejects the mode.  Nothing upstream reports that condition, and the ported
 * code has no way to fail loudly about it.
 *
 * So measure the result instead of trusting that the path ran.  The register
 * groups below are the ones that guard controls:
 *
 *	CR00-CR04  horizontal total/retrace, from XGI_SetCRT1Timing_H
 *	CR13       low byte of the memory offset, from XGI_SetCRT1Offset
 *	SR0E       high nibble of that offset *and* the VCLK divider select,
 *	           rewritten by both XGI_SetCRT1Offset and XGI_SetCRT1VCLK
 *	SR10       display unit, from XGI_SetCRT1Offset (mode 0x4a decrements it)
 *	SR2B/SR2C  pixel clock divider, from XGI_SetCRT1VCLK via XGI_VCLKData
 *
 * For 1024x768x16 they should hold CR00-CR04 = d7 9f 9b ac 1e, SR10 non-zero,
 * and SR2B/SR2C matching XGI_VCLKData[] for the chosen entry.  All zeros means
 * the guard failed and no mode was ever set.
 */
static void
xgiframe_probe_mode(struct xgiframe_softc *sc)
{
	struct xgifb_video_info *xi = &sc->sc_xgi;
	uint8_t cr[5], cr07, cr13, cr10;
	uint8_t sr0e, sr10, sr2b, sr2c;
	unsigned int htotal, vtotal;

	cr[0] = xgiframe_cr_get(xi, 0x00);
	cr[1] = xgiframe_cr_get(xi, 0x01);
	cr[2] = xgiframe_cr_get(xi, 0x02);
	cr[3] = xgiframe_cr_get(xi, 0x03);
	cr[4] = xgiframe_cr_get(xi, 0x04);
	cr07 = xgiframe_cr_get(xi, 0x07);
	cr13 = xgiframe_cr_get(xi, 0x13);
	cr10 = xgiframe_cr_get(xi, 0x10);

	sr0e = xgiframe_sr_get(xi, 0x0e);
	sr10 = xgiframe_sr_get(xi, 0x10);
	sr2b = xgiframe_sr_get(xi, 0x2b);
	sr2c = xgiframe_sr_get(xi, 0x2c);

	/* CR01's top two bits extend CR00, so htotal spans 10 bits. */
	htotal = ((cr[0] | ((cr[1] & 0xc0) << 4)) << 2) |
	    ((cr[1] & 0x1f) << 8) | cr[2];
	vtotal = (cr07 & 0x1f) << 8;

	aprint_normal_dev(sc->sc_dev,
	    "mode: CR00-CR04 = %02x %02x %02x %02x %02x\n",
	    cr[0], cr[1], cr[2], cr[3], cr[4]);
	aprint_normal_dev(sc->sc_dev,
	    "mode: htotal = %u, vtotal = %u, CR07 = 0x%02x, CR13 = 0x%02x, "
	    "SR0E = 0x%02x, CR10 = 0x%02x, SR10 = 0x%02x, "
	    "SR2B = 0x%02x, SR2C = 0x%02x\n",
	    htotal, vtotal, cr07, cr13, sr0e, cr10, sr10, sr2b, sr2c);

	/*
	 * The chip should be at the table[0x16] 1024x768@60 mode (CR00 = 0xa3,
	 * from XGI_CRT1Table[0x16]) and the pitch we just wrote: SR0E must read
	 * back 0x01 (line_length 256 -> (256 & 0xff00)>>8) and CR13 0x00.
	 */
	if (cr[0] != 0xa3)
		aprint_error_dev(sc->sc_dev, "CR00 = 0x%02x, expected 0xa3; "
		    "mode not programmed\n", cr[0]);
	if (sr0e != 0x01 || cr13 != 0x00)
		aprint_error_dev(sc->sc_dev,
		    "SR0E = 0x%02x, CR13 = 0x%02x; expected SR0E = 0x01, CR13 = 0x00\n",
		    sr0e, cr13);
	if (cr[0] == 0xa3 && sr0e == 0x01 && cr13 == 0x00)
		aprint_normal_dev(sc->sc_dev, "timing and pitch programmed\n");
}

/*
 * Report the board's display straps.
 *
 * GetXG27Sense() reads CR48's GPIO pins to decide LVDS vs TMDS/DVI, and
 * XGI_DisplayOn() acts on that decision.  Take the reading here, before
 * XGIInitNew() resets the extended registers, so it survives whatever happens
 * next: if the chip drives the wrong connector that is board wiring, and it
 * has to be visible without depending on the rest of init succeeding.
 */
static void
xgiframe_probe_straps(struct xgiframe_softc *sc)
{
	struct xgifb_video_info *xi = &sc->sc_xgi;
	uint8_t cr4a, gpio;

	/* CR4A gates the GPIO pins as inputs; read them without leaving it on. */
	cr4a = xgiframe_cr_get(xi, 0x4a);
	xgiframe_cr_set(xi, 0x4a, cr4a | 0x07);
	gpio = xgiframe_cr_get(xi, 0x48) & 0x07;
	xgiframe_cr_set(xi, 0x4a, cr4a);

	/*
	 * vb_init.c:1054 -- GetXG27Sense() treats anything above 0x02 as
	 * TMDS/DVO and anything at or below as LVDS.
	 */
	aprint_normal_dev(sc->sc_dev, "display straps: CR48[2:0] = 0x%02x "
	    "(%s)\n", gpio, gpio <= 0x02 ? "LVDS" : "DVI");
}

/*
 * Report what the chip says about itself, so a dark screen is diagnosable.
 *
 * Two questions matter and neither is visible from the outside:
 *
 *  - Is the register window really the card, or the bridge answering on its
 *    behalf?  XGIInitNew()'s "openkey" step writes 0x86 to SR05 and the chip
 *    read-modify-writes it back as 0xa1; upstream checks for exactly this and
 *    bails out with "I/O error" (XGI_main_26.c:1701).  Anything other than
 *    0xa1 means no register programming in this driver reached the silicon.
 *
 *  - Did the chip's own sense logic choose DVI or LVDS?  GetXG27Sense() sets
 *    CR38[7:5] to 0xa0 for TMDS/DVO and 0xc0 for LVDS, and XGI_DisplayOn()
 *    brings up the DVI or the LVDS backlight based on that same reading
 *    (vb_setmode.c:5494, 2346).
 */
static void
xgiframe_probe_chip(struct xgiframe_softc *sc)
{
	struct xgifb_video_info *xi = &sc->sc_xgi;
	uint8_t sr05, cr33, cr38, cr30;

	sr05 = xgiframe_sr_get(xi, 0x05);
	if (sr05 == 0xa1)
		aprint_normal_dev(sc->sc_dev,
		    "register window responding (SR05 = 0xa1)\n");
	else
		aprint_error_dev(sc->sc_dev, "SR05 read back 0x%02x, not 0xa1: "
		    "register window is not responding\n", sr05);

	/*
	 * CR33[3:0] is the refresh-rate index.  It must read 0: XGI_GetRatePtrCRT2
	 * decrements it once if non-zero and then walks that many extra entries
	 * through XGI330_RefIndex (vb_setmode.c:5120-5169), so a stray non-zero
	 * value silently programs CR00-CR23 and the pixel clock from the wrong
	 * table entry and produces a garbage display rather than an error.
	 */
	cr33 = xgiframe_cr_get(xi, 0x33);

	cr38 = xgiframe_cr_get(xi, 0x38);
	cr30 = xgiframe_cr_get(xi, 0x30);

	aprint_normal_dev(sc->sc_dev,
	    "CR33 = 0x%02x (refresh index must be 0)\n", cr33);
	aprint_normal_dev(sc->sc_dev,
	    "connector: %s (CR38 = 0x%02x, CR30 = 0x%02x)\n",
	    (cr38 & 0xe0) == 0xc0 && (cr30 & 0x20) ? "LVDS" : "DVI",
	    cr38, cr30);

	xgiframe_probe_mode(sc);
}

/*
 * Make sure a BAR points somewhere the parent bridge actually decodes.
 *
 * U-Boot programs the XGI's BAR0 to 0x40000000, which is nowhere near the
 * Kirkwood's PEX0_MEM window (0xe8000000 + 128 MB), so the framebuffer maps
 * to an address the chip does not decode and the display stays dark.
 * pciconf tries to reallocate it, fails ("Failed to allocate PCI memory
 * space"), and leaves the bad value in place -- so the driver has to fix it.
 *
 * baridx is the BAR *index* (0, 1, 2 ...); PCI_BAR() is a conversion macro,
 * not an offset, and must be applied exactly once.
 *
 * win_start/win_end come from the iostart/ioend/memstart/memend properties the
 * Kirkwood machdep publishes on the PEX bridge, so the addresses follow the SoC
 * rather than being hard-coded.
 */
static void
xgiframe_claim_bar(device_t self, pci_chipset_tag_t pc, pcitag_t tag,
    int baridx, uint64_t win_start, uint64_t win_end, bool iospace)
{
	pcireg_t reg, orig, mask, csr, newval;
	uint64_t addr, size;

	reg = PCI_BAR(baridx);

	/*
	 * Size probe, following pci_io_find()/pci_mem_find(): decoding must be
	 * turned off first, or writing all-ones latches a bogus decode address
	 * on the device.  csr is restored afterwards.
	 */
	csr = pci_conf_read(pc, tag, PCI_COMMAND_STATUS_REG);
	orig = pci_conf_read(pc, tag, reg);
	pci_conf_write(pc, tag, PCI_COMMAND_STATUS_REG,
	    csr & ~(pcireg_t)(iospace ? PCI_COMMAND_IO_ENABLE :
	    PCI_COMMAND_MEM_ENABLE));
	pci_conf_write(pc, tag, reg, 0xffffffff);
	mask = pci_conf_read(pc, tag, reg);
	pci_conf_write(pc, tag, reg, orig);
	pci_conf_write(pc, tag, PCI_COMMAND_STATUS_REG, csr);

	if (iospace) {
		addr = PCI_MAPREG_IO_ADDR(orig);
		size = PCI_MAPREG_IO_SIZE(mask);
	} else {
		addr = PCI_MAPREG_MEM_ADDR(orig);
		size = PCI_MAPREG_MEM_SIZE(mask);
	}

	aprint_normal_dev(self, "BAR%d: offset 0x%02lx raw 0x%08lx "
	    "addr 0x%llx size 0x%llx%s\n", baridx, (unsigned long)reg,
	    (unsigned long)orig, (unsigned long long)addr,
	    (unsigned long long)size,
	    (size != 0 && addr >= win_start && addr + size <= win_end) ?
	    " (already in window)" : "");

	if (size == 0) {
		aprint_error_dev(self, "BAR%d is not implemented\n", baridx);
		return;
	}

	if (addr >= win_start && addr + size <= win_end) {
		/* Already decodable. */
		return;
	}

	/* 1 MB-aligned address at the bottom of the window. */
	addr = (win_start + 0xfffff) & ~0xfffffULL;

	/*
	 * Replace only the address field.  The low type/flag bits (I/O
	 * indicator, prefetchable) have to survive, so mask them out of the
	 * original rather than building the value from the address alone.
	 */
	newval = iospace ?
	    (orig & ~(pcireg_t)PCI_MAPREG_IO_ADDR_MASK) :
	    (orig & ~(pcireg_t)PCI_MAPREG_MEM_ADDR_MASK);
	newval |= (pcireg_t)addr;

	aprint_normal_dev(self,
	    "BAR%d base 0x%llx outside window 0x%llx-0x%llx, "
	    "reprogramming to 0x%llx\n", baridx,
	    (unsigned long long)(iospace ? PCI_MAPREG_IO_ADDR(orig) :
	    PCI_MAPREG_MEM_ADDR(orig)),
	    (unsigned long long)win_start, (unsigned long long)win_end,
	    (unsigned long long)addr);

	pci_conf_write(pc, tag, reg, newval);
}

static void
xgiframe_attach(device_t parent, device_t self, void *aux)
{
	struct xgiframe_softc *sc = device_private(self);
	struct pci_attach_args *pa = (struct pci_attach_args *)aux;
	struct rasops_info *ri;
	struct wsemuldisplaydev_attach_args aa;
	prop_dictionary_t dict;
	prop_dictionary_t pexdict;
	uint64_t mem_start = 0, mem_end = 0, io_start = 0, io_end = 0;
	pcireg_t csr;
	unsigned long defattr;
	bool is_console = false;
	int reg;

	sc->sc_dev = self;
	sc->sc_pc = pa->pa_pc;
	sc->sc_pcitag = pa->pa_tag;
	sc->sc_iot = pa->pa_iot;
	sc->sc_memt = pa->pa_memt;

	pci_aprint_devinfo(pa, NULL);

	dict = device_properties(self);
	if (!prop_dictionary_get_bool(dict, "is_console", &is_console))
		is_console = true;

	/*
	 * Point the chip's BARs at the windows the PEX bridge decodes.  This
	 * has to happen before pci_mapreg_info(), which would otherwise report
	 * the firmware-assigned addresses.
	 */
	pexdict = device_properties(device_parent(parent));
	if (prop_dictionary_get_uint64(pexdict, "memstart", &mem_start) &&
	    prop_dictionary_get_uint64(pexdict, "memend", &mem_end))
		xgiframe_claim_bar(self, sc->sc_pc, sc->sc_pcitag, 0,
		    mem_start, mem_end, false);
	else
		aprint_error_dev(self, "no memstart/memend from PEX bridge\n");

	if (prop_dictionary_get_uint64(pexdict, "iostart", &io_start) &&
	    prop_dictionary_get_uint64(pexdict, "ioend", &io_end))
		xgiframe_claim_bar(self, sc->sc_pc, sc->sc_pcitag, 2,
		    io_start, io_end, true);
	else
		aprint_error_dev(self, "no iostart/ioend from PEX bridge\n");

	/*
	 * Turn on the chip's address decoding.
	 *
	 * pciconf only sets PCI_COMMAND_IO_ENABLE/PCI_COMMAND_MEM_ENABLE on
	 * each device *after* it has successfully allocated the bus ranges
	 * (pciconf.c:1302); its setup_memwins() fails on this board, so it
	 * bails out at pciconf.c:1296 having never reached that loop.  The
	 * device is therefore left with decoding off, and every access to
	 * either window is answered by the bridge instead of the card -- which
	 * is why an unfixed build shows a dark screen no matter what the
	 * register writes say.
	 *
	 * Upstream gets this from pci_enable_device().  We reach the BARs
	 * through pci_mapreg_info() + bus_space_map() rather than
	 * pci_mapreg_map(), which is the only path that would have enabled
	 * decoding as a side effect, so do it explicitly -- after the BARs are
	 * programmed, since decoding a half-configured address is pointless.
	 */
	csr = pci_conf_read(sc->sc_pc, sc->sc_pcitag, PCI_COMMAND_STATUS_REG);
	if ((csr & (PCI_COMMAND_IO_ENABLE | PCI_COMMAND_MEM_ENABLE)) !=
	    (PCI_COMMAND_IO_ENABLE | PCI_COMMAND_MEM_ENABLE)) {
		csr |= PCI_COMMAND_IO_ENABLE | PCI_COMMAND_MEM_ENABLE;
		pci_conf_write(sc->sc_pc, sc->sc_pcitag,
		    PCI_COMMAND_STATUS_REG, csr);
		aprint_normal_dev(self,
		    "enabled PCI IO and memory decoding\n");
	}

	/*
	 * BAR2 is the legacy register window: PCI I/O space at the base of the
	 * Kirkwood's PEX0_IO window.  See XGI_REG_MAP_SIZE for why more than
	 * the BAR's nominal size is mapped.
	 */
	if (pci_mapreg_info(sc->sc_pc, sc->sc_pcitag, PCI_BAR(2),
	    PCI_MAPREG_TYPE_IO, &sc->sc_ioaddr, &sc->sc_iosize, &reg)) {
		aprint_error_dev(self, "no I/O BAR (registers)\n");
		return;
	}
	sc->sc_iomapsize = XGI_REG_MAP_SIZE;
	if (bus_space_map(sc->sc_iot, sc->sc_ioaddr, sc->sc_iomapsize,
	    0, &sc->sc_ioh)) {
		aprint_error_dev(self, "can't map registers\n");
		return;
	}

	/* BAR0 is the linear framebuffer. */
	if (pci_mapreg_info(sc->sc_pc, sc->sc_pcitag, PCI_BAR(0),
	    PCI_MAPREG_TYPE_MEM, &sc->sc_fbaddr, &sc->sc_fbsize, &reg)) {
		aprint_error_dev(self, "no framebuffer BAR\n");
		goto unmap_io;
	}
	if (sc->sc_fbsize < 1024 * 1024) {
		aprint_error_dev(self, "framebuffer BAR too small\n");
		goto unmap_io;
	}

	/*
	 * Map the whole aperture, not just the part PEX0_MEM decodes.
	 *
	 * BAR0's mask claims 64 MB while initarm() programs the PEX0_MEM
	 * window to MARVELL_PEXMEM_SIZE, which is 16 MB (memstart/memend
	 * above), so the top 48 MB is unclaimed PCI space.  Truncating the
	 * mapping there is wrong all the same: XGINew_ReadWriteRest() sizes
	 * the DRAM by writing a marker at fbaddr + (1 << i) for i = 5..26
	 * and reading it back, and the read failing past the end of real
	 * video memory is precisely how it discovers how much VRAM the chip
	 * has (vb_init.c:551-569).  Cut the mapping short and it takes a data
	 * abort on the marker at 1 << 24 instead.  Upstream maps the full
	 * pci_resource_len() and relies on the same read-back, so do that;
	 * a non-fatal unsupported-request read returns all ones and a write
	 * is dropped, so nothing there can hang.
	 */
	sc->sc_fbmapsize = sc->sc_fbsize;

	if (bus_space_map(sc->sc_memt, sc->sc_fbaddr, sc->sc_fbmapsize,
	    BUS_SPACE_MAP_LINEAR, &sc->sc_fbh)) {
		aprint_error_dev(self, "can't map framebuffer\n");
		goto unmap_io;
	}

	sc->sc_fb = bus_space_vaddr(sc->sc_memt, sc->sc_fbh);

	aprint_normal_dev(self, "XGI Volari: regs 0x%lx, fb 0x%lx (%lu KB)\n",
	    (unsigned long)sc->sc_ioaddr, (unsigned long)sc->sc_fbaddr,
	    (unsigned long)(sc->sc_fbmapsize >> 10));

	/*
	 * is_console decides whether anything is ever rendered.  Without it
	 * attach() skips both eraserows() and wsdisplay_cnattach(), so VRAM keeps
	 * whatever the DRAM sizing left in it -- power-of-two markers from
	 * XGINew_ReadWriteRest() at fbaddr + (1 << i) -- and the panel scans out
	 * that unchanged instead of a console.  Say which way it went, because
	 * "the display is full of garbage" looks identical either way.
	 */
	aprint_normal_dev(self, "is_console = %s\n", is_console ? "yes" : "no");

	/* Publish the mapped handles for the ported register accessors. */
	xgiframe_iot = sc->sc_iot;
	xgiframe_ioh = sc->sc_ioh;
	xgiframe_iobase = sc->sc_ioaddr;

	xgiframe_probe_windows(sc);

	if (xgiframe_chip_init(sc) != 0)
		goto unmap_fb;

	sc->sc_depth = XGI_DEFAULT_DEPTH;
	sc->sc_mode = WSDISPLAYIO_MODE_EMUL;

	if (xgiframe_set_mode(sc, XGI_MODE_DEFAULT,
	    XGI_DEFAULT_WIDTH, XGI_DEFAULT_HEIGHT) != 0)
		goto unmap_fb;

	xgiframe_probe_chip(sc);

	sc->sc_stride = XGI_DEFAULT_STRIDE;

	sc->sc_defaultscreen_descr = (struct wsscreen_descr){
		"default",
		0, 0,
		NULL,
		8, 16,
		WSSCREEN_WSCOLORS | WSSCREEN_RESIZE,
		NULL
	};
	sc->sc_screens[0] = &sc->sc_defaultscreen_descr;
	sc->sc_screenlist = (struct wsscreen_list){1, sc->sc_screens};

	vcons_init(&sc->sc_vd, sc, &sc->sc_defaultscreen_descr,
	    &xgiframe_accessops);
	sc->sc_vd.init_screen = xgiframe_init_screen;

	ri = &sc->sc_console_screen.scr_ri;

	if (is_console) {
		vcons_init_screen(&sc->sc_vd, &sc->sc_console_screen, 1,
		    &defattr);
		sc->sc_console_screen.scr_flags |= VCONS_SCREEN_IS_STATIC;

		/*
		 * Clear through rasops rather than memset(): at 16bpp the
		 * cell is a 16-bit RGB565 word built from the attribute's
		 * palette index, so a byte memset of the framebuffer would
		 * fill the wrong pattern.  eraserows() takes the same
		 * (rows, row, attr) triple wscons uses.
		 */
		ri->ri_ops.eraserows(ri, 0, ri->ri_rows, defattr);

		sc->sc_defaultscreen_descr.textops = &ri->ri_ops;
		sc->sc_defaultscreen_descr.capabilities = ri->ri_caps;
		sc->sc_defaultscreen_descr.nrows = ri->ri_rows;
		sc->sc_defaultscreen_descr.ncols = ri->ri_cols;

		wsdisplay_cnattach(&sc->sc_defaultscreen_descr, ri, 0, 0,
		    defattr);
		vcons_replay_msgbuf(&sc->sc_console_screen);
	} else {
		vcons_init_screen(&sc->sc_vd, &sc->sc_console_screen, 1,
		    &defattr);
	}

	aa.console = is_console;
	aa.scrdata = &sc->sc_screenlist;
	aa.accessops = &xgiframe_accessops;
	aa.accesscookie = &sc->sc_vd;

	config_found(self, &aa, wsemuldisplaydevprint, CFARGS_NONE);
	return;

unmap_fb:
	bus_space_unmap(sc->sc_memt, sc->sc_fbh, sc->sc_fbmapsize);
unmap_io:
	bus_space_unmap(sc->sc_iot, sc->sc_ioh, sc->sc_iomapsize);
}

static void
xgiframe_init_screen(void *cookie, struct vcons_screen *scr,
    int existing, long *defattr)
{
	struct xgiframe_softc *sc = cookie;
	struct rasops_info *ri = &scr->scr_ri;

	/*
	 * rasops_init() below already sets these to what we want; do not
	 * overwrite ri_caps afterwards or we would discard WSSCREEN_REVERSE,
	 * WSSCREEN_HILIT and WSSCREEN_UNDERLINE, which rasops only reports for
	 * depth >= 8.  rasops_init() also chooses the font, so ri_font is
	 * valid on return -- hence the rasops_reconfig() that follows, which
	 * sets ri_cols/ri_rows from the emulated geometry.
	 */
	ri->ri_depth = sc->sc_depth;
	ri->ri_width = sc->sc_width;
	ri->ri_height = sc->sc_height;
	ri->ri_stride = sc->sc_stride;
	ri->ri_bits = sc->sc_fb;
	ri->ri_flg = RI_CENTER;
	ri->ri_hw = scr;

	/*
	 * rasops_init() dispatches on ri_depth, and for depth 16 rasops15_init()
	 * derives rpos=11/gnum=6/bpos=0 -- i.e. RGB565, which is exactly the
	 * pixel format the driver programs for 16bpp modes (XGIfb_bpp_to_var:
	 * red 11/5, green 5/6, blue 0/5).  No RI_RGB565 equivalent exists, and
	 * none is needed.
	 *
	 * Do not set RI_8BIT_IS_RGB: that declares an 8bpp palette whose DAC
	 * entries are direct RGB, which is not the case here.
	 */
	rasops_init(ri, 0, 0);

	scr->scr_flags |= VCONS_LOADFONT;
	scr->scr_flags |= VCONS_DONT_READ;

	/*
	 * rasops_init() has already called rasops_reconfig() (rasops.c:352),
	 * which fitted the emulated console inside ri_width/ri_height and set
	 * ri_rows/ri_cols.  Calling it again with the full pixel dimensions as
	 * rows/cols would ask for a console of 768x1024 characters, which
	 * rasops_reconfig() clamps to what fits -- but that only works by luck
	 * of the clamping, so don't.  The geometry is already correct.
	 */
}

static int
xgiframe_ioctl(void *v, void *vs, u_long cmd, void *data, int flag, struct lwp *l)
{
	struct vcons_data *vd = v;
	struct xgiframe_softc *sc = vd->cookie;
	struct wsdisplay_fbinfo *wdf;
	struct vcons_screen *ms = vd->active;

	switch (cmd) {
	case WSDISPLAYIO_GTYPE:
		*(u_int *)data = WSDISPLAY_TYPE_PCIMISC;
		return (0);

	case PCI_IOC_CFGREAD:
	case PCI_IOC_CFGWRITE:
		return pci_devioctl(sc->sc_pc, sc->sc_pcitag,
		    cmd, data, flag, l);

	case WSDISPLAYIO_GET_BUSID:
		return wsdisplayio_busid_pci(sc->sc_dev, sc->sc_pc,
		    sc->sc_pcitag, data);

	case WSDISPLAYIO_GINFO:
		if (ms == NULL)
			return (ENODEV);
		wdf = (void *)data;
		wdf->height = ms->scr_ri.ri_height;
		wdf->width = ms->scr_ri.ri_width;
		wdf->depth = ms->scr_ri.ri_depth;
		/* rasops15 advertises WSSCREEN_256COL; 16bpp is RGB565. */
		wdf->cmsize = 256;
		return (0);

	case WSDISPLAYIO_LINEBYTES:
		*(u_int *)data = sc->sc_stride;
		return (0);

	case WSDISPLAYIO_SMODE: {
		int new_mode = *(int *)data;

		if (new_mode != sc->sc_mode) {
			sc->sc_mode = new_mode;
			if (new_mode == WSDISPLAYIO_MODE_EMUL) {
				(void) xgiframe_set_mode(sc, XGI_MODE_DEFAULT,
				    XGI_DEFAULT_WIDTH, XGI_DEFAULT_HEIGHT);
				if (ms != NULL) {
					struct rasops_info *ri = &ms->scr_ri;

					ri->ri_ops.eraserows(ri, 0, ri->ri_rows,
					    ms->scr_defattr);
					vcons_redraw_screen(ms);
				}
			}
		}
		return (0);
	}

	case WSDISPLAYIO_GET_FBINFO: {
		struct wsdisplayio_fbinfo *fbi = data;
		return wsdisplayio_get_fbinfo(&ms->scr_ri, fbi);
	}
	}

	return (EPASSTHROUGH);
}

static paddr_t
xgiframe_mmap(void *v, void *vs, off_t offset, int prot)
{
	struct vcons_data *vd = v;
	struct xgiframe_softc *sc = vd->cookie;
	paddr_t pa;

	/*
	 * Bound this by the mapped size rather than sc_fbsize: BAR0 claims
	 * more aperture than the PEX0_MEM window decodes, and mapping the
	 * undecodable remainder would hand out addresses that fault on touch.
	 */
	if (offset < sc->sc_fbmapsize) {
		pa = bus_space_mmap(sc->sc_memt, sc->sc_fbaddr + offset,
		    0, prot, BUS_SPACE_MAP_LINEAR);
		return (pa);
	}

	if (kauth_authorize_machdep(kauth_cred_get(),
	    KAUTH_MACHDEP_UNMANAGEDMEM, NULL, NULL, NULL, NULL) != 0) {
		aprint_normal_dev(sc->sc_dev, "mmap() rejected\n");
		return (-1);
	}

	return (-1);
}
