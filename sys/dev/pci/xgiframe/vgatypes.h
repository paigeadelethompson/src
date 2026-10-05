#ifndef _VGATYPES_
#define _VGATYPES_

#include "xgiframe_compat.h"

/* enum XGI_VB_CHIP_TYPE is defined in XGIfb.h. */

struct xgi_hw_device_info {
	unsigned long ulExternalChip; /* NO VB or other video bridge*/
				      /* if ujVBChipID = VB_CHIP_UNKNOWN, */

	void *pjVideoMemoryAddress;	/* base virtual memory address */
					/* of Linear VGA memory */

	unsigned long ulVideoMemorySize; /* size, in bytes, of the
					    memory on the board */

	unsigned char jChipType; /* Used to Identify Graphics Chip */
				 /* defined in the data structure type  */
				 /* "XGI_CHIP_TYPE" */

	unsigned char jChipRevision; /* Used to Identify Graphics
					Chip Revision */

	unsigned char ujVBChipID; /* the ID of video bridge */
				  /* defined in the data structure type */
				  /* "XGI_VB_CHIP_TYPE" */

	unsigned long ulCRT2LCDType; /* defined in the data structure type */
};

/* Additional IOCTL for communication xgifb <> X driver        */
/* If changing this, xgifb.h must also be changed (for xgifb) */
#endif

