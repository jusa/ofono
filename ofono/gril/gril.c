/*
 *
 *  RIL library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012 Canonical Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>

#include <glib.h>

#include "log.h"
#include "ringbuffer.h"
#include "gril.h"

#define COMMAND_FLAG_EXPECT_PDU			0x1
#define COMMAND_FLAG_EXPECT_SHORT_PROMPT	0x2

#define RILD_CMD_SOCKET "/dev/socket/rild"
#define RILD_DBG_SOCKET "/dev/socket/rild-debug"

struct ril_request {
	gchar *data;
	guint data_len;
	guint req;
	guint id;
	guint gid;
	GRilResponseFunc callback;
	gpointer user_data;
	GDestroyNotify notify;
};

struct ril_notify_node {
	guint id;
	guint gid;
	GRilNotifyFunc callback;
	gpointer user_data;
	gboolean destroyed;
};

typedef gboolean (*node_remove_func)(struct ril_notify_node *node,
					gpointer user_data);

struct ril_notify {
	GSList *nodes;
};

struct ril_s {
	gint ref_count;				/* Ref count */
	guint next_cmd_id;			/* Next command id */
	guint next_notify_id;			/* Next notify id */
	guint next_gid;				/* Next group id */
	GRilIO *io;				/* GRil IO */
	GQueue *command_queue;			/* Command queue */
	guint req_bytes_written;		/* bytes written from req */
	GHashTable *notify_list;		/* List of notification reg */
	GRilDisconnectFunc user_disconnect;	/* user disconnect func */
	gpointer user_disconnect_data;		/* user disconnect data */
	guint read_so_far;			/* Number of bytes processed */
	gboolean suspended;			/* Are we suspended? */
	GRilDebugFunc debugf;			/* debugging output function */
	gpointer debug_data;			/* Data to pass to debug func */
	GSList *response_lines;			/* char * lines of the response */
	gint timeout_source;
	gboolean destroyed;			/* Re-entrancy guard */
	gboolean in_read_handler;		/* Re-entrancy guard */
	gboolean in_notify;
};

struct _GRil {
	gint ref_count;
	struct ril_s *parent;
	guint group;
};

/*
 * This struct represents the header of a RIL reply.
 * It does not include the Big Endian UINT32 length prefix.
 */
struct ril_reply {
	guint32 reply;                          /* LE: should be 0 */
	guint32 serial_no;                      /* LE: used to match requests */
	guint32 error_code;                     /* LE: */
};

static void ril_wakeup_writer(struct ril_s *ril);

static void ril_notify_node_destroy(gpointer data, gpointer user_data)
{
	struct ril_notify_node *node = data;
	g_free(node);
}

static void ril_notify_destroy(gpointer user_data)
{
	struct ril_notify *notify = user_data;

	g_slist_foreach(notify->nodes, ril_notify_node_destroy, NULL);
	g_slist_free(notify->nodes);
	g_free(notify);
}

static gint ril_notify_node_compare_by_id(gconstpointer a, gconstpointer b)
{
	const struct ril_notify_node *node = a;
	guint id = GPOINTER_TO_UINT(b);

	if (node->id < id)
		return -1;

	if (node->id > id)
		return 1;

	return 0;
}

static gboolean ril_unregister_all(struct ril_s *ril,
					gboolean mark_only,
					node_remove_func func,
					gpointer userdata)
{
	GHashTableIter iter;
	struct ril_notify *notify;
	struct ril_notify_node *node;
	gpointer key, value;
	GSList *p;
	GSList *c;
	GSList *t;

	if (ril->notify_list == NULL)
		return FALSE;

	g_hash_table_iter_init(&iter, ril->notify_list);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		notify = value;

		p = NULL;
		c = notify->nodes;

		while (c) {
			node = c->data;

			if (func(node, userdata) != TRUE) {
				p = c;
				c = c->next;
				continue;
			}

			if (mark_only) {
				node->destroyed = TRUE;
				p = c;
				c = c->next;
				continue;
			}

			if (p)
				p->next = c->next;
			else
				notify->nodes = c->next;

			ril_notify_node_destroy(node, NULL);

			t = c;
			c = c->next;
			g_slist_free_1(t);
		}

		if (notify->nodes == NULL)
			g_hash_table_iter_remove(&iter);
	}

	return TRUE;
}


/*
 * This function creates a RIL request.  For a good reference on
 * the layout of RIL requests, responses, and unsolicited requests
 * see:
 *
 * https://wiki.mozilla.org/B2G/RIL
 *
 */
static struct ril_request *ril_request_create(struct ril_s *ril,
						guint gid,
						const guint req,
						const guint id,
						const char *data,
						const gsize data_len,
						GRilResponseFunc func,
						gpointer user_data,
						GDestroyNotify notify,
						gboolean wakeup)
{
	struct ril_request *r;
	gsize len;
	gchar *cur_bufp;
	guint32 *net_length, *request, *serial_no;

	r = g_try_new0(struct ril_request, 1);
	if (r == NULL)
		return 0;

        DBG("req: %s, id: %d, data_len: %d",
		ril_request_id_to_string(req), id, data_len);

        /* RIL request: 8 byte header + data */
        len = 8 + data_len;

        /* Add 4 bytes to buffer length to include length prefix */
        r->data_len = len + 4;

	r->data = g_try_new(char, r->data_len);
	if (r->data == NULL) {
		ofono_error("ril_request: can't allocate new request.");
		g_free(r);
		return 0;
	}

        /* convert length to network byte order (Big Endian) */
        net_length = (guint32 *) r->data;
        *net_length = htonl(len);

	/* advance past initial length */
        cur_bufp = r->data + 4;

	/* write request code */
	request = (guint32 *) cur_bufp;
	*request = req;
	cur_bufp += 4;

	/* write serial number */
	serial_no = (guint32 *) cur_bufp;
	*serial_no = id;
	cur_bufp += 4;

	/* copy request data */
	memcpy(cur_bufp, (const void *) data, data_len);

	r->req = req;
	r->gid = gid;
	r->id = id;
	r->callback = func;
	r->user_data = user_data;
	r->notify = notify;

	return r;
}

static void ril_request_destroy(struct ril_request *req)
{
	if (req->notify)
		req->notify(req->user_data);

	g_free(req->data);
	g_free(req);
}

static void ril_cleanup(struct ril_s *p)
{
	/* Cleanup pending commands */

	g_queue_free(p->command_queue);
        p->command_queue = NULL;

	/* Cleanup any response lines we have pending */
	g_slist_foreach(p->response_lines, (GFunc)g_free, NULL);
	g_slist_free(p->response_lines);
	p->response_lines = NULL;

	/* Cleanup registered notifications */

	if (p->notify_list)
		g_hash_table_destroy(p->notify_list);

	p->notify_list = NULL;

	if (p->timeout_source) {
		g_source_remove(p->timeout_source);
		p->timeout_source = 0;
	}
}

static void io_disconnect(gpointer user_data)
{
	struct ril_s *ril = user_data;

	ril_cleanup(ril);
	g_ril_io_unref(ril->io);
	ril->io = NULL;

	if (ril->user_disconnect)
		ril->user_disconnect(ril->user_disconnect_data);
}

static void handle_response(struct ril_s *p, struct ril_msg *message)
{
	gsize count = g_queue_get_length(p->command_queue);
	struct ril_request *req;
	gboolean found = FALSE;
	int i;

	g_assert(count > 0);

	for (i = 0; i < count; i++) {
		req = g_queue_peek_nth(p->command_queue, i);

		/* TODO: make conditional
		 * DBG("comparing req->id: %d to message->serial_no: %d",
		 *	req->id, message->serial_no);
		 */

		if (req->id == message->serial_no) {
			found = TRUE;
			message->req = req->req;

			DBG("RIL Reply: %s serial-no: %d errno: %s",
				ril_request_id_to_string(message->req),
				message->serial_no,
				ril_error_to_string(message->error));

			req = g_queue_pop_nth(p->command_queue, i);
			if (req->callback)
				req->callback(message, req->user_data);

			ril_request_destroy(req);

			if (g_queue_peek_head(p->command_queue))
				ril_wakeup_writer(p);
			/*
			 * TODO: there's a flaw in the current logic.
			 * If a matching response isn't received,
			 * req_bytes_written doesn't get reset.
			 * gatchat has the concept of modem wakeup,
			 * which is a failsafe way of making sure
			 * cmd_bytes_written gets reset, however if
			 * the modem isn't configured for wakeup,
			 * it may have the same problem.  Perhaps
			 * we should consider adding a timer?
			 */
			p->req_bytes_written = 0;

			/* Found our matching one */
			break;
		}
	}

	if (found == FALSE)
		DBG("Reply: %s serial_no: %d without a matching request!",
			ril_request_id_to_string(message->req),
			message->serial_no);

}

static void handle_unsol_req(struct ril_s *p, struct ril_msg *message)
{
	GHashTableIter iter;
	struct ril_notify *notify;
	int req_key;
	gpointer key, value;
	GList *list_item;
	struct ril_notify_node *node;

	if (p->notify_list == NULL)
		return;

	p->in_notify = TRUE;

	g_hash_table_iter_init(&iter, p->notify_list);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		req_key = *((int *)key);
		notify = value;

                /*
		 * TODO: add #ifdef...
		 * DBG("checking req_key: %d to req: %d", req_key, message->req);
		 */

		if (req_key != message->req)
			continue;

		list_item = notify->nodes;

		while (list_item != NULL) {
			node = list_item->data;

			/*
			 * TODO: add #ifdef...
			 * DBG("about to callback: notify: %x, node: %x, notify->nodes: %x, callback: %x",
			 *	notify, node, notify->nodes, node->callback);
			 */

			node->callback(message, node->user_data);

			list_item = g_slist_next(list_item);
		}
	}

	p->in_notify = FALSE;
}

static void dispatch(struct ril_s *p, struct ril_msg *message)
{
	guint32 *unsolicited_field, *id_num_field;
	gchar *bufp = message->buf;
	gchar *datap;
	gsize data_len;

	/* This could be done with a struct/union... */
	unsolicited_field = (guint32 *) bufp;
	if (*unsolicited_field)
		message->unsolicited = TRUE;
	else
		message->unsolicited = FALSE;

	bufp += 4;

	id_num_field = (guint32 *) bufp;
	if (message->unsolicited) {
		message->req = (int) *id_num_field;

		/*
		 * A RIL Unsolicited Event is two UINT32 fields ( unsolicited, and req/ev ),
		 * so subtract the length of the header from the overall length to calculate
		 * the length of the Event Data.
		 */
		data_len = message->buf_len - 8;
	} else {
		message->serial_no = (int) *id_num_field;

		bufp += 4;
		message->error = *((guint32 *) bufp);

		/*
		 * A RIL Solicited Response is three UINT32 fields ( unsolicied, serial_no
		 * and error ), so subtract the length of the header from the overall length
		 * to calculate the length of the Event Data.
		 */
		data_len = message->buf_len - 12;
	}

        /* advance to start of data.. */
	bufp += 4;

	/* Now, allocate new buffer for data only, copy from
	 * original, and free the original...
	 */
	if (data_len) {
		datap = g_try_malloc(data_len);
		if (datap == NULL)
			goto error;

		/* Copy bytes into new buffer */
		memmove(datap, (const void *) bufp, data_len);

		/* Free old buffer */
		g_free(message->buf);

		/* ...and replace with new buffer */
		message->buf = datap;
		message->buf_len = data_len;
	}

	if (message->unsolicited == TRUE) {
		DBG("RIL Event: %s\n",
			ril_unsol_request_to_string(message->req));

		handle_unsol_req(p, message);
	} else {
		handle_response(p, message);
	}
error:
	g_free(message->buf);
	g_free(message);
}

static struct ril_msg *read_fixed_record(struct ril_s *p,
						const guchar *bytes, gsize *len)
{
	struct ril_msg *message;
	int message_len, plen;

	/* First four bytes are length in TCP byte order (Big Endian) */
	plen = ntohl(*((uint32_t *) bytes));
	bytes += 4;

	/* TODO: Verify that 4k is the max message size from rild.
	 *
	 * These conditions shouldn't happen.  If it does
	 * there are three options:
	 *
	 * 1) ASSERT; ofono will restart via DBus
	 * 2) Consume the bytes & continue
	 * 3) force a disconnect
	 */
	g_assert(plen >= 8 && plen <= 4092);

	/* If we don't have the whole fixed record in the ringbuffer
	 * then return NULL & leave ringbuffer as is.
	*/

	message_len = *len - 4;
	if (message_len < plen) {
		return NULL;
	}

	/* FIXME: add check for message_len = 0? */

        message = g_try_malloc(sizeof(struct ril_msg));
        g_assert(message != NULL);

        /* allocate ril_msg->buffer */
        message->buf_len = plen;
        message->buf = g_try_malloc(plen);
        g_assert(message->buf != NULL);

        /* Copy bytes into message buffer */
        memmove(message->buf, (const void *) bytes, plen);

	/* Indicate to caller size of record we extracted */
	*len = plen + 4;
	return message;
}

static void new_bytes(struct ring_buffer *rbuf, gpointer user_data)
{
	struct ril_msg *message;
	struct ril_s *p = user_data;
	unsigned int len = ring_buffer_len(rbuf);
	unsigned int wrap = ring_buffer_len_no_wrap(rbuf);
	guchar *buf = ring_buffer_read_ptr(rbuf, p->read_so_far);

	p->in_read_handler = TRUE;

	/*
	 * TODO: make conditional
	 *	DBG("len: %d, wrap: %d", len, wrap);
	 */
	while (p->suspended == FALSE && (p->read_so_far < len)) {
		gsize rbytes = MIN(len - p->read_so_far, wrap - p->read_so_far);

		if (rbytes < 4) {
			/*
			 * TODO: make conditional
			 * DBG("Not enough bytes for header length: len: %d", len);
			 */
			return;
		}

		/* this function attempts to read the next full length
		 * fixed message from the stream.  if not all bytes are
		 * available, it returns NULL.  otherwise it allocates
		 * and returns a ril_message with the copied bytes, and
		 * drains those bytes from the ring_buffer
		 */
		message = read_fixed_record(p, buf, &rbytes);

		/* wait for the rest of the record... */
		if (message == NULL) {

			/* TODO: make conditional
			 * DBG("Not enough bytes for fixed record");
			 */
			break;
		}

		buf += rbytes;
		p->read_so_far += rbytes;

		/* TODO: need to better understand how wrap works! */
		if (p->read_so_far == wrap) {
			buf = ring_buffer_read_ptr(rbuf, p->read_so_far);
			wrap = len;
		}

		dispatch(p, message);

		ring_buffer_drain(rbuf, p->read_so_far);

		len -= p->read_so_far;
		wrap -= p->read_so_far;
		p->read_so_far = 0;
	}

	p->in_read_handler = FALSE;

	if (p->destroyed)
		g_free(p);
}

static gboolean can_write_data(gpointer data)
{
	struct ril_s *ril = data;
	struct ril_request *req;
	gsize bytes_written;
	gsize towrite;
	gsize len;

	/* Grab the first command off the queue and write as
	 * much of it as we can
	 */
	req = g_queue_peek_head(ril->command_queue);

	/* For some reason command queue is empty, cancel write watcher */
	if (req == NULL)
		return FALSE;

	len = req->data_len;

	/*
	 * TODO: make conditional:
	 * DBG("len: %d, req_bytes_written: %d", len, ril->req_bytes_written);
	 */

	/* For some reason write watcher fired, but we've already
	 * written the entire command out to the io channel,
	 * cancel write watcher
	 */
	if (ril->req_bytes_written >= len)
		return FALSE;

        /*
	 * AT modems need to be woken up via a command set by the
	 * upper layers.  RIL has no such concept, hence wakeup needed
	 * NOTE - I'm keeping the if statement here commented out, just
	 * in case this concept needs to be added back in...
	 *
	 * if (ril->req_bytes_written == 0 && wakeup_first == TRUE) {
	 *		cmd = at_command_create(0, chat->wakeup, none_prefix, 0,
	 *					NULL, wakeup_cb, chat, NULL, TRUE);
	 *		g_queue_push_head(chat->command_queue, cmd);
	 *	len = strlen(chat->wakeup);
	 *	chat->timeout_source = g_timeout_add(chat->wakeup_timeout,
	 *					wakeup_no_response, chat);
	 *	}
	 */

	towrite = len - ril->req_bytes_written;

#ifdef WRITE_SCHEDULER_DEBUG
	if (towrite > 5)
		towrite = 5;
#endif

	bytes_written = g_ril_io_write(ril->io,
					req->data + ril->req_bytes_written,
					towrite);

	/*
	 * TODO: make conditional
	 * DBG("bytes_written: %d", bytes_written);
	 */

	if (bytes_written == 0)
		return FALSE;

	ril->req_bytes_written += bytes_written;
	if (bytes_written < towrite)
		return TRUE;

	return FALSE;
}

static void ril_wakeup_writer(struct ril_s *ril)
{
	g_ril_io_set_write_handler(ril->io, can_write_data, ril);
}

static void ril_suspend(struct ril_s *ril)
{
	ril->suspended = TRUE;

	g_ril_io_set_write_handler(ril->io, NULL, NULL);
	g_ril_io_set_read_handler(ril->io, NULL, NULL);
        g_ril_io_set_debug(ril->io, NULL, NULL);
}

/*
 * TODO: need to determine when ril_resume/suspend are called.
 *
 * Most likely, this is in response to DBUS messages sent to
 * oFono to tell it the system is suspending/resuming.
 */
static void ril_resume(struct ril_s *ril)
{
	ril->suspended = FALSE;

	if (g_ril_io_get_channel(ril->io) == NULL) {
        	io_disconnect(ril);
        	return;
        }

	g_ril_io_set_disconnect_function(ril->io, io_disconnect, ril);

	g_ril_io_set_debug(ril->io, ril->debugf, ril->debug_data);

	g_ril_io_set_read_handler(ril->io, new_bytes, ril);

	if (g_queue_get_length(ril->command_queue) > 0)
	        ril_wakeup_writer(ril);
}

static gboolean ril_set_debug(struct ril_s *ril,
				GRilDebugFunc func, gpointer user_data)
{

	ril->debugf = func;
	ril->debug_data = user_data;

	if (ril->io)
		g_ril_io_set_debug(ril->io, func, user_data);

	return TRUE;
}

static void ril_unref(struct ril_s *ril)
{
	gboolean is_zero;

	is_zero = g_atomic_int_dec_and_test(&ril->ref_count);

	if (is_zero == FALSE)
		return;

        if (ril->io) {
        	ril_suspend(ril);
		g_ril_io_unref(ril->io);
        	ril->io = NULL;
        	ril_cleanup(ril);
        }

	if (ril->in_read_handler)
		ril->destroyed = TRUE;
	else
		g_free(ril);
}

static gboolean node_compare_by_group(struct ril_notify_node *node,
					gpointer userdata)
{
	guint group = GPOINTER_TO_UINT(userdata);

	if (node->gid == group)
		return TRUE;

	return FALSE;
}

static struct ril_s *create_ril()

{
	struct ril_s *ril;
	struct sockaddr_un addr;
	int sk;
	GIOChannel *io;

	ril = g_try_new0(struct ril_s, 1);
	if (ril == NULL)
		return ril;

	ril->ref_count = 1;
	ril->next_cmd_id = 1;
	ril->next_notify_id = 1;
	ril->next_gid = 0;
	ril->debugf = NULL;
	ril->req_bytes_written = 0;

	sk = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sk < 0) {
		ofono_error("create_ril: can't create unix socket: %s (%d)\n",
				strerror(errno), errno);
		goto error;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, RILD_CMD_SOCKET, sizeof(addr.sun_path) - 1);

	if (connect(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		ofono_error("create_ril: can't connect to RILD: %s (%d)\n",
				strerror(errno), errno);
		goto error;
	}

        io = g_io_channel_unix_new(sk);
        if (io == NULL) {
		ofono_error("create_ril: can't connect to RILD: %s (%d)\n",
				strerror(errno), errno);
		return NULL;
        }

	g_io_channel_set_buffered(io, FALSE);
	g_io_channel_set_encoding(io, NULL, NULL);
	g_io_channel_set_close_on_unref(io, TRUE);
	g_io_channel_set_flags(io, G_IO_FLAG_NONBLOCK, NULL);

        ril->io = g_ril_io_new(io);
        if (ril->io == NULL) {
		ofono_error("create_ril: can't create ril->io");
        	goto error;
	}

	g_ril_io_set_disconnect_function(ril->io, io_disconnect, ril);

	ril->command_queue = g_queue_new();
	if (ril->command_queue == NULL) {
		ofono_error("create_ril: Couldn't create command_queue.");
		goto error;
	}

	ril->notify_list = g_hash_table_new_full(g_int_hash, g_int_equal,
							g_free, ril_notify_destroy);

        g_ril_io_set_read_handler(ril->io, new_bytes, ril);

	return ril;

error:
        g_ril_io_unref(ril->io);

	if (ril->command_queue)
		g_queue_free(ril->command_queue);

	if (ril->notify_list)
		g_hash_table_destroy(ril->notify_list);

	g_free(ril);
	return NULL;
}

static struct ril_notify *ril_notify_create(struct ril_s *ril,
						const int req)
{
	struct ril_notify *notify;
	int *key;

	notify = g_try_new0(struct ril_notify, 1);
	if (notify == NULL)
		return 0;

	key = g_try_new0(int, 1);
	if (key == NULL)
		return 0;

	*key = req;

	g_hash_table_insert(ril->notify_list, key, notify);

	return notify;
}

static guint ril_register(struct ril_s *ril, guint group,
				const int req, GRilNotifyFunc func,
				gpointer user_data)
{
	struct ril_notify *notify;
	struct ril_notify_node *node;

	if (ril->notify_list == NULL)
		return 0;

	if (func == NULL)
		return 0;

	notify = g_hash_table_lookup(ril->notify_list, &req);

	if (notify == NULL)
		notify = ril_notify_create(ril, req);

	if (notify == NULL)
		return 0;

	node = g_try_new0(struct ril_notify_node, 1);
	if (node == NULL)
		return 0;

	node->id = ril->next_notify_id++;
	node->gid = group;
	node->callback = func;
	node->user_data = user_data;

	notify->nodes = g_slist_prepend(notify->nodes, node);
	DBG("after pre-pend; notify: %x, node %x, notify->nodes: %x, callback: %x",
		notify, node, notify->nodes, node->callback);

	return node->id;
}

static gboolean ril_unregister(struct ril_s *ril, gboolean mark_only,
					guint group, guint id)
{
	GHashTableIter iter;
	struct ril_notify *notify;
	struct ril_notify_node *node;
	gpointer key, value;
	GSList *l;

	if (ril->notify_list == NULL)
		return FALSE;

	g_hash_table_iter_init(&iter, ril->notify_list);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		notify = value;

		l = g_slist_find_custom(notify->nodes, GUINT_TO_POINTER(id),
					ril_notify_node_compare_by_id);

		if (l == NULL)
			continue;

		node = l->data;

		if (node->gid != group)
			return FALSE;

		if (mark_only) {
			node->destroyed = TRUE;
			return TRUE;
		}

		ril_notify_node_destroy(node, NULL);
		notify->nodes = g_slist_remove(notify->nodes, node);

		if (notify->nodes == NULL)
			g_hash_table_iter_remove(&iter);

		return TRUE;
	}

	return FALSE;
}

GRil *g_ril_new()
{
	GRil *ril;

	ril = g_try_new0(GRil, 1);
	if (ril == NULL)
		return NULL;

	ril->parent = create_ril();
	if (ril->parent == NULL) {
		g_free(ril);
		return NULL;
	}

	ril->group = ril->parent->next_gid++;
	ril->ref_count = 1;

	return ril;
}

GRil *g_ril_clone(GRil *clone)
{
	GRil *ril;

	if (clone == NULL)
		return NULL;

	ril = g_try_new0(GRil, 1);
	if (ril == NULL)
		return NULL;

	ril->parent = clone->parent;
	ril->group = ril->parent->next_gid++;
	ril->ref_count = 1;
	g_atomic_int_inc(&ril->parent->ref_count);

	return ril;
}

GIOChannel *g_ril_get_channel(GRil *ril)
{
	if (ril == NULL || ril->parent->io == NULL)
 		return NULL;

	return g_ril_io_get_channel(ril->parent->io);

}

GRilIO *g_ril_get_io(GRil *ril)
{
	if (ril == NULL)
		return NULL;

	return ril->parent->io;
}

GRil *g_ril_ref(GRil *ril)
{
	if (ril == NULL)
		return NULL;

	g_atomic_int_inc(&ril->ref_count);

	return ril;
}

guint g_ril_send(GRil *ril, const guint req, const char *data,
			const gsize data_len, GRilResponseFunc func,
			gpointer user_data, GDestroyNotify notify)
{
	struct ril_request *r;
	struct ril_s *p;

	if (ril == NULL || ril->parent == NULL || ril->parent->command_queue == NULL)
		return 0;

        p = ril->parent;

	r = ril_request_create(p, ril->group, req, p->next_cmd_id,
				data, data_len, func,
				user_data, notify, FALSE);
	if (r == NULL)
		return 0;

	p->next_cmd_id++;

	g_queue_push_tail(p->command_queue, r);

	if (g_queue_get_length(p->command_queue) == 1) {
		DBG("calling wakeup_writer: qlen: %d", g_queue_get_length(p->command_queue));
		ril_wakeup_writer(p);
	}

	return r->id;
}

void g_ril_suspend(GRil *ril)
{
	if (ril == NULL)
		return;

	ril_suspend(ril->parent);
}

void g_ril_resume(GRil *ril)
{
	if (ril == NULL)
		return;

	ril_resume(ril->parent);
}

void g_ril_unref(GRil *ril)
{
	gboolean is_zero;

	if (ril == NULL)
		return;

	is_zero = g_atomic_int_dec_and_test(&ril->ref_count);

	if (is_zero == FALSE)
		return;

	ril_unref(ril->parent);

	g_free(ril);
}

gboolean g_ril_set_debug(GRil *ril,
			GRilDebugFunc func, gpointer user_data)
{

	if (ril == NULL || ril->group != 0)
		return FALSE;

	return ril_set_debug(ril->parent, func, user_data);
}

guint g_ril_register(GRil *ril, const int req,
			GRilNotifyFunc func, gpointer user_data)
{
	if (ril == NULL)
		return 0;

	return ril_register(ril->parent, ril->group, req,
				func, user_data);
}

gboolean g_ril_unregister(GRil *ril, guint id)
{
	if (ril == NULL)
		return FALSE;

	return ril_unregister(ril->parent, ril->parent->in_notify,
					ril->group, id);
}

gboolean g_ril_unregister_all(GRil *ril)
{
	if (ril == NULL)
		return FALSE;

	return ril_unregister_all(ril->parent,
					ril->parent->in_notify,
					node_compare_by_group,
					GUINT_TO_POINTER(ril->group));
}
