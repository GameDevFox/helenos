/*
 * Copyright (c) 2009 Lukas Mejdrech
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

/** @addtogroup lo
 *  @{
 */

/** @file
 *  Loopback network interface implementation.
 */

#include <async.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <ipc/ipc.h>
#include <ipc/services.h>

#include "../../err.h"
#include "../../messages.h"
#include "../../modules.h"

#include "../../structures/measured_strings.h"
#include "../../structures/packet/packet_client.h"

#include "../../include/device.h"
#include "../../include/nil_interface.h"

#include "../../nil/nil_messages.h"

#include "../netif.h"
#include "../netif_module.h"

/** Default hardware address.
 */
#define DEFAULT_ADDR		"\0\0\0\0\0\0"

/** Default address length.
 */
#define DEFAULT_ADDR_LEN	( sizeof( DEFAULT_ADDR ) / sizeof( char ))

/** Loopback module name.
 */
#define NAME	"lo - loopback interface"

/** Network interface global data.
 */
netif_globals_t	netif_globals;

/** Changes the loopback state.
 *  @param[in] device The device structure.
 *  @param[in] state The new device state.
 *  @returns The new state if changed.
 *  @returns EOK otherwise.
 */
int	change_state_message( device_ref device, device_state_t state );

/** Creates and returns the loopback network interface structure.
 *  @param[in] device_id The new devce identifier.
 *  @param[out] device The device structure.
 *  @returns EOK on success.
 *  @returns EXDEV if one loopback network interface already exists.
 *  @returns ENOMEM if there is not enough memory left.
 */
int	create( device_id_t device_id, device_ref * device );

/** Prints the module name.
 *  @see NAME
 */
void	module_print_name( void );

int netif_specific_message( ipc_callid_t callid, ipc_call_t * call, ipc_call_t * answer, int * answer_count ){
	return ENOTSUP;
}

int netif_get_addr_message( device_id_t device_id, measured_string_ref address ){
	if( ! address ) return EBADMEM;
	address->value = str_dup(DEFAULT_ADDR);
	address->length = DEFAULT_ADDR_LEN;
	return EOK;
}

int netif_get_device_stats( device_id_t device_id, device_stats_ref stats ){
	ERROR_DECLARE;

	device_ref	device;

	if( ! stats ) return EBADMEM;
	ERROR_PROPAGATE( find_device( device_id, & device ));
	memcpy( stats, ( device_stats_ref ) device->specific, sizeof( device_stats_t ));
	return EOK;
}

int change_state_message( device_ref device, device_state_t state ){
	if( device->state != state ){
		device->state = state;
		printf( "State changed to %s\n", ( state == NETIF_ACTIVE ) ? "ACTIVE" : "STOPPED" );
		return state;
	}
	return EOK;
}

int create( device_id_t device_id, device_ref * device ){
	int	index;

	if( device_map_count( & netif_globals.device_map ) > 0 ){
		return EXDEV;
	}else{
		* device = ( device_ref ) malloc( sizeof( device_t ));
		if( !( * device )) return ENOMEM;
		( ** device ).specific = malloc( sizeof( device_stats_t ));
		if( ! ( ** device ).specific ){
			free( * device );
			return ENOMEM;
		}
		null_device_stats(( device_stats_ref )( ** device ).specific );
		( ** device ).device_id = device_id;
		( ** device ).nil_phone = -1;
		( ** device ).state = NETIF_STOPPED;
		index = device_map_add( & netif_globals.device_map, ( ** device ).device_id, * device );
		if( index < 0 ){
			free( * device );
			free(( ** device ).specific );
			* device = NULL;
			return index;
		}
	}
	return EOK;
}

int netif_initialize( void ){
	ipcarg_t	phonehash;

	return REGISTER_ME( SERVICE_LO, & phonehash );
}

void module_print_name( void ){
	printf( "%s", NAME );
}

int netif_probe_message( device_id_t device_id, int irq, uintptr_t io ){
	ERROR_DECLARE;

	device_ref			device;

	// create a new device
	ERROR_PROPAGATE( create( device_id, & device ));
	// print the settings
	printf("New device created:\n\tid\t= %d\n", device->device_id );
	return EOK;
}

int netif_send_message( device_id_t device_id, packet_t packet, services_t sender ){
	ERROR_DECLARE;

	device_ref	device;
	size_t		length;
	packet_t	next;
	int			phone;

	ERROR_PROPAGATE( find_device( device_id, & device ));
	if( device->state != NETIF_ACTIVE ){
		netif_pq_release( packet_get_id( packet ));
		return EFORWARD;
	}
	next = packet;
	do{
		++ (( device_stats_ref ) device->specific )->send_packets;
		++ (( device_stats_ref ) device->specific )->receive_packets;
		length = packet_get_data_length( next );
		(( device_stats_ref ) device->specific )->send_bytes += length;
		(( device_stats_ref ) device->specific )->receive_bytes += length;
		next = pq_next( next );
	}while( next );
	phone = device->nil_phone;
	fibril_rwlock_write_unlock( & netif_globals.lock );
	nil_received_msg( phone, device_id, packet, sender );
	fibril_rwlock_write_lock( & netif_globals.lock );
	return EOK;
}

int netif_start_message( device_ref device ){
	return change_state_message( device, NETIF_ACTIVE );
}

int netif_stop_message( device_ref device ){
	return change_state_message( device, NETIF_STOPPED );
}

/** @}
 */