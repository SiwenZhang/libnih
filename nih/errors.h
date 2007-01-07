/* libnih
 *
 * Copyright © 2006 Scott James Remnant <scott@netsplit.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef NIH_ERRORS_H
#define NIH_ERRORS_H

#include <nih/error.h>

#include <errno.h>


/* Allocated error numbers */
enum {
	/* 0x0000 thru 0xFFFF reserved for errno */
	NIH_ERROR_ERRNO_START = 0x0000L,

	/* 0x10000 thru 0x1FFFF reserved for libnih */
	NIH_ERROR_LIBNIH_START = 0x10000L,

	NIH_CONFIG_TOO_LONG,
	NIH_CONFIG_TRAILING_SLASH,
	NIH_CONFIG_UNTERMINATED_QUOTE,
	NIH_CONFIG_UNTERMINATED_BLOCK,
	NIH_CONFIG_EXPECTED_STANZA,
	NIH_CONFIG_UNKNOWN_STANZA,

	/* 0x20000 thru 0x2FFFF reserved for applications */
	NIH_ERROR_APPLICATION_START = 0x20000L,

	/* 0x30000 upwards for other libraries */
	NIH_ERROR_LIBRARY_START = 0x30000L
};

/* Error strings for defined messages */
#define NIH_CONFIG_TOO_LONG_STR            N_("file is too long")
#define NIH_CONFIG_TRAILING_SLASH_STR	   N_("trailing slash in file")
#define NIH_CONFIG_UNTERMINATED_QUOTE_STR  N_("unterminated quoted string")
#define NIH_CONFIG_UNTERMINATED_BLOCK_STR  N_("unterminated block")
#define NIH_CONFIG_EXPECTED_STANZA_STR     N_("expected stanza")
#define NIH_CONFIG_UNKNOWN_STANZA_STR	   N_("unknown stanza")

#endif /* NIH_ERRORS_H */
