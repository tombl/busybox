/* vi: set sw=4 ts=4: */
/*
 * Utility routines.
 *
 * Copyright (C) 2008 by Denys Vlasenko <vda.linux@googlemail.com>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */
#include "libbb.h"

/* Keeping it separate allows to NOT pull in stdio for VERY small applets.
 * Try building busybox with only "true" enabled... */

void (*die_func)(void);

void FAST_FUNC bb_exit(int status)
{
	if (bb_vforked_child())
		_exit(status);
	/* The parentheses keep libbb.h's exit() macro from expanding. */
	(exit)(status);
}

void FAST_FUNC xfunc_die(void)
{
	/* die_func longjmps back into, or cleans up after, the parent's
	 * code (a NOFORK applet's caller, a terminal to restore, a temporary
	 * file to remove), which a vforked child must leave alone. */
	if (die_func && !bb_vforked_child())
		die_func();
	exit(xfunc_error_retval);
}
