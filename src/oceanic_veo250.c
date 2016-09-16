/*
 * libdivecomputer
 *
 * Copyright (C) 2008 Jef Driesen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <string.h> // memcpy
#include <stdlib.h> // malloc, free

#include <libdivecomputer/oceanic_veo250.h>

#include "context-private.h"
#include "device-private.h"
#include "oceanic_common.h"
#include "serial.h"
#include "ringbuffer.h"
#include "checksum.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &oceanic_veo250_device_vtable.base)

#define MAXRETRIES 2
#define MULTIPAGE  4

#define ACK 0x5A
#define NAK 0xA5

typedef struct oceanic_veo250_device_t {
	oceanic_common_device_t base;
	dc_serial_t *port;
	unsigned int last;
} oceanic_veo250_device_t;

static dc_status_t oceanic_veo250_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t oceanic_veo250_device_close (dc_device_t *abstract);

static const oceanic_common_device_vtable_t oceanic_veo250_device_vtable = {
	{
		sizeof(oceanic_veo250_device_t),
		DC_FAMILY_OCEANIC_VEO250,
		oceanic_common_device_set_fingerprint, /* set_fingerprint */
		oceanic_veo250_device_read, /* read */
		NULL, /* write */
		oceanic_common_device_dump, /* dump */
		oceanic_common_device_foreach, /* foreach */
		oceanic_veo250_device_close /* close */
	},
	oceanic_common_device_logbook,
	oceanic_common_device_profile,
};

static const oceanic_common_version_t oceanic_veo250_version[] = {
	{"GENREACT \0\0 256K"},
	{"VEO 200 R\0\0 256K"},
	{"VEO 250 R\0\0 256K"},
	{"SEEMANN R\0\0 256K"},
	{"VEO 180 R\0\0 256K"},
	{"AERISXR2 \0\0 256K"},
	{"INSIGHT R\0\0 256K"},
};

static const oceanic_common_layout_t oceanic_veo250_layout = {
	0x8000, /* memsize */
	0x0000, /* cf_devinfo */
	0x0040, /* cf_pointers */
	0x0400, /* rb_logbook_begin */
	0x0600, /* rb_logbook_end */
	8, /* rb_logbook_entry_size */
	0x0600, /* rb_profile_begin */
	0x8000, /* rb_profile_end */
	1, /* pt_mode_global */
	1, /* pt_mode_logbook */
	1, /* pt_mode_serial */
};


static dc_status_t
oceanic_veo250_send (oceanic_veo250_device_t *device, const unsigned char command[], unsigned int csize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	// Discard garbage bytes.
	dc_serial_purge (device->port, DC_DIRECTION_INPUT);

	// Send the command to the dive computer.
	status = dc_serial_write (device->port, command, csize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Receive the response (ACK/NAK) of the dive computer.
	unsigned char response = NAK;
	status = dc_serial_read (device->port, &response, 1, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the response of the dive computer.
	if (response != ACK) {
		ERROR (abstract->context, "Unexpected answer start byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_veo250_transfer (oceanic_veo250_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// Send the command to the device. If the device responds with an
	// ACK byte, the command was received successfully and the answer
	// (if any) follows after the ACK byte. If the device responds with
	// a NAK byte, we try to resend the command a number of times before
	// returning an error.

	unsigned int nretries = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = oceanic_veo250_send (device, command, csize)) != DC_STATUS_SUCCESS) {
		if (rc != DC_STATUS_TIMEOUT && rc != DC_STATUS_PROTOCOL)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;

		// Delay the next attempt.
		dc_serial_sleep (device->port, 100);
	}

	// Receive the answer of the dive computer.
	status = dc_serial_read (device->port, answer, asize, NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the last byte of the answer.
	if (answer[asize - 1] != NAK) {
		ERROR (abstract->context, "Unexpected answer byte.");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_veo250_init (oceanic_veo250_device_t *device)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// Send the command to the dive computer.
	unsigned char command[2] = {0x55, 0x00};
	status = dc_serial_write (device->port, command, sizeof (command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Receive the answer of the dive computer.
	size_t n = 0;
	unsigned char answer[13] = {0};
	status = dc_serial_read (device->port, answer, sizeof (answer), &n);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		if (n == 0)
			return DC_STATUS_SUCCESS;
		return status;
	}

	// Verify the answer.
	const unsigned char response[13] = {
		0x50, 0x50, 0x53, 0x2D, 0x2D, 0x4F, 0x4B,
		0x5F, 0x56, 0x32, 0x2E, 0x30, 0x30};
	if (memcmp (answer, response, sizeof (response)) != 0) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_veo250_quit (oceanic_veo250_device_t *device)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;

	// Send the command to the dive computer.
	unsigned char command[2] = {0x98, 0x00};
	status = dc_serial_write (device->port, command, sizeof (command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
oceanic_veo250_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceanic_veo250_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (oceanic_veo250_device_t *) dc_device_allocate (context, &oceanic_veo250_device_vtable.base);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	oceanic_common_device_init (&device->base);

	// Override the base class values.
	device->base.multipage = MULTIPAGE;

	// Set the default values.
	device->port = NULL;
	device->last = 0;

	// Open the device.
	status = dc_serial_open (&device->port, context, name);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to open the serial port.");
		goto error_free;
	}

	// Set the serial communication protocol (9600 8N1).
	status = dc_serial_configure (device->port, 9600, 8, DC_PARITY_NONE, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_close;
	}

	// Set the timeout for receiving data (3000 ms).
	status = dc_serial_set_timeout (device->port, 3000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_close;
	}

	// Set the DTR line.
	status = dc_serial_set_dtr (device->port, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the DTR line.");
		goto error_close;
	}

	// Set the RTS line.
	status = dc_serial_set_rts (device->port, 1);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the RTS line.");
		goto error_close;
	}

	// Give the interface 100 ms to settle and draw power up.
	dc_serial_sleep (device->port, 100);

	// Make sure everything is in a sane state.
	dc_serial_purge (device->port, DC_DIRECTION_ALL);

	// Initialize the data cable (PPS mode).
	status = oceanic_veo250_init (device);
	if (status != DC_STATUS_SUCCESS) {
		goto error_close;
	}

	// Delay the sending of the version command.
	dc_serial_sleep (device->port, 100);

	// Switch the device from surface mode into download mode. Before sending
	// this command, the device needs to be in PC mode (manually activated by
	// the user), or already in download mode.
	status = oceanic_veo250_device_version ((dc_device_t *) device, device->base.version, sizeof (device->base.version));
	if (status != DC_STATUS_SUCCESS) {
		goto error_close;
	}

	// Override the base class values.
	if (OCEANIC_COMMON_MATCH (device->base.version, oceanic_veo250_version)) {
		device->base.layout = &oceanic_veo250_layout;
	} else {
		WARNING (context, "Unsupported device detected!");
		device->base.layout = &oceanic_veo250_layout;
	}

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_close:
	dc_serial_close (device->port);
error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
oceanic_veo250_device_close (dc_device_t *abstract)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	oceanic_veo250_device_t *device = (oceanic_veo250_device_t*) abstract;
	dc_status_t rc = DC_STATUS_SUCCESS;

	// Switch the device back to surface mode.
	oceanic_veo250_quit (device);

	// Close the device.
	rc = dc_serial_close (device->port);
	if (rc != DC_STATUS_SUCCESS) {
		dc_status_set_error(&status, rc);
	}

	return status;
}


dc_status_t
oceanic_veo250_device_keepalive (dc_device_t *abstract)
{
	oceanic_veo250_device_t *device = (oceanic_veo250_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	unsigned char answer[2] = {0};
	unsigned char command[4] = {0x91,
		(device->last     ) & 0xFF, // low
		(device->last >> 8) & 0xFF, // high
		0x00};
	dc_status_t rc = oceanic_veo250_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the answer.
	if (answer[0] != NAK) {
		ERROR (abstract->context, "Unexpected answer byte(s).");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
oceanic_veo250_device_version (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	oceanic_veo250_device_t *device = (oceanic_veo250_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size < PAGESIZE)
		return DC_STATUS_INVALIDARGS;

	unsigned char answer[PAGESIZE + 2] = {0};
	unsigned char command[2] = {0x90, 0x00};
	dc_status_t rc = oceanic_veo250_transfer (device, command, sizeof (command), answer, sizeof (answer));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Verify the checksum of the answer.
	unsigned char crc = answer[PAGESIZE];
	unsigned char ccrc = checksum_add_uint8 (answer, PAGESIZE, 0x00);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	memcpy (data, answer, PAGESIZE);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_veo250_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	oceanic_veo250_device_t *device = (oceanic_veo250_device_t*) abstract;

	if ((address % PAGESIZE != 0) ||
		(size    % PAGESIZE != 0))
		return DC_STATUS_INVALIDARGS;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the number of packages.
		unsigned int npackets = (size - nbytes) / PAGESIZE;
		if (npackets > MULTIPAGE)
			npackets = MULTIPAGE;

		// Read the package.
		unsigned int first =  address / PAGESIZE;
		unsigned int last  = first + npackets - 1;
		unsigned char answer[(PAGESIZE + 1) * MULTIPAGE + 1] = {0};
		unsigned char command[6] = {0x20,
				(first     ) & 0xFF, // low
				(first >> 8) & 0xFF, // high
				(last     ) & 0xFF, // low
				(last >> 8) & 0xFF, // high
				0};
		dc_status_t rc = oceanic_veo250_transfer (device, command, sizeof (command), answer, (PAGESIZE + 1) * npackets + 1);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		device->last = last;

		unsigned int offset = 0;
		for (unsigned int i = 0; i < npackets; ++i) {
			// Verify the checksum of the answer.
			unsigned char crc = answer[offset + PAGESIZE];
			unsigned char ccrc = checksum_add_uint8 (answer + offset, PAGESIZE, 0x00);
			if (crc != ccrc) {
				ERROR (abstract->context, "Unexpected answer checksum.");
				return DC_STATUS_PROTOCOL;
			}

			memcpy (data, answer + offset, PAGESIZE);

			offset += PAGESIZE + 1;
			nbytes += PAGESIZE;
			address += PAGESIZE;
			data += PAGESIZE;
		}
	}

	return DC_STATUS_SUCCESS;
}
