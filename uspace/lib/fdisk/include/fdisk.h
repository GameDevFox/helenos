/*
 * Copyright (c) 2015 Jiri Svoboda
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

/** @addtogroup libfdisk
 * @{
 */
/**
 * @file Disk management library.
 */

#ifndef LIBFDISK_FDISK_H_
#define LIBFDISK_FDISK_H_

#include <loc.h>
#include <types/fdisk.h>

extern int fdisk_dev_list_get(fdisk_dev_list_t **);
extern void fdisk_dev_list_free(fdisk_dev_list_t *);
extern fdisk_dev_info_t *fdisk_dev_first(fdisk_dev_list_t *);
extern fdisk_dev_info_t *fdisk_dev_next(fdisk_dev_info_t *);
extern int fdisk_dev_get_svcname(fdisk_dev_info_t *, char **);
extern void fdisk_dev_get_svcid(fdisk_dev_info_t *, service_id_t *);
extern int fdisk_dev_capacity(fdisk_dev_info_t *, fdisk_cap_t *);

extern int fdisk_dev_open(service_id_t, fdisk_dev_t **);
extern void fdisk_dev_close(fdisk_dev_t *);

extern int fdisk_label_get_info(fdisk_dev_t *, fdisk_label_info_t *);
extern int fdisk_label_create(fdisk_dev_t *, fdisk_label_type_t);
extern int fdisk_label_destroy(fdisk_dev_t *);

extern fdisk_part_t *fdisk_part_first(fdisk_dev_t *);
extern fdisk_part_t *fdisk_part_next(fdisk_part_t *);
extern int fdisk_part_get_max_avail(fdisk_dev_t *, fdisk_cap_t *);
extern int fdisk_part_create(fdisk_dev_t *, fdisk_partspec_t *,
    fdisk_part_t **);
extern int fdisk_part_destroy(fdisk_part_t *);

extern int fdisk_cap_format(fdisk_cap_t *, char **);
extern int fdisk_ltype_format(fdisk_label_type_t, char **);

#endif

/** @}
 */
