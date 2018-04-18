/*
 * Copyright 2018 AVSystem <avsystem@avsystem.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <avs_commons_config.h>

#include <assert.h>
#include <inttypes.h>

#include <avsystem/commons/persistence.h>
#include <avsystem/commons/utils.h>

#define MODULE_NAME avs_persistence
#include <x_log_config.h>

VISIBILITY_SOURCE_BEGIN

typedef int persistence_handler_u16_t(avs_persistence_context_t *ctx,
                                      uint16_t *value);
typedef int persistence_handler_u32_t(avs_persistence_context_t *ctx,
                                      uint32_t *value);
typedef int persistence_handler_u64_t(avs_persistence_context_t *ctx,
                                      uint64_t *value);
typedef int persistence_handler_bool_t(avs_persistence_context_t *ctx,
                                       bool *value);
typedef int persistence_handler_bytes_t(avs_persistence_context_t *ctx,
                                        void *buffer,
                                        size_t buffer_size);
typedef int persistence_handler_float_t(avs_persistence_context_t *ctx,
                                        float *value);
typedef int persistence_handler_double_t(avs_persistence_context_t *ctx,
                                         double *value);
typedef int persistence_handler_sized_buffer_t(avs_persistence_context_t *ctx,
                                               void **data_ptr,
                                               size_t *size_ptr);
typedef int persistence_handler_string_t(avs_persistence_context_t *ctx,
                                         char **string_ptr);
typedef int
persistence_handler_list_t(avs_persistence_context_t *ctx,
                           AVS_LIST(void) *list_ptr,
                           size_t element_size,
                           avs_persistence_handler_collection_element_t *handler,
                           void *handler_user_ptr,
                           avs_persistence_cleanup_collection_element_t *cleanup);
typedef int
persistence_handler_tree_t(avs_persistence_context_t *ctx,
                           AVS_RBTREE(void) tree,
                           size_t element_size,
                           avs_persistence_handler_collection_element_t *handler,
                           void *handler_user_ptr,
                           avs_persistence_cleanup_collection_element_t *cleanup);

struct avs_persistence_context_struct {
    avs_persistence_direction_t direction;
    persistence_handler_u16_t *handle_u16;
    persistence_handler_u32_t *handle_u32;
    persistence_handler_u64_t *handle_u64;
    persistence_handler_bool_t *handle_bool;
    persistence_handler_bytes_t *handle_bytes;
    persistence_handler_float_t *handle_float;
    persistence_handler_double_t *handle_double;
    persistence_handler_sized_buffer_t *handle_sized_buffer;
    persistence_handler_string_t *handle_string;
    persistence_handler_list_t *handle_list;
    persistence_handler_tree_t *handle_tree;
    avs_stream_abstract_t *stream;
};

//// PERSIST ///////////////////////////////////////////////////////////////////

static int persist_bool(avs_persistence_context_t *ctx, bool *value) {
    AVS_STATIC_ASSERT(sizeof(*value) == 1, bool_is_1byte);
    return avs_stream_write(ctx->stream, value, 1);
}

static int persist_bytes(avs_persistence_context_t *ctx,
                         void *buffer,
                         size_t buffer_size) {
    return avs_stream_write(ctx->stream, buffer, buffer_size);
}

static int persist_u16(avs_persistence_context_t *ctx, uint16_t *value) {
    AVS_STATIC_ASSERT(sizeof(*value) == 2, u16_is_2bytes);
    uint16_t tmp = avs_convert_be16(*value);
    return avs_stream_write(ctx->stream, &tmp, 2);
}

static int persist_u32(avs_persistence_context_t *ctx, uint32_t *value) {
    AVS_STATIC_ASSERT(sizeof(*value) == 4, u32_is_4bytes);
    uint32_t tmp = avs_convert_be32(*value);
    return avs_stream_write(ctx->stream, &tmp, 4);
}

static int persist_u64(avs_persistence_context_t *ctx, uint64_t *value) {
    AVS_STATIC_ASSERT(sizeof(*value) == 8, u64_is_8bytes);
    uint64_t tmp = avs_convert_be64(*value);
    return avs_stream_write(ctx->stream, &tmp, 8);
}

static int persist_float(avs_persistence_context_t *ctx, float *value) {
    uint32_t value_be = avs_htonf(*value);
    AVS_STATIC_ASSERT(sizeof(*value) == sizeof(value_be), float_is_32);
    AVS_STATIC_ASSERT(sizeof(value_be) == 4, u32_is_4bytes);
    return avs_stream_write(ctx->stream, &value_be, 4);
}

static int persist_double(avs_persistence_context_t *ctx, double *value) {
    uint64_t value_be = avs_htond(*value);
    AVS_STATIC_ASSERT(sizeof(*value) == sizeof(value_be), double_is_64);
    AVS_STATIC_ASSERT(sizeof(value_be) == 8, u64_is_8bytes);
    return avs_stream_write(ctx->stream, &value_be, 8);
}

static int persist_sized_buffer(avs_persistence_context_t *ctx,
                                void **data_ptr,
                                size_t *size_ptr) {
    uint32_t size32 = (uint32_t) *size_ptr;
    if (size32 != *size_ptr) {
        LOG(ERROR, "Element too big to persist");
    }
    int retval = persist_u32(ctx, &size32);
    if (!retval && size32 > 0) {
        retval = persist_bytes(ctx, *data_ptr, *size_ptr);
    }
    return retval;
}

static int persist_string(avs_persistence_context_t *ctx,
                          char **string_ptr) {
    size_t size = (string_ptr && *string_ptr) ? (strlen(*string_ptr) + 1) : 0;
    return persist_sized_buffer(ctx, (void **) string_ptr, &size);
}

static int persist_list(avs_persistence_context_t *ctx,
                        AVS_LIST(void) *list_ptr,
                        size_t element_size,
                        avs_persistence_handler_collection_element_t *handler,
                        void *handler_user_ptr,
                        avs_persistence_cleanup_collection_element_t *cleanup) {
    (void) element_size; (void) cleanup;
    size_t count = AVS_LIST_SIZE(*list_ptr);
    uint32_t count32 = (uint32_t) count;
    if (count != count32) {
        return -1;
    }
    int retval = persist_u32(ctx, &count32);
    if (!retval) {
        AVS_LIST(void) element;
        AVS_LIST_FOREACH(element, *list_ptr) {
            if ((retval = handler(ctx, element, handler_user_ptr))) {
                break;
            }
        }
    }
    return retval;
}

static int persist_tree(avs_persistence_context_t *ctx,
                        AVS_RBTREE(void) tree,
                        size_t element_size,
                        avs_persistence_handler_collection_element_t *handler,
                        void *handler_user_ptr,
                        avs_persistence_cleanup_collection_element_t *cleanup) {
    (void) element_size; (void) cleanup;
    size_t count = AVS_RBTREE_SIZE(tree);
    uint32_t count32 = (uint32_t) count;
    if (count != count32) {
        return -1;
    }
    int retval = persist_u32(ctx, &count32);
    if (!retval) {
        AVS_RBTREE_ELEM(void) element;
        AVS_RBTREE_FOREACH(element, tree) {
            if ((retval = handler(ctx, element, handler_user_ptr))) {
                break;
            }
        }
    }
    return retval;
}

#define INIT_STORE_CONTEXT(Stream) { \
            AVS_PERSISTENCE_STORE, \
            persist_u16, \
            persist_u32, \
            persist_u64, \
            persist_bool, \
            persist_bytes, \
            persist_float, \
            persist_double, \
            persist_sized_buffer, \
            persist_string, \
            persist_list, \
            persist_tree, \
            Stream \
        }

//// RESTORE ///////////////////////////////////////////////////////////////////

static int restore_bool(avs_persistence_context_t *ctx, bool *out) {
    AVS_STATIC_ASSERT(sizeof(*out) == 1, bool_is_1byte);
    return avs_stream_read_reliably(ctx->stream, out, 1);
}

static int restore_bytes(avs_persistence_context_t *ctx,
                         void *buffer,
                         size_t buffer_size) {
    return avs_stream_read_reliably(ctx->stream, buffer, buffer_size);
}

static int restore_u16(avs_persistence_context_t *ctx, uint16_t *out) {
    AVS_STATIC_ASSERT(sizeof(*out) == 2, u16_is_2bytes);
    uint16_t tmp;
    int retval = avs_stream_read_reliably(ctx->stream, &tmp, 2);
    if (!retval && out) {
        *out = avs_convert_be16(tmp);
    }
    return retval;
}

static int restore_u32(avs_persistence_context_t *ctx, uint32_t *out) {
    AVS_STATIC_ASSERT(sizeof(*out) == 4, u32_is_4bytes);
    uint32_t tmp;
    int retval = avs_stream_read_reliably(ctx->stream, &tmp, 4);
    if (!retval) {
        *out = avs_convert_be32(tmp);
    }
    return retval;
}

static int restore_u64(avs_persistence_context_t *ctx, uint64_t *out) {
    AVS_STATIC_ASSERT(sizeof(*out) == 8, u64_is_8bytes);
    uint64_t tmp;
    int retval = avs_stream_read_reliably(ctx->stream, &tmp, 8);
    if (!retval) {
        *out = avs_convert_be64(tmp);
    }
    return retval;
}

static int restore_float(avs_persistence_context_t *ctx, float *out) {
    uint32_t tmp;
    AVS_STATIC_ASSERT(sizeof(*out) == sizeof(tmp), float_is_32);
    AVS_STATIC_ASSERT(sizeof(tmp) == 4, u32_is_4bytes);
    int retval = avs_stream_read_reliably(ctx->stream, &tmp, 4);
    if (!retval) {
        *out = avs_ntohf(tmp);
    }
    return retval;
}

static int restore_double(avs_persistence_context_t *ctx, double *out) {
    uint64_t tmp;
    AVS_STATIC_ASSERT(sizeof(*out) == sizeof(tmp), double_is_64);
    AVS_STATIC_ASSERT(sizeof(tmp) == 8, u64_is_8bytes);
    int retval = avs_stream_read_reliably(ctx->stream, &tmp, 8);
    if (!retval) {
        *out = avs_ntohd(tmp);
    }
    return retval;
}

static int restore_sized_buffer(avs_persistence_context_t *ctx,
                                void **data_ptr,
                                size_t *size_ptr) {
    assert(!*data_ptr);
    assert(!*size_ptr);
    uint32_t size32;
    int retval = restore_u32(ctx, &size32);
    if (retval) {
        return retval;
    }
    if (size32 == 0) {
        return 0;
    }
    if (!(*data_ptr = malloc(size32))) {
        LOG(ERROR, "Cannot allocate %" PRIu32 " bytes", size32);
        return -1;
    }
    if ((retval = restore_bytes(ctx, *data_ptr, size32))) {
        free(*data_ptr);
        *data_ptr = NULL;
    } else {
        *size_ptr = size32;
    }
    return retval;
}

static int restore_string(avs_persistence_context_t *ctx,
                          char **string_ptr) {
    size_t size = 0;
    int retval = restore_sized_buffer(ctx, (void **) string_ptr, &size);
    if (retval) {
        return retval;
    }
    if (size > 0 && (*string_ptr)[size - 1] != '\0') {
        LOG(ERROR, "Invalid string");
        free(*string_ptr);
        *string_ptr = NULL;
        return -1;
    }
    return 0;
}

static int restore_list(avs_persistence_context_t *ctx,
                        AVS_LIST(void) *list_ptr,
                        size_t element_size,
                        avs_persistence_handler_collection_element_t *handler,
                        void *handler_user_ptr,
                        avs_persistence_cleanup_collection_element_t *cleanup) {
    assert(list_ptr && !*list_ptr);
    uint32_t count;
    int retval = restore_u32(ctx, &count);
    if (!retval) {
        AVS_LIST(void) *insert_ptr = list_ptr;
        while (count--) {
            AVS_LIST(void) element = AVS_LIST_NEW_BUFFER(element_size);
            if (!element) {
                LOG(ERROR, "Out of memory");
                retval = -1;
                break;
            }
            AVS_LIST_INSERT(insert_ptr, element);
            insert_ptr = AVS_LIST_NEXT_PTR(insert_ptr);
            if ((retval = handler(ctx, element, handler_user_ptr))) {
                break;
            }
        }
    }
    if (retval && cleanup) {
        AVS_LIST_CLEAR(list_ptr) {
            cleanup(*list_ptr);
        }
    }
    return retval;
}

static int restore_tree(avs_persistence_context_t *ctx,
                        AVS_RBTREE(void) tree,
                        size_t element_size,
                        avs_persistence_handler_collection_element_t *handler,
                        void *handler_user_ptr,
                        avs_persistence_cleanup_collection_element_t *cleanup) {
    assert(AVS_RBTREE_SIZE(tree) == 0);
    assert(cleanup);
    uint32_t count;
    int retval = restore_u32(ctx, &count);
    if (!retval) {
        while (count--) {
            AVS_RBTREE_ELEM(void) element =
                    AVS_RBTREE_ELEM_NEW_BUFFER(element_size);
            if (!element) {
                LOG(ERROR, "Out of memory");
                retval = -1;
                break;
            }
            if (!(retval = handler(ctx, element, handler_user_ptr))) {
                if (AVS_RBTREE_INSERT(tree, element) != element) {
                    retval = -1;
                }
            }
            if (retval) {
                cleanup(element);
                AVS_RBTREE_ELEM_DELETE_DETACHED(&element);
                break;
            }
        }
    }
    if (retval) {
        AVS_RBTREE_CLEAR(tree) {
            cleanup(*tree);
        }
    }
    return retval;
}

#define INIT_RESTORE_CONTEXT(Stream) { \
            AVS_PERSISTENCE_RESTORE, \
            restore_u16, \
            restore_u32, \
            restore_u64, \
            restore_bool, \
            restore_bytes, \
            restore_float, \
            restore_double, \
            restore_sized_buffer, \
            restore_string, \
            restore_list, \
            restore_tree, \
            .stream = Stream \
        }

//// IGNORE ////////////////////////////////////////////////////////////////////

static int ignore_bool(avs_persistence_context_t *ctx, bool *out) {
    (void) out;
    bool tmp;
    AVS_STATIC_ASSERT(sizeof(*out) == 1, bool_is_1byte);
    return avs_stream_read_reliably(ctx->stream, &tmp, 1);
}

#define PERSISTENCE_IGNORE_BYTES_BUFSIZE 512

static int ignore_bytes(avs_persistence_context_t *ctx,
                        void *buffer,
                        size_t buffer_size) {
    (void) buffer;
    uint8_t buf[PERSISTENCE_IGNORE_BYTES_BUFSIZE];
    while (buffer_size > 0) {
        size_t chunk_to_ignore =
                buffer_size < sizeof(buf) ? buffer_size : sizeof(buf);
        int retval =
                avs_stream_read_reliably(ctx->stream, buf, chunk_to_ignore);
        if (retval) {
            return retval;
        }
        buffer_size -= chunk_to_ignore;
    }
    return 0;
}

static int ignore_u16(avs_persistence_context_t *ctx, uint16_t *out) {
    (void) out;
    uint16_t tmp;
    AVS_STATIC_ASSERT(sizeof(*out) == 2, u16_is_2bytes);
    return avs_stream_read_reliably(ctx->stream, &tmp, 2);
}

static int ignore_u32(avs_persistence_context_t *ctx, uint32_t *out) {
    (void) out;
    uint32_t tmp;
    AVS_STATIC_ASSERT(sizeof(*out) == 4, u32_is_4bytes);
    return avs_stream_read_reliably(ctx->stream, &tmp, 4);
}

static int ignore_u64(avs_persistence_context_t *ctx, uint64_t *out) {
    (void) out;
    uint64_t tmp;
    AVS_STATIC_ASSERT(sizeof(*out) == 8, u64_is_8bytes);
    return avs_stream_read_reliably(ctx->stream, &tmp, 8);
}

static int ignore_float(avs_persistence_context_t *ctx, float *out) {
    (void) out;
    uint32_t tmp;
    AVS_STATIC_ASSERT(sizeof(*out) == sizeof(tmp), float_is_32);
    AVS_STATIC_ASSERT(sizeof(tmp) == 4, u32_is_4bytes);
    return avs_stream_read_reliably(ctx->stream, &tmp, 4);
}

static int ignore_double(avs_persistence_context_t *ctx, double *out) {
    (void) out;
    uint64_t tmp;
    AVS_STATIC_ASSERT(sizeof(*out) == sizeof(tmp), double_is_64);
    AVS_STATIC_ASSERT(sizeof(tmp) == 8, u64_is_8bytes);
    return avs_stream_read_reliably(ctx->stream, &tmp, 8);
}

static int ignore_collection(
        avs_persistence_context_t *ctx,
        avs_persistence_handler_collection_element_t *handler,
        void *handler_user_ptr) {
    uint32_t count;
    int retval = restore_u32(ctx, &count);
    while (!retval && count--) {
        retval = handler(ctx, NULL, handler_user_ptr);
    }
    return retval;
}

static int ignore_sized_buffer(avs_persistence_context_t *ctx,
                               void **data_ptr,
                               size_t *size_ptr) {
    (void) data_ptr;
    (void) size_ptr;
    uint32_t size32;
    int retval = restore_u32(ctx, &size32);
    if (!retval) {
        retval = ignore_bytes(ctx, NULL, size32);
    }
    return retval;
}

static int ignore_string(avs_persistence_context_t *ctx,
                         char **string_ptr) {
    (void) string_ptr;
    return ignore_sized_buffer(ctx, NULL, NULL);
}

static int ignore_list(avs_persistence_context_t *ctx,
                       AVS_LIST(void) *list_ptr,
                       size_t element_size,
                       avs_persistence_handler_collection_element_t *handler,
                       void *handler_user_ptr,
                       avs_persistence_cleanup_collection_element_t *cleanup) {
    (void) list_ptr; (void) element_size; (void) cleanup;
    return ignore_collection(ctx, handler, handler_user_ptr);
}

static int ignore_tree(avs_persistence_context_t *ctx,
                       AVS_RBTREE(void) tree,
                       size_t element_size,
                       avs_persistence_handler_collection_element_t *handler,
                       void *handler_user_ptr,
                       avs_persistence_cleanup_collection_element_t *cleanup) {
    (void) tree; (void) element_size; (void) cleanup;
    return ignore_collection(ctx, handler, handler_user_ptr);
}

#define INIT_IGNORE_CONTEXT(Stream) { \
            AVS_PERSISTENCE_RESTORE, \
            ignore_u16, \
            ignore_u32, \
            ignore_u64, \
            ignore_bool, \
            ignore_bytes, \
            ignore_float, \
            ignore_double, \
            ignore_sized_buffer, \
            ignore_string, \
            ignore_list, \
            ignore_tree, \
            Stream \
        }

avs_persistence_context_t *
avs_persistence_store_context_new(avs_stream_abstract_t *stream) {
    if (!stream) {
        return NULL;
    }
    avs_persistence_context_t *ctx = (avs_persistence_context_t *)
            calloc(1, sizeof(avs_persistence_context_t));
    if (ctx) {
        *ctx = (avs_persistence_context_t) INIT_STORE_CONTEXT(stream);
    }
    return ctx;
}

avs_persistence_context_t *
avs_persistence_restore_context_new(avs_stream_abstract_t *stream) {
    if (!stream) {
        return NULL;
    }
    avs_persistence_context_t *ctx = (avs_persistence_context_t *)
            calloc(1, sizeof(avs_persistence_context_t));
    if (ctx) {
        *ctx = (avs_persistence_context_t) INIT_RESTORE_CONTEXT(stream);
    }
    return ctx;
}

avs_persistence_context_t *
avs_persistence_ignore_context_new(avs_stream_abstract_t *stream) {
    if (!stream) {
        return NULL;
    }
    avs_persistence_context_t *ctx = (avs_persistence_context_t *)
            calloc(1, sizeof(avs_persistence_context_t));
    if (ctx) {
        *ctx = (avs_persistence_context_t) INIT_IGNORE_CONTEXT(stream);
    }
    return ctx;
}

void avs_persistence_context_delete(avs_persistence_context_t *ctx) {
    free(ctx);
}

avs_persistence_direction_t
avs_persistence_direction(avs_persistence_context_t *ctx) {
    if (!ctx) {
        return AVS_PERSISTENCE_UNKNOWN;
    }
    return ctx->direction;
}

int avs_persistence_u8(avs_persistence_context_t *ctx,
                       uint8_t *value) {
    AVS_STATIC_ASSERT(sizeof(*value) == 1, uint8_is_1_byte);
    if (!ctx) {
        return -1;
    }
    return ctx->handle_bytes(ctx, value, 1);
}

int avs_persistence_u16(avs_persistence_context_t *ctx,
                        uint16_t *value) {
    if (!ctx) {
        return -1;
    }
    return ctx->handle_u16(ctx, value);
}

int avs_persistence_u32(avs_persistence_context_t *ctx,
                        uint32_t *value) {
    if (!ctx) {
        return -1;
    }
    return ctx->handle_u32(ctx, value);
}

int avs_persistence_u64(avs_persistence_context_t *ctx,
                        uint64_t *value) {
    if (!ctx) {
        return -1;
    }
    return ctx->handle_u64(ctx, value);
}

int avs_persistence_i8(avs_persistence_context_t *ctx,
                       int8_t *value) {
    AVS_STATIC_ASSERT(sizeof(*value) == 1, int8_is_1_byte);
    if (!ctx) {
        return -1;
    }
    return ctx->handle_bytes(ctx, value, 1);
}

int avs_persistence_i16(avs_persistence_context_t *ctx,
                        int16_t *value) {
    AVS_STATIC_ASSERT(sizeof(*value) == sizeof(uint16_t),
                      int16_uint16_equivalent);
    return avs_persistence_u16(ctx, (uint16_t *) value);
}

int avs_persistence_i32(avs_persistence_context_t *ctx,
                        int32_t *value) {
    AVS_STATIC_ASSERT(sizeof(*value) == sizeof(uint32_t),
                      int32_uint32_equivalent);
    return avs_persistence_u32(ctx, (uint32_t *) value);
}

int avs_persistence_i64(avs_persistence_context_t *ctx,
                        int64_t *value) {
    AVS_STATIC_ASSERT(sizeof(*value) == sizeof(uint64_t),
                      int64_uint64_equivalent);
    return avs_persistence_u64(ctx, (uint64_t *) value);
}

int avs_persistence_bool(avs_persistence_context_t *ctx, bool *value) {
    if (!ctx) {
        return -1;
    }
    return ctx->handle_bool(ctx, value);
}

int avs_persistence_bytes(avs_persistence_context_t *ctx,
                          void *buffer,
                          size_t buffer_size) {
    if (!ctx) {
        return -1;
    }
    return ctx->handle_bytes(ctx, buffer, buffer_size);
}

int avs_persistence_float(avs_persistence_context_t *ctx,
                          float *value) {
    if (!ctx) {
        return -1;
    }
    return ctx->handle_float(ctx, value);
}

int avs_persistence_double(avs_persistence_context_t *ctx,
                           double *value) {
    if (!ctx) {
        return -1;
    }
    return ctx->handle_double(ctx, value);
}

int avs_persistence_sized_buffer(avs_persistence_context_t *ctx,
                                 void **data_ptr,
                                 size_t *size_ptr) {
    if (!ctx) {
        return -1;
    }
    return ctx->handle_sized_buffer(ctx, data_ptr, size_ptr);
}

int avs_persistence_string(avs_persistence_context_t *ctx,
                           char **string_ptr) {
    if (!ctx) {
        return -1;
    }
    return ctx->handle_string(ctx, string_ptr);
}

int avs_persistence_list(
        avs_persistence_context_t *ctx,
        AVS_LIST(void) *list_ptr,
        size_t element_size,
        avs_persistence_handler_collection_element_t *handler,
        void *handler_user_ptr,
        avs_persistence_cleanup_collection_element_t *cleanup) {
    if (!ctx) {
        return -1;
    }
    return ctx->handle_list(ctx, list_ptr, element_size,
                            handler, handler_user_ptr, cleanup);
}

int avs_persistence_tree(
        avs_persistence_context_t *ctx,
        AVS_RBTREE(void) tree,
        size_t element_size,
        avs_persistence_handler_collection_element_t *handler,
        void *handler_user_ptr,
        avs_persistence_cleanup_collection_element_t *cleanup) {
    if (!ctx) {
        return -1;
    }
    return ctx->handle_tree(ctx, tree, element_size,
                            handler, handler_user_ptr, cleanup);
}

#ifdef AVS_UNIT_TESTING
#include "test/persistence.c"
#endif
