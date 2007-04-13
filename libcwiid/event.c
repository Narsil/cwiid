/* Copyright (C) 2007 L. Donnie Smith <cwiid@abstrakraft.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *  ChangeLog:
 *  2007-04-09 L. Donnie Smith <cwiid@abstrakraft.org>
 *  * renamed wiimote to libcwiid, renamed structures accordingly
 *
 *  2007-04-08 L. Donnie Smith <cwiid@abstrakraft.org>
 *  * fixed incompatible pointer warning in process_error
 *
 *  2007-04-08 Petter Reinholdtsen <pere@hungry.com>
 *  * fixed signed/unsigned comparison error in int_listen
 *
 *  2007-04-04 L. Donnie Smith <cwiid@abstrakraft.org>
 *  * implemented process_error to handle socket read errors
 *  * added rw_status triggers to read and write handlers
 *
 *  2007-03-14 L. Donnie Smith <cwiid@abstrakraft.org>
 *  * audit error checking
 *  * reorganized file
 *  * moved int_listen read/write code to process_read and process_write
 *  * updated (some/a few) comments
 *
 *  2007-03-06 L. Donnie Smith <cwiid@abstrakraft.org>
 *  * added wiimote parameter to cwiid_err calls
 *
 *  2007-03-01 L. Donnie Smith <cwiid@abstrakraft.org>
 *  * Initial ChangeLog
 *  * type audit (stdint, const, char booleans)
 */

/* Note on thread synchronization:
 * Note on Note on Note: This is really out of data.  Seriously, don't even
 * read it.  I only keep it around to make it easier if I ever actually get
 * around to updating it.
 *
 * Note on Note: this is slightly out of date, there's only one mutex, one
 * condition, and one condition mutex now.  It protects read, write, and 
 * report mode update.  Dispatch queue synchronization is handled in queue.c
 *
 * Thread synchronization in this library has gotten a bit more complicated
 * than I had hoped, so a brief overview is in order.
 *
 * Message Dispatch:
 * In order to isolate the client application's callback from the bluetooth
 * interrupt channel listener thread, as well as avoid deadlock issues
 * encountered with GTK, a message queue is used to pass messages between the
 * listener and dispatch threads.  Synchronization variables include:
 * dispatch_mutex - protects the dispatch queue
 * dispatch_cond - signals the dispatch thread that messages are available
 * dispatch_cond_mutex - mutex associated with dispatch_cond (see
 *   pthread_cond_wait and pthread_cond_signal documentation)
 * GTK continues to provide an ample testbed for deadlocks.  In order to
 * completely isolate the rest of the library from the dispatch thread,
 * cwiid_disconnect detaches the dispatch thread rather than joins it.
 * To maintain isolation, it is impossible to synchronize cwiid_disconnect
 * and the dispatch thread, so dispatch destroys it's own synchronization
 * variables.  In order to be sure this doesn't happen after cwiid_disconnect
 * has freed the memory allocated for those variables, the wiimote struct holds
 * pointers to variables, which dispatch copies on entry.  As the complexity
 * grows, I expect additional deadlock and race scenarios to be found.
 *
 * Data Read:
 * The read function (currently, only blocking reads are available) must
 * retrieve data from the interrupt channel listener thread.  In order to
 * prevent confusion over which reply corresponds with a given request,
 * only one read may be in process at any given time.  It may be possible in
 * the future to support multiple requests based on offset data included in the
 * reply, although the reply does not appear to contain all necessary address
 * data.  Synchronization variables include:
 * read_mutex - protects read function
 * read_cond - signals the read function that the entire message has been
 *   recieved
 * read_cond_mutex - mutex associated with read_cond
 *
 * Data Write:
 * The write function (currently, only blocking writes are available) must
 * wait for an ACK signal from the interrupt channel.  Like the reads, only one
 * write may be in process at any given time.  Synchronization variables include:
 * write_mutex - protects write function
 * write_cond - signals receipt of a write ACK
 * write_cond_mutex - mutex associated with write_cond
 *
 * I'm open to ways of reducing this complexity if anyone has ideas, but it
 * seems to me that it's required in order properly isolate the listener
 * thread from the callback.  In the case of the read function, I suppose the
 * listener thread could be canceled while the read function collects its own
 * data, then restarts the listener thread.  This seems ugly, and would make it
 * more difficult to implement concurrent/non-blocking reads in the future, as 
 * read reply collection is no longer centralized. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "cwiid_internal.h"
#include "queue.h"

/* process_* messages (except read and write ) allocate and fill in
 * cwiid_*_mesg structs from raw wiimote data.  Messages are then
 * placed at the end of mesg_array. */

static int process_error(struct wiimote *, size_t);
static int process_status(struct wiimote *, const unsigned char *,
                          struct mesg_array *);
static int process_btn(struct wiimote *, const unsigned char *,
                       struct mesg_array *);
static int process_acc(struct wiimote *, const unsigned char *,
                       struct mesg_array *);
static int process_ir10(struct wiimote *, const unsigned char *,
                        struct mesg_array *);
static int process_ir12(struct wiimote *, const unsigned char *,
                        struct mesg_array *);
static int process_ext(struct wiimote *, unsigned char *, unsigned char,
                       struct mesg_array *);

/* process_read and process_write handle copying data between wiimote buffers
 * and r/w buffers, as well as communication between cwiid_read or
 * cwiid_write */
static int process_read(struct wiimote *, unsigned char *);
static int process_write(struct wiimote *);

#define READ_BUF_LEN 23
void *int_listen(struct wiimote *wiimote)
{
	unsigned char buf[READ_BUF_LEN];
	ssize_t len;
	struct mesg_array *mesg_array;
	char err;

	do {
		/* Read packet */
		len = read(wiimote->int_socket, buf, READ_BUF_LEN);
		if ((len == -1) || (len == 0)) {
			process_error(wiimote, len);
			/* Quit! */
			break;
		}
		else {
			/* Verify first byte (DATA/INPUT) */
			if (buf[0] != (BT_TRANS_DATA | BT_PARAM_INPUT)) {
				cwiid_err(wiimote, "Invalid packet type");
			}

			/* Main switch */
			switch (buf[1]) {
			case RPT_STATUS:
			case RPT_BTN:
			case RPT_BTN_ACC:
			case RPT_BTN_EXT8:
			case RPT_BTN_ACC_IR12:
			case RPT_BTN_EXT19:
			case RPT_BTN_ACC_EXT16:
			case RPT_BTN_IR10_EXT9:
			case RPT_BTN_ACC_IR10_EXT6:
			case RPT_EXT21:
				if ((mesg_array = malloc(sizeof *mesg_array)) == NULL) {
					cwiid_err(wiimote, "Error allocating mesg array");
					break;
				}
				mesg_array->count = 0;
				err = 0;
				switch (buf[1]) {
				case RPT_STATUS:
					if (process_status(wiimote, &buf[2], mesg_array)) {
						err = 1;
					}
					break;
				case RPT_BTN:
					if (process_btn(wiimote, &buf[2], mesg_array)) {
						err = 1;
					}
					break;
				case RPT_BTN_ACC:
					if (process_btn(wiimote, &buf[2], mesg_array) ||
					  process_acc(wiimote, &buf[4], mesg_array)) {
						err = 1;
					}
					break;
				case RPT_BTN_EXT8:
					if (process_btn(wiimote, &buf[2], mesg_array) ||
					  process_ext(wiimote, &buf[4], 8, mesg_array)) {
						err = 1;
					}
					break;
				case RPT_BTN_ACC_IR12:
					if (process_btn(wiimote, &buf[2], mesg_array) ||
					  process_acc(wiimote, &buf[4], mesg_array) ||
					  process_ir12(wiimote, &buf[7], mesg_array)) {
						err = 1;
					}
					break;
				case RPT_BTN_EXT19:
					if (process_btn(wiimote, &buf[2], mesg_array) ||
					  process_ext(wiimote, &buf[4], 19, mesg_array)) {
						err = 1;
					}
					break;
				case RPT_BTN_ACC_EXT16:
					if (process_btn(wiimote, &buf[2], mesg_array) ||
					  process_acc(wiimote, &buf[4], mesg_array) ||
					  process_ext(wiimote, &buf[7], 16, mesg_array)) {
						err = 1;
					}
					break;
				case RPT_BTN_IR10_EXT9:
					if (process_btn(wiimote, &buf[2], mesg_array) ||
					  process_ir10(wiimote, &buf[4], mesg_array) ||
					  process_ext(wiimote, &buf[14], 9, mesg_array)) {
						err = 1;
					}
					break;
				case RPT_BTN_ACC_IR10_EXT6:
					if (process_btn(wiimote, &buf[2], mesg_array) ||
					  process_acc(wiimote, &buf[4], mesg_array) ||
					  process_ir10(wiimote, &buf[7], mesg_array) ||
					  process_ext(wiimote, &buf[17], 6, mesg_array)) {
						err = 1;
					}
					break;
				case RPT_EXT21:
					if (process_ext(wiimote, &buf[2], 21, mesg_array)) {
						err = 1;
					}
					break;
				}

				if (err || (mesg_array->count == 0)) {
					free(mesg_array);
				}
				else {
					if (queue_queue(wiimote->dispatch_queue, mesg_array)) {
						free_mesg_array(mesg_array);
						cwiid_err(wiimote, "error dispatching mesg array");
					}
				}
				break;
			case RPT_BTN_ACC_IR36_1:
			case RPT_BTN_ACC_IR36_2:
				cwiid_err(wiimote, "Unsupported report type received "
				                   "(interleaved data)");
				break;
			case RPT_READ_DATA:
				process_read(wiimote, &buf[4]);
				/* TODO: send button message */
				break;
			case RPT_WRITE_ACK:
				process_write(wiimote);
				break;
			default:
				cwiid_err(wiimote, "Unknown message type");
				break;
			}
		}
	} while (-1);

	return NULL;
}

static int process_error(struct wiimote *wiimote, size_t len)
{
	struct mesg_array *mesg_array;
	struct cwiid_error_mesg *error_mesg;
	int ret = 0;

	/* Error message */
	if (len == 0) {
		cwiid_err(wiimote, "Disconnect");
	}
	else {
		cwiid_err(wiimote, "Interrupt channel read error");
	}

	if ((mesg_array = malloc(sizeof *mesg_array)) == NULL) {
		cwiid_err(wiimote, "Error allocating mesg array");
		ret = -1;
		goto SKIP_QUEUE_MESG;
	}
	mesg_array->count = 1;
	if ((error_mesg = malloc(sizeof *error_mesg)) == NULL) {
		cwiid_err(wiimote, "Error allocating error message");
		ret = -1;
		goto SKIP_QUEUE_MESG;
	}
	error_mesg->type = CWIID_MESG_ERROR;
	if (len == 0) {
		error_mesg->error = CWIID_ERROR_DISCONNECT;
	}
	else {
		error_mesg->error = CWIID_ERROR_COMM;
	}
	mesg_array->mesg[0] = (union cwiid_mesg *)error_mesg;
	if (queue_flush(wiimote->dispatch_queue, (free_func_t *)free_mesg_array)) {
		cwiid_err(wiimote, "error flushing dispatch queue");
		ret = -1;
	}
	if (queue_queue(wiimote->dispatch_queue, mesg_array)) {
		free_mesg_array(mesg_array);
		cwiid_err(wiimote, "error dispatching mesg array");
		ret = -1;
	}

SKIP_QUEUE_MESG:

	/* Cancel any RW operations in progress */
	wiimote->rw_error = 1;
	if (pthread_mutex_lock(&wiimote->rw_cond_mutex)) {
		cwiid_err(wiimote, "Error locking rw_cond_mutex: deadlock warning");
		ret = -1;
	}
	else {
		if (pthread_cond_signal(&wiimote->rw_cond)) {
			cwiid_err(wiimote, "Error signaling rw_cond: deadlock warning");
			ret = -1;
		}
		if (pthread_mutex_unlock(
		  &wiimote->rw_cond_mutex)) {
			cwiid_err(wiimote, "Error unlocking rw_cond_mutex: "
			                   "deadlock warning");
			ret = -1;
		}
	}

	return ret;
}

static int process_status(struct wiimote *wiimote, const unsigned char *data,
                          struct mesg_array *mesg_array)
{
	struct cwiid_status_mesg *mesg;

	if ((mesg = malloc(sizeof *mesg)) == NULL) {
		cwiid_err(wiimote, "Error allocating status mesg");
		return -1;
	}

	mesg->type = CWIID_MESG_STATUS;
	mesg->battery = data[5];
	if (data[2] & 0x02) {
		/* dispatch will figure out what it is */
		mesg->extension = CWIID_EXT_UNKNOWN;
	}
	else {
		mesg->extension = CWIID_EXT_NONE;
	}

	mesg_array->mesg[mesg_array->count] = (union cwiid_mesg *)mesg;
	mesg_array->count++;

	return 0;
}

static int process_btn(struct wiimote *wiimote, const unsigned char *data,
                       struct mesg_array *mesg_array)
{
	struct cwiid_btn_mesg *mesg;
	uint16_t buttons;

	buttons = (data[0] & BTN_MASK_0)<<8 |
	          (data[1] & BTN_MASK_1);
	if (wiimote->buttons != buttons) {
		wiimote->buttons = buttons;

		if (wiimote->rpt_mode_flags & CWIID_RPT_BTN) {
			if ((mesg = malloc(sizeof *mesg)) == NULL) {
				cwiid_err(wiimote, "Error allocating btn mesg");
				return -1;
			}
			mesg->type = CWIID_MESG_BTN;
			mesg->buttons = buttons;

			mesg_array->mesg[mesg_array->count] = (union cwiid_mesg *)mesg;
			mesg_array->count++;
		}
	}

	return 0;
}

static int process_acc(struct wiimote *wiimote, const unsigned char *data,
                       struct mesg_array *mesg_array)
{
	struct cwiid_acc_mesg *mesg;

	if (wiimote->rpt_mode_flags & CWIID_RPT_ACC) {
		if ((mesg = malloc(sizeof *mesg)) == NULL) {
			cwiid_err(wiimote, "Error allocating acc mesg");
			return -1;
		}
		mesg->type = CWIID_MESG_ACC;
		mesg->x = data[0];
		mesg->y = data[1];
		mesg->z = data[2];

		mesg_array->mesg[mesg_array->count] = (union cwiid_mesg *)mesg;
		mesg_array->count++;
	}

	return 0;
}

static int process_ir10(struct wiimote *wiimote, const unsigned char *data,
                        struct mesg_array *mesg_array)
{
	struct cwiid_ir_mesg *mesg;
	int i;
	const unsigned char *block;

	if (wiimote->rpt_mode_flags & CWIID_RPT_IR) {
		if ((mesg = malloc(sizeof *mesg)) == NULL) {
			cwiid_err(wiimote, "Error allocating ir mesg");
			return -1;
		}
		mesg->type = CWIID_MESG_IR;

		for (i=0, block=data; i < CWIID_IR_SRC_COUNT; i+=2, block+=5) {
			if (block[0] == 0xFF) {
				mesg->src[i].valid = 0;
			}
			else {
				mesg->src[i].valid = 1;
				mesg->src[i].x = ((uint16_t)block[2] & 0x30)<<4 |
				                  (uint16_t)block[0];
				mesg->src[i].y = ((uint16_t)block[2] & 0xC0)<<2 |
				                  (uint16_t)block[1];
				mesg->src[i].size = -1;
			}

			if (block[3] == 0xFF) {
				mesg->src[i+1].valid = 0;
			}
			else {
				mesg->src[i+1].valid = 1;
				mesg->src[i+1].x = ((uint16_t)block[2] & 0x03)<<8 |
				                    (uint16_t)block[3];
				mesg->src[i+1].y = ((uint16_t)block[2] & 0x0C)<<6 |
				                    (uint16_t)block[4];
				mesg->src[i+1].size = -1;
			}
		}

		mesg_array->mesg[mesg_array->count] = (union cwiid_mesg *)mesg;
		mesg_array->count++;
	}

	return 0;
}

static int process_ir12(struct wiimote *wiimote, const unsigned char *data,
                        struct mesg_array *mesg_array)
{
	struct cwiid_ir_mesg *mesg;
	int i;
	const unsigned char *block;

	if (wiimote->rpt_mode_flags & CWIID_RPT_IR) {
		if ((mesg = malloc(sizeof *mesg)) == NULL) {
			cwiid_err(wiimote, "Error allocating ir mesg");
			return -1;
		}
		mesg->type = CWIID_MESG_IR;

		for (i=0, block=data; i < CWIID_IR_SRC_COUNT; i++, block+=3) {
			if (block[0] == 0xFF) {
				mesg->src[i].valid = 0;
			}
			else {
				mesg->src[i].valid = 1;
				mesg->src[i].x = ((uint16_t)block[2] & 0x30)<<4 |
				                  (uint16_t)block[0];
				mesg->src[i].y = ((uint16_t)block[2] & 0xC0)<<2 |
				                  (uint16_t)block[1];
				mesg->src[i].size = block[2] & 0x0F;
			}
		}

		mesg_array->mesg[mesg_array->count] = (union cwiid_mesg *)mesg;
		mesg_array->count++;
	}

	return 0;
}

static int process_ext(struct wiimote *wiimote, unsigned char *data,
                       unsigned char len, struct mesg_array *mesg_array)
{
	struct cwiid_nunchuk_mesg *nunchuk_mesg;
	struct cwiid_classic_mesg *classic_mesg;
	int i;

	switch (wiimote->extension) {
	case CWIID_EXT_NONE:
		cwiid_err(wiimote,
		          "Extension report received with no extension present");
		break;
	case CWIID_EXT_UNKNOWN:
		break;
	case CWIID_EXT_NUNCHUK:
		if (wiimote->rpt_mode_flags & CWIID_RPT_NUNCHUK) {
			if ((nunchuk_mesg = malloc(sizeof *nunchuk_mesg)) == NULL) {
				cwiid_err(wiimote, "Error allocating nunchuk mesg");
				return -1;
			}

			nunchuk_mesg->type = CWIID_MESG_NUNCHUK;
			nunchuk_mesg->stick_x = DECODE(data[0]);
			nunchuk_mesg->stick_y = DECODE(data[1]);
			nunchuk_mesg->acc_x = DECODE(data[2]);
			nunchuk_mesg->acc_y = DECODE(data[3]);
			nunchuk_mesg->acc_z = DECODE(data[4]);
			nunchuk_mesg->buttons = ~DECODE(data[5]) & NUNCHUK_BTN_MASK;

			mesg_array->mesg[mesg_array->count] =
			  (union cwiid_mesg *)nunchuk_mesg;
			mesg_array->count++;
		}
		break;
	case CWIID_EXT_CLASSIC:
		if (wiimote->rpt_mode_flags & CWIID_RPT_CLASSIC) {
			if ((classic_mesg = malloc(sizeof *classic_mesg)) == NULL) {
				cwiid_err(wiimote, "Error allocating classic mesg");
				return -1;
			}

			for (i=0; i < 6; i++) {
				data[i] = DECODE(data[i]);
			}

			classic_mesg->type = CWIID_MESG_CLASSIC;
			classic_mesg->l_stick_x = data[0] & 0x3F;
			classic_mesg->l_stick_y = data[1] & 0x3F;
			classic_mesg->r_stick_x = (data[0] & 0xC0)>>3 |
			                          (data[1] & 0xC0)>>5 |
			                          (data[2] & 0x80)>>7;
			classic_mesg->r_stick_y = data[2] & 0x1F;
			classic_mesg->l = (data[2] & 0x60)>>2 |
			                  (data[3] & 0xE0)>>5;
			classic_mesg->r = data[3] & 0x1F;
			classic_mesg->buttons = ~((uint16_t)data[4]<<8 |
			                          (uint16_t)data[5]);

			mesg_array->mesg[mesg_array->count] =
			  (union cwiid_mesg *)classic_mesg;
			mesg_array->count++;
		}
		break;
	}

	return 0;
}

static int process_read(struct wiimote *wiimote, unsigned char *data)
{
	uint8_t data_len;
	uint8_t error;
	int ret = 0;

	if (wiimote->rw_status == RW_PENDING) {
		/* Extract error status and current packet length */
		data_len = (data[0]>>4)+1;
		error = data[0] & 0x0F;
		/* Error if wiimote errors, or if packet is too long */
		if (((data_len + wiimote->read_received) > wiimote->read_len) ||
		  error) {
			cwiid_err(wiimote, "Error in read data");
			wiimote->rw_error = 1;
			wiimote->rw_status = RW_NONE;
			ret = -1;
		}
		else {
			/* Copy data into read_buf, update read data, signal
			 * ready if we have enough data */
			memcpy(wiimote->read_buf, data+3, data_len);
			wiimote->read_buf += data_len;
			wiimote->read_received += data_len;
			if (wiimote->read_received == wiimote->read_len) {
				wiimote->rw_status = RW_READY;
			}
		}
		if (wiimote->rw_error || (wiimote->rw_status != RW_PENDING)) {
			/* Lock rw_cond_mutex, signal rw_cond, unlock
			 * rw_cond_mutex */
			if (pthread_mutex_lock(&wiimote->rw_cond_mutex)) {
				cwiid_err(wiimote, "Error locking rw_cond_mutex: "
				                   "deadlock warning");
				wiimote->rw_error = 1;
				ret = -1;
			}
			else {
				if (pthread_cond_signal(&wiimote->rw_cond)) {
					cwiid_err(wiimote, "Error signaling rw_cond: "
					                   "deadlock warning");
					wiimote->rw_error = 1;
					ret = -1;
				}
				if (pthread_mutex_unlock(
				  &wiimote->rw_cond_mutex)) {
					cwiid_err(wiimote, "Error unlocking rw_cond_mutex: "
				                       "deadlock warning");
					wiimote->rw_error = 1;
					ret = -1;
				}
			}
		}
	}
	else {
		cwiid_err(wiimote, "Extraneous read data received");
		ret = -1;
	}

	return ret;
}

static int process_write(struct wiimote *wiimote)
{
	int ret = 0;

	if (wiimote->rw_status == RW_PENDING) {
		wiimote->rw_status = RW_READY;
		/* Lock write_cond_mutex, signal write_cond, unlock
		 * write_cond_mutex */
		if (pthread_mutex_lock(&wiimote->rw_cond_mutex)) {
			cwiid_err(wiimote, "Error locking rw_cond_mutex: "
			                   "deadlock warning");
			wiimote->rw_error = 1;
			ret = -1;
		}
		else {
			if (pthread_cond_signal(&wiimote->rw_cond)) {
				cwiid_err(wiimote, "Error signaling rw_cond: "
				                   "deadlock warning");
				wiimote->rw_error = 1;
				ret = -1;
			}
			if (pthread_mutex_unlock(&wiimote->rw_cond_mutex)) {
				cwiid_err(wiimote, "Error unlocking rw_cond_mutex: "
		                           "deadlock warning");
				wiimote->rw_error = 1;
				ret = -1;
			}
		}
	}
	else {
		cwiid_err(wiimote, "Extraneous write ack received");
		ret = -1;
	}

	return ret;
}

/* cleanup func */
static void free_dispatch_queue(struct queue *queue)
{
	if (queue_free(queue, (free_func_t *)free_mesg_array)) {
		/* TODO: return proper wiimote ptr */
		cwiid_err(NULL, "Error freeing dispatch queue");
	}
}

void *dispatch(struct wiimote *wiimote)
{
	int cancelstate;
	struct queue *dispatch_queue = wiimote->dispatch_queue;
	struct mesg_array *mesg_array;
	union cwiid_mesg *mesg;
	unsigned char buf;

	pthread_cleanup_push((void (*)(void *))free_dispatch_queue,
	                     (void *)dispatch_queue);
	do {
		if (queue_wait(dispatch_queue)) {
			cwiid_err(wiimote, "Error waiting on dispatch_queue");
			return NULL;
		}
		pthread_testcancel();

		/* dispatch messages */
		while (!queue_dequeue(dispatch_queue, (void **)&mesg_array)) {
			pthread_testcancel();
			/* Disable cancelling while in callback */
			if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancelstate)) {
				cwiid_err(wiimote, "Error disabling cancel state");
			}
			else {
				/* TODO:Move this outside of the cancel disable? */
				/* TODO:This assumes that status messages are the first and
				 * only messages in the array */
				mesg = mesg_array->mesg[0];
				if (mesg->type == CWIID_MESG_STATUS) {
					if ((mesg->status_mesg.extension == CWIID_EXT_UNKNOWN) &&
					  (wiimote->extension == CWIID_EXT_NONE)) {
						buf = 0x00;
						/* Initialize extension register space */
						if (cwiid_write(wiimote, CWIID_RW_REG, 0xA40040, 1,
						                  &buf)) {
							cwiid_err(wiimote, "Error initializing extension");
							wiimote->extension = CWIID_EXT_UNKNOWN;
						}
						/* Read extension ID */
						else if (cwiid_read(wiimote,
						         CWIID_RW_REG | CWIID_RW_DECODE, 0xA400FE,
							     1, &buf)) {
							cwiid_err(wiimote, "Error reading extension type");
							wiimote->extension = CWIID_EXT_UNKNOWN;
						}
						else {
							switch (buf) {
							case EXT_NONE:
							case EXT_PARTIAL:
								wiimote->extension = CWIID_EXT_NONE;
								break;
							case EXT_NUNCHUK:
								wiimote->extension = CWIID_EXT_NUNCHUK;
								break;
							case EXT_CLASSIC:
								wiimote->extension = CWIID_EXT_CLASSIC;
								break;
							default:
								wiimote->extension = CWIID_EXT_UNKNOWN;
								break;
							}
						}
					}
					else if (mesg->status_mesg.extension == CWIID_EXT_NONE) {
						wiimote->extension = CWIID_EXT_NONE;
					}

					if (update_rpt_mode(wiimote, -1)) {
						cwiid_err(wiimote, "Error reseting report mode");
					}

					/* Invoke Callback (for Status Messages) */
					if (wiimote->rpt_mode_flags & CWIID_RPT_STATUS) {
						mesg->status_mesg.extension = wiimote->extension;
						if (wiimote->mesg_callback) {
							wiimote->mesg_callback(wiimote->id,
							                       mesg_array->count,
							                       mesg_array->mesg);
						}
					}
				}
				else {
					/* Invoke Callback (all but Status Messages) */
					if (wiimote->mesg_callback) {
						wiimote->mesg_callback(wiimote->id, mesg_array->count,
						                       mesg_array->mesg);
					}
				}
				/* Reenable Thread Cancel */
				if (pthread_setcancelstate(cancelstate, &cancelstate)) {
					cwiid_err(wiimote, "Error enabling cancel state");
				}
			}
			free_mesg_array(mesg_array);
		}
	} while (-1);

	/* This code should never execute */
	cwiid_err(wiimote, "Exiting dispatch thread");

	/* Just in case, free the dispatch queue */
	pthread_cleanup_pop(1);

	return NULL;
}
