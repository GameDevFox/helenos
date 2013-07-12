/*
 * Copyright (c) 2013 Antonin Steinhauser
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
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

/** @addtogroup inet
 * @{
 */
/**
 * @file
 * @brief
 */

#include <adt/list.h>
#include <errno.h>
#include <fibril_synch.h>
#include <inet/iplink_srv.h>
#include <stdlib.h>
#include "ntrans.h"

/** Address translation list (of inet_ntrans_t) */
static FIBRIL_MUTEX_INITIALIZE(ntrans_list_lock);
static LIST_INITIALIZE(ntrans_list);
static FIBRIL_CONDVAR_INITIALIZE(ntrans_cv);

static inet_ntrans_t *ntrans_find(addr128_t ip_addr)
{
	list_foreach(ntrans_list, link) {
		inet_ntrans_t *ntrans = list_get_instance(link,
		    inet_ntrans_t, ntrans_list);

		if (addr128_compare(ntrans->ip_addr, ip_addr))
			return ntrans;
	}

	return NULL;
}

int ntrans_add(addr128_t ip_addr, addr48_t mac_addr)
{
	inet_ntrans_t *ntrans;
	inet_ntrans_t *prev;

	ntrans = calloc(1, sizeof(inet_ntrans_t));
	if (ntrans == NULL)
		return ENOMEM;

	addr128(ip_addr, ntrans->ip_addr);
	addr48(mac_addr, ntrans->mac_addr);

	fibril_mutex_lock(&ntrans_list_lock);
	prev = ntrans_find(ip_addr);
	if (prev != NULL) {
		list_remove(&prev->ntrans_list);
		free(prev);
	}

	list_append(&ntrans->ntrans_list, &ntrans_list);
	fibril_mutex_unlock(&ntrans_list_lock);
	fibril_condvar_broadcast(&ntrans_cv);

	return EOK;
}

int ntrans_remove(addr128_t ip_addr)
{
	inet_ntrans_t *ntrans;

	fibril_mutex_lock(&ntrans_list_lock);
	ntrans = ntrans_find(ip_addr);
	if (ntrans == NULL) {
		fibril_mutex_unlock(&ntrans_list_lock);
		return ENOENT;
	}

	list_remove(&ntrans->ntrans_list);
	fibril_mutex_unlock(&ntrans_list_lock);
	free(ntrans);

	return EOK;
}

int ntrans_lookup(addr128_t ip_addr, addr48_t mac_addr)
{
	fibril_mutex_lock(&ntrans_list_lock);
	inet_ntrans_t *ntrans = ntrans_find(ip_addr);
	if (ntrans == NULL) {
		fibril_mutex_unlock(&ntrans_list_lock);
		return ENOENT;
	}
	
	fibril_mutex_unlock(&ntrans_list_lock);
	addr48(ntrans->mac_addr, mac_addr);
	return EOK;
}

int ntrans_wait_timeout(suseconds_t timeout)
{
	fibril_mutex_lock(&ntrans_list_lock);
	int rc = fibril_condvar_wait_timeout(&ntrans_cv, &ntrans_list_lock,
	    timeout);
	fibril_mutex_unlock(&ntrans_list_lock);
	
	return rc;
}

/** @}
 */