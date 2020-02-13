/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

#include <inttypes.h>
#include <unistd.h>

#include <isc/meminfo.h>
#if defined(HAVE_SYS_SYSCTL_H) && !defined(__linux__)
#include <sys/sysctl.h>
#endif /* if defined(HAVE_SYS_SYSCTL_H) && !defined(__linux__) */

uint64_t
isc_meminfo_totalphys(void)
{
#if defined(CTL_HW) && (defined(HW_PHYSMEM64) || defined(HW_MEMSIZE))
	int mib[2];
	mib[0] = CTL_HW;
#if defined(HW_MEMSIZE)
	mib[1] = HW_MEMSIZE;
#elif defined(HW_PHYSMEM64)
	mib[1] = HW_PHYSMEM64;
#endif /* if defined(HW_MEMSIZE) */
	uint64_t size = 0;
	size_t	 len = sizeof(size);
	if (sysctl(mib, 2, &size, &len, NULL, 0) == 0) {
		return (size);
	}
#endif /* if defined(CTL_HW) && (defined(HW_PHYSMEM64) || defined(HW_MEMSIZE)) \
	* */
#if defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
	return ((size_t)(sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE)));
#endif /* if defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE) */
	return (0);
}
