#ifndef _VBINIT_
#define _VBINIT_

/*
 * NetBSD port: the upstream signature took a Linux "struct pci_dev *";
 * here the caller passes the driver context directly.
 */
struct xgifb_video_info;

extern unsigned char XGIInitNew(struct xgifb_video_info *);
extern void XGIRegInit(struct vb_device_info *, unsigned long);

#endif
