/*
 * Copyright (c) 2011 Jan Vesely
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
/** @addtogroup libusb
 * @{
 */
/** @file
 * @brief OHCI driver USB transaction structure
 */
#include <errno.h>
#include <str_error.h>

#include <usb/usb.h>
#include <usb/debug.h>
#include <usb/host/batch.h>

void batch_init(
    batch_t *instance,
    usb_target_t target,
    usb_transfer_type_t transfer_type,
    usb_speed_t speed,
    size_t max_packet_size,
    char *buffer,
    char *transport_buffer,
    size_t buffer_size,
    char *setup_buffer,
    size_t setup_size,
    usbhc_iface_transfer_in_callback_t func_in,
    usbhc_iface_transfer_out_callback_t func_out,
    void *arg,
    ddf_fun_t *fun,
    void *private_data
    )
{
	assert(instance);
	link_initialize(&instance->link);
	instance->target = target;
	instance->transfer_type = transfer_type;
	instance->speed = speed;
	instance->callback_in = func_in;
	instance->callback_out = func_out;
	instance->arg = arg;
	instance->buffer = buffer;
	instance->transport_buffer = transport_buffer;
	instance->buffer_size = buffer_size;
	instance->setup_buffer = setup_buffer;
	instance->setup_size = setup_size;
	instance->max_packet_size = max_packet_size;
	instance->fun = fun;
	instance->private_data = private_data;
	instance->transfered_size = 0;
	instance->next_step = NULL;
	instance->error = EOK;

}
/*----------------------------------------------------------------------------*/
/** Mark batch as finished and continue with next step.
 *
 * @param[in] instance Batch structure to use.
 *
 */
void batch_finish(batch_t *instance, int error)
{
	assert(instance);
	instance->error = error;
	instance->next_step(instance);
}
/*----------------------------------------------------------------------------*/
/** Prepare data, get error status and call callback in.
 *
 * @param[in] instance Batch structure to use.
 * Copies data from transport buffer, and calls callback with appropriate
 * parameters.
 */
void batch_call_in(batch_t *instance)
{
	assert(instance);
	assert(instance->callback_in);

	/* We are data in, we need data */
	memcpy(instance->buffer, instance->transport_buffer,
	    instance->buffer_size);

	int err = instance->error;
	usb_log_debug("Batch(%p) callback IN(type:%d): %s(%d), %zu.\n",
	    instance, instance->transfer_type, str_error(err), err,
	    instance->transfered_size);

	instance->callback_in(
	    instance->fun, err, instance->transfered_size, instance->arg);
}
/*----------------------------------------------------------------------------*/
/** Get error status and call callback out.
 *
 * @param[in] instance Batch structure to use.
 */
void batch_call_out(batch_t *instance)
{
	assert(instance);
	assert(instance->callback_out);

	int err = instance->error;
	usb_log_debug("Batch(%p) callback OUT(type:%d): %s(%d).\n",
	    instance, instance->transfer_type, str_error(err), err);
	instance->callback_out(instance->fun,
	    err, instance->arg);
}
/**
 * @}
 */
