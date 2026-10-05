/*	$NetBSD$
 *
 * vb_util.c -- index/data register access for the ported XGI Volari code.
 *
 * Ported from the Linux xgifb driver's vb_util.c.  The register access
 * itself is unchanged; only the underlying I/O primitives differ (see
 * xgiframe_io.h).
 */

#include <sys/cdefs.h>
#include <sys/types.h>

#include "xgiframe_io.h"

#include "vgatypes.h"
#include "vb_util.h"

/* set index/data register pair (port, port+1) */
void
xgifb_reg_set(unsigned long port, uint8_t index, uint8_t data)
{
	outb(index, port);
	outb(data, port + 1);
}

uint8_t
xgifb_reg_get(unsigned long port, uint8_t index)
{
	uint8_t data;

	outb(index, port);
	data = inb(port + 1);
	return data;
}

void
xgifb_reg_and_or(unsigned long port, uint8_t index,
    unsigned data_and, unsigned data_or)
{
	uint8_t temp;

	temp = xgifb_reg_get(port, index);
	temp = (temp & data_and) | data_or;
	xgifb_reg_set(port, index, temp);
}

void
xgifb_reg_and(unsigned long port, uint8_t index, unsigned data_and)
{
	uint8_t temp;

	temp = xgifb_reg_get(port, index);
	temp &= data_and;
	xgifb_reg_set(port, index, temp);
}

void
xgifb_reg_or(unsigned long port, uint8_t index, unsigned data_or)
{
	uint8_t temp;

	temp = xgifb_reg_get(port, index);
	temp |= data_or;
	xgifb_reg_set(port, index, temp);
}
