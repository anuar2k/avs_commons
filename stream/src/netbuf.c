/*
 * AVSystem Commons Library
 *
 * Copyright (C) 2014 AVSystem <http://www.avsystem.com/>
 *
 * This code is free and open source software licensed under the MIT License.
 * See the LICENSE file for details.
 */

#include <config.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <avsystem/commons/buffer.h>
#include <avsystem/commons/net.h>
#include <avsystem/commons/stream_v_table.h>
#include <avsystem/commons/stream/netbuf.h>

#include "avsystem/commons/stream/net.h"

#ifdef HAVE_VISIBILITY
#pragma GCC visibility push(hidden)
#endif

typedef struct buffered_netstream_struct {
    const avs_stream_v_table_t * const vtable;
    avs_net_abstract_socket_t *socket;

    avs_buffer_t *out_buffer;
    avs_buffer_t *in_buffer;

    int errno_;
} buffered_netstream_t;

static void set_errno(buffered_netstream_t *stream) {
    if (!stream->errno_) {
        stream->errno_ = errno;
    }
}

static int out_buffer_flush(buffered_netstream_t *stream) {
    int result = avs_net_socket_send(stream->socket,
                                   avs_buffer_data(stream->out_buffer),
                                   avs_buffer_data_size(stream->out_buffer));
    if (!result) {
        avs_buffer_reset(stream->out_buffer);
    } else {
        set_errno(stream);
    }
    return result;
}

static int buffered_netstream_write(avs_stream_abstract_t *stream_,
                                    const void *data,
                                    size_t data_length) {
    buffered_netstream_t *stream = (buffered_netstream_t *) stream_;

    if (data_length >= avs_buffer_space_left(stream->out_buffer)) {
        if (out_buffer_flush(stream)
                || avs_net_socket_send(stream->socket, data, data_length)) {
            set_errno(stream);
            return -1;
        } else {
            return 0;
        }
    }

    return avs_buffer_append_bytes(stream->out_buffer, data, data_length);
}

static int buffered_netstream_finish_message(avs_stream_abstract_t *stream) {
    return out_buffer_flush((buffered_netstream_t *) stream);
}

static int return_data_from_buffer(avs_buffer_t *in_buffer,
                                   size_t *out_bytes_read,
                                   char *out_message_finished,
                                   void *buffer,
                                   size_t buffer_length) {
    *out_bytes_read = avs_buffer_data_size(in_buffer);
    if (buffer_length < *out_bytes_read) {
        *out_bytes_read = buffer_length;
    }

    memcpy(buffer, avs_buffer_data(in_buffer), *out_bytes_read);
    avs_buffer_consume_bytes(in_buffer, *out_bytes_read);

    *out_message_finished = 0;

    return 0;
}

static int read_data_to_user_buffer(buffered_netstream_t *stream,
                                    size_t *out_bytes_read,
                                    char *out_message_finished,
                                    void *buffer,
                                    size_t buffer_length) {
    int result = avs_net_socket_receive(stream->socket,
                                      out_bytes_read,
                                      buffer,
                                      buffer_length);
    if (result) {
        set_errno(stream);
    }
    *out_message_finished = (result || *out_bytes_read == 0);
    return result;
}

static int in_buffer_read_some(buffered_netstream_t *stream,
                               size_t *out_bytes_read) {
    int result;
    avs_buffer_t *in_buffer = stream->in_buffer;
    size_t space_left = avs_buffer_space_left(in_buffer);

    if (!space_left) {
        return -1;
    }

    result = avs_net_socket_receive(stream->socket,
                                  out_bytes_read,
                                  avs_buffer_raw_insert_ptr(in_buffer),
                                  space_left);

    if (!result) {
        avs_buffer_advance_ptr(in_buffer, *out_bytes_read);
    } else {
        set_errno(stream);
    }
    return result;
}

static int read_data_through_internal_buffer(buffered_netstream_t *stream,
                                             size_t *out_bytes_read,
                                             char *out_message_finished,
                                             void *buffer,
                                             size_t buffer_length) {
    if (in_buffer_read_some(stream, out_bytes_read)) {
        *out_message_finished = 0;
        return -1;
    } else {
        if (avs_buffer_data_size(stream->in_buffer) > 0) {
            return return_data_from_buffer(stream->in_buffer,
                                           out_bytes_read,
                                           out_message_finished,
                                           buffer,
                                           buffer_length);
        } else {
            *out_bytes_read = 0;
            *out_message_finished = 1;
            return 0;
        }
    }
}

static int read_new_data(buffered_netstream_t *stream,
                         size_t *out_bytes_read,
                         char *out_message_finished,
                         void *buffer,
                         size_t buffer_length) {
    if (buffer_length >= avs_buffer_capacity(stream->in_buffer)) {
        return read_data_to_user_buffer(stream,
                                        out_bytes_read,
                                        out_message_finished,
                                        buffer,
                                        buffer_length);
    } else {
        return read_data_through_internal_buffer(stream,
                                                 out_bytes_read,
                                                 out_message_finished,
                                                 buffer,
                                                 buffer_length);
    }
}

static int buffered_netstream_read(avs_stream_abstract_t *stream_,
                                   size_t *out_bytes_read,
                                   char *out_message_finished,
                                   void *buffer,
                                   size_t buffer_length) {
    size_t bytes_read;
    char message_finished;
    buffered_netstream_t *stream = (buffered_netstream_t *) stream_;
    if (!out_bytes_read) {
        out_bytes_read = &bytes_read;
    }
    if (!out_message_finished) {
        out_message_finished = &message_finished;
    }

    if (avs_buffer_data_size(stream->in_buffer) > 0) {
        return return_data_from_buffer(stream->in_buffer,
                                       out_bytes_read,
                                       out_message_finished,
                                       buffer,
                                       buffer_length);
    } else {
        return read_new_data(stream,
                             out_bytes_read,
                             out_message_finished,
                             buffer,
                             buffer_length);

    }
}

static int buffered_netstream_peek(avs_stream_abstract_t *stream_,
                                   size_t offset) {
    buffered_netstream_t *stream = (buffered_netstream_t *) stream_;

    if (offset < avs_buffer_capacity(stream->in_buffer)) {
        while (offset >= avs_buffer_data_size(stream->in_buffer)) {
            size_t bytes_read;
            if (in_buffer_read_some(stream, &bytes_read)) {
                return EOF;
            } else if (bytes_read == 0) {
                return EOF;
            }
        }
        return (unsigned char)avs_buffer_data(stream->in_buffer)[offset];
    } else {
        errno = EINVAL;
        set_errno(stream);
        return EOF;
    }
}

static int buffered_netstream_reset(avs_stream_abstract_t *stream_) {
    buffered_netstream_t *stream = (buffered_netstream_t *) stream_;
    stream->errno_ = 0;
    avs_buffer_reset(stream->in_buffer);
    avs_buffer_reset(stream->out_buffer);
    return 0;
}

static int buffered_netstream_close(avs_stream_abstract_t *stream_) {
    buffered_netstream_t *stream = (buffered_netstream_t *) stream_;
    if (stream->socket) {
        avs_net_socket_shutdown(stream->socket);
    }
    avs_net_socket_cleanup(&stream->socket);
    free(stream->in_buffer);
    avs_buffer_free(&stream->out_buffer);
    return 0;
}

static int buffered_netstream_getsock(avs_stream_abstract_t *stream_,
                                      avs_net_abstract_socket_t **out_socket) {
    buffered_netstream_t *stream = (buffered_netstream_t *) stream_;
    *out_socket = stream->socket;
    return 0;
}

static int buffered_netstream_setsock(avs_stream_abstract_t *stream_,
                                      avs_net_abstract_socket_t *socket) {
    buffered_netstream_t *stream = (buffered_netstream_t *) stream_;
    stream->socket = socket;
    return 0;
}

static int buffered_netstream_errno(avs_stream_abstract_t *stream) {
    return ((buffered_netstream_t *) stream)->errno_;
}

static int unimplemented() {
    return -1;
}

static const avs_stream_v_table_extension_net_t
buffered_netstream_net_vtable = {
    buffered_netstream_getsock,
    buffered_netstream_setsock
};

static const avs_stream_v_table_extension_t
buffered_netstream_vtable_extensions[] = {
    { AVS_STREAM_V_TABLE_EXTENSION_NET, &buffered_netstream_net_vtable },
    AVS_STREAM_V_TABLE_EXTENSION_NULL
};

static const avs_stream_v_table_t buffered_netstream_vtable = {
    buffered_netstream_write,
    buffered_netstream_finish_message,
    buffered_netstream_read,
    buffered_netstream_peek,
    (avs_stream_write_subchannel_t) unimplemented,
    buffered_netstream_reset,
    buffered_netstream_close,
    buffered_netstream_errno,
    buffered_netstream_vtable_extensions
};

int avs_stream_netbuf_create(avs_stream_abstract_t **stream_,
                                    avs_net_abstract_socket_t *socket,
                                    size_t in_buffer_size,
                                    size_t out_buffer_size) {
    buffered_netstream_t *stream = NULL;
    *stream_ = (avs_stream_abstract_t*) calloc(1, sizeof(buffered_netstream_t));
    if (!*stream_) {
        return -1;
    }
    stream = (buffered_netstream_t*) * stream_;
    *(const avs_stream_v_table_t **) (intptr_t) &stream->vtable =
            &buffered_netstream_vtable;

    stream->socket = socket;
    if (avs_buffer_create(&stream->in_buffer, in_buffer_size)) {
        goto buffered_netstream_create_error;
    }
    if (avs_buffer_create(&stream->out_buffer, out_buffer_size)) {
        goto buffered_netstream_create_error;
    }
    return 0;

buffered_netstream_create_error:
    avs_buffer_free(&stream->in_buffer);
    avs_buffer_free(&stream->out_buffer);
    free(*stream_);
    *stream_ = NULL;
    return -1;
}

int avs_stream_netbuf_transfer(avs_stream_abstract_t *destination_,
                                      avs_stream_abstract_t *source_) {
    buffered_netstream_t *destination = (buffered_netstream_t *) destination_;
    buffered_netstream_t *source = (buffered_netstream_t *) source_;

    if (source->vtable != &buffered_netstream_vtable
            || destination->vtable != &buffered_netstream_vtable) {
        return -1;
    }

    if (avs_buffer_space_left(destination->out_buffer)
            < avs_buffer_data_size(source->out_buffer)
            || avs_buffer_space_left(destination->in_buffer)
            < avs_buffer_data_size(source->in_buffer)) {
        return -1;
    }

    avs_buffer_append_bytes(destination->out_buffer,
                            avs_buffer_data(source->out_buffer),
                            avs_buffer_data_size(source->out_buffer));
    avs_buffer_reset(source->out_buffer);

    avs_buffer_append_bytes(destination->in_buffer,
                            avs_buffer_data(source->in_buffer),
                            avs_buffer_data_size(source->in_buffer));

    avs_buffer_reset(source->in_buffer);

    return 0;
}

int avs_stream_netbuf_out_buffer_left(avs_stream_abstract_t *str) {
    buffered_netstream_t *stream = (buffered_netstream_t *) str;
    if (stream->vtable != &buffered_netstream_vtable) {
        return -1;
    }
    return (int) avs_buffer_space_left(stream->out_buffer);
}

void avs_stream_netbuf_set_recv_timeout(avs_stream_abstract_t *str,
                                               int timeout_ms) {
    buffered_netstream_t *stream = (buffered_netstream_t *) str;
    avs_net_socket_opt_value_t timeout_opt;

    timeout_opt.recv_timeout = timeout_ms;
    avs_net_socket_set_opt(stream->socket,
                           AVS_NET_SOCKET_OPT_RECV_TIMEOUT,
                           timeout_opt);
}