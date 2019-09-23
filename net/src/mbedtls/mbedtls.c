/*
 * Copyright 2017-2019 AVSystem <avsystem@avsystem.com>
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

// this uses some symbols such as "printf" - include it before poisoning them
#include <mbedtls/platform.h>

#include <avs_commons_config.h>

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>

#if !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 199901L)
#    define inline
#endif

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/platform.h>
#include <mbedtls/version.h>
#if MBEDTLS_VERSION_NUMBER >= 0x02040000 // mbed TLS 2.4 deprecated net.h
#    include <mbedtls/net_sockets.h>
#else // support mbed TLS <=2.3
#    include <mbedtls/net.h>
#endif
#include <mbedtls/ssl.h>
#include <mbedtls/timing.h>
#ifdef WITH_MBEDTLS_LOGS
#    include <mbedtls/debug.h>
#endif // WITH_MBEDTLS_LOGS

#include <avsystem/commons/errno_map.h>
#include <avsystem/commons/memory.h>
#include <avsystem/commons/utils.h>

#include "../global.h"
#include "../net_impl.h"
#ifdef WITH_X509
#    include "data_loader.h"
#endif // WITH_X509
#include "mbedtls_persistence.h"

VISIBILITY_SOURCE_BEGIN

#ifdef WITH_X509
typedef struct {
    mbedtls_x509_crt *ca_cert;
    mbedtls_x509_crt *client_cert;
    mbedtls_pk_context *client_key;
} ssl_socket_certs_t;
#endif // WITH_X509

typedef struct {
    const avs_net_socket_v_table_t *const operations;
    struct {
        bool context_valid : 1;
        bool session_restored : 1;
    } flags;
    mbedtls_ssl_context context;
    mbedtls_ssl_config config;
#ifdef WITH_TLS_SESSION_PERSISTENCE
    void *session_resumption_buffer;
    size_t session_resumption_buffer_size;
#endif // WITH_TLS_SESSION_PERSISTENCE
    avs_net_security_mode_t security_mode;
    union {
#ifdef WITH_X509
        ssl_socket_certs_t cert;
#endif // WITH_X509
#ifdef WITH_PSK
        avs_net_owned_psk_t psk;
#endif // WITH_PSK
    } security;
    mbedtls_timing_delay_context timer;
    avs_net_socket_type_t backend_type;
    avs_net_abstract_socket_t *backend_socket;
    avs_error_t bio_error;
    avs_net_socket_configuration_t backend_configuration;
    /// Set of ciphersuites configured by user
    avs_net_socket_tls_ciphersuites_t enabled_ciphersuites;
    /// Subset of @ref ssl_socket_t#enabled_ciphersuites appropriate for
    /// security mode, 0-terminated array
    int *effective_ciphersuites;
    /// Non empty, when custom server hostname shall be used.
    char server_name_indication[256];
    avs_net_ssl_alert_t last_alert;
    bool use_connection_id;
} ssl_socket_t;

static bool is_ssl_started(ssl_socket_t *socket) {
    return socket->flags.context_valid;
}

static bool is_session_resumed(ssl_socket_t *socket) {
    return socket->flags.session_restored;
}

static mbedtls_ssl_context *get_context(ssl_socket_t *socket) {
    assert(socket->flags.context_valid);
    return &socket->context;
}

#ifdef WITH_MBEDTLS_LOGS
static void debug_mbedtls(
        void *ctx, int level, const char *file, int line, const char *str) {
    (void) ctx;
    (void) level;
    const size_t len = strlen(str);
    const char *msg = str;
    char msgbuf[len + 1];
    if (len > 0 && str[len - 1] == '\n') {
        memset(msgbuf, 0, len);
        memcpy(msgbuf, str, len - 1);
        msg = msgbuf;
    }
    avs_log_internal_l__(AVS_LOG_TRACE, "mbedtls", file, (unsigned) line, "%s",
                         msg);
}
#endif // WITH_MBEDTLS_LOGS

#define NET_SSL_COMMON_INTERNALS
#include "../ssl_common.h"

static void update_alert_if_any(ssl_socket_t *socket) {
    mbedtls_ssl_context *context = get_context(socket);
    if (context->in_msgtype == AVS_TLS_MESSAGE_TYPE_ALERT) {
        socket->last_alert = (avs_net_ssl_alert_t) {
            .alert_level = context->in_msg[0],
            .alert_description = context->in_msg[1]
        };
        LOG(DEBUG, "alert_level = %u, alert_description = %u",
            socket->last_alert.alert_level,
            socket->last_alert.alert_description);
    }
}

static struct {
    // this weighs almost 40KB because of HAVEGE state
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context rng;
} AVS_SSL_GLOBAL;

void _avs_net_cleanup_global_ssl_state(void) {
    mbedtls_ctr_drbg_free(&AVS_SSL_GLOBAL.rng);
    mbedtls_entropy_free(&AVS_SSL_GLOBAL.entropy);
}

avs_error_t _avs_net_initialize_global_ssl_state(void) {
    avs_error_t err = AVS_OK;
    mbedtls_entropy_init(&AVS_SSL_GLOBAL.entropy);
#ifdef AVS_COMMONS_WITH_MBEDTLS_CUSTOM_ENTROPY_INITIALIZER
    err = avs_net_mbedtls_entropy_init(&AVS_SSL_GLOBAL.entropy);
#endif // AVS_COMMONS_WITH_MBEDTLS_CUSTOM_ENTROPY_INITIALIZER
    if (avs_is_err(err)) {
        LOG(ERROR, "custom entropy initializer failed");
    } else {
        mbedtls_ctr_drbg_init(&AVS_SSL_GLOBAL.rng);
        if (mbedtls_ctr_drbg_seed(&AVS_SSL_GLOBAL.rng, mbedtls_entropy_func,
                                  &AVS_SSL_GLOBAL.entropy, NULL, 0)) {
            LOG(ERROR, "mbedtls_ctr_drbg_seed() failed");
            err = avs_errno(AVS_EPROTO);
        }
    }
    if (avs_is_err(err)) {
        _avs_net_cleanup_global_ssl_state();
    }
    return err;
}

static int
avs_bio_recv(void *ctx, unsigned char *buf, size_t len, uint32_t timeout_ms) {
    ssl_socket_t *socket = (ssl_socket_t *) ctx;
    avs_net_socket_opt_value_t orig_timeout;
    avs_net_socket_opt_value_t new_timeout;
    size_t read_bytes;
    int result;
    if (avs_is_err((socket->bio_error = avs_net_socket_get_opt(
                            socket->backend_socket,
                            AVS_NET_SOCKET_OPT_RECV_TIMEOUT,
                            &orig_timeout)))) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    new_timeout = orig_timeout;
    if (timeout_ms) {
        new_timeout.recv_timeout =
                avs_time_duration_from_scalar(timeout_ms, AVS_TIME_MS);
    }
    avs_net_socket_set_opt(socket->backend_socket,
                           AVS_NET_SOCKET_OPT_RECV_TIMEOUT, new_timeout);
    if (avs_is_err((socket->bio_error = avs_net_socket_receive(
                            socket->backend_socket, &read_bytes, buf, len)))) {
        if (socket->bio_error.category == AVS_ERRNO_CATEGORY
                && socket->bio_error.code == AVS_ETIMEDOUT) {
            result = MBEDTLS_ERR_SSL_TIMEOUT;
        } else {
            result = MBEDTLS_ERR_NET_RECV_FAILED;
        }
    } else {
        result = (int) read_bytes;
    }
    avs_net_socket_set_opt(socket->backend_socket,
                           AVS_NET_SOCKET_OPT_RECV_TIMEOUT, orig_timeout);
    return result;
}

static int avs_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    ssl_socket_t *socket = (ssl_socket_t *) ctx;
    if (avs_is_err((socket->bio_error = avs_net_socket_send(
                            socket->backend_socket, buf, len)))) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    } else {
        return (int) len;
    }
}

static avs_error_t get_dtls_overhead(ssl_socket_t *socket,
                                     int *out_header,
                                     int *out_padding_size) {
    if (!is_ssl_started(socket)) {
        return avs_errno(AVS_EBADF);
    }

    const mbedtls_ssl_ciphersuite_t *ciphersuite =
            mbedtls_ssl_ciphersuite_from_string(
                    mbedtls_ssl_get_ciphersuite(get_context(socket)));

    int result = mbedtls_ssl_get_record_expansion(get_context(socket));
    if (result == MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE
            || result == MBEDTLS_ERR_SSL_INTERNAL_ERROR) {
        /* This is either a result of compression mode or some internal
         * error, and in both cases we can't predict the size. */
        return avs_errno(AVS_EBADF);
    }

    const mbedtls_cipher_info_t *cipher =
            mbedtls_cipher_info_from_type(ciphersuite->cipher);

    *out_padding_size = 0;
    if (cipher->mode == MBEDTLS_MODE_CBC) {
        *out_padding_size = (int) cipher->block_size;
        /* Looking at the mbedtls_ssl_get_record_expansion it adds size
         * of the block to the record size, and we don't want that */
        result -= (int) cipher->block_size;
    }

    *out_header = result;
    return AVS_OK;
}

static void close_ssl_raw(ssl_socket_t *socket) {
    if (socket->backend_socket) {
        avs_net_socket_close(socket->backend_socket);
    }

    if (socket->flags.context_valid) {
        mbedtls_ssl_free(get_context(socket));
        socket->flags.context_valid = false;
    }
}

static int set_min_ssl_version(mbedtls_ssl_config *config,
                               avs_net_ssl_version_t version) {
    switch (version) {
    case AVS_NET_SSL_VERSION_DEFAULT:
    case AVS_NET_SSL_VERSION_SSLv2_OR_3:
    case AVS_NET_SSL_VERSION_SSLv3:
        mbedtls_ssl_conf_min_version(config, MBEDTLS_SSL_MAJOR_VERSION_3,
                                     MBEDTLS_SSL_MINOR_VERSION_0);
        return 0;
    case AVS_NET_SSL_VERSION_TLSv1:
        mbedtls_ssl_conf_min_version(config, MBEDTLS_SSL_MAJOR_VERSION_3,
                                     MBEDTLS_SSL_MINOR_VERSION_1);
        return 0;
    case AVS_NET_SSL_VERSION_TLSv1_1:
        mbedtls_ssl_conf_min_version(config, MBEDTLS_SSL_MAJOR_VERSION_3,
                                     MBEDTLS_SSL_MINOR_VERSION_2);
        return 0;
    case AVS_NET_SSL_VERSION_TLSv1_2:
        mbedtls_ssl_conf_min_version(config, MBEDTLS_SSL_MAJOR_VERSION_3,
                                     MBEDTLS_SSL_MINOR_VERSION_3);
        return 0;
    default:
        LOG(ERROR, "Unsupported SSL version");
        return -1;
    }
}

#if defined(WITH_X509) || defined(WITH_PSK)
static bool
contains_cipher(const avs_net_socket_tls_ciphersuites_t *enabled_ciphers,
                int cipher) {
    if (!enabled_ciphers->ids) {
        return true;
    } else {
        for (size_t i = 0; i < enabled_ciphers->num_ids; ++i) {
            if (enabled_ciphers->ids[i] == (uint32_t) cipher) {
                return true;
            }
        }
        return false;
    }
}
#endif // defined(WITH_X509) || defined(WITH_PSK)

#ifdef WITH_X509
static int *init_cert_ciphersuites(
        const avs_net_socket_tls_ciphersuites_t *enabled_ciphers) {
    const int *all_ciphers = mbedtls_ssl_list_ciphersuites();

    size_t ciphers_count = 0;
    for (const int *cipher = all_ciphers; cipher && *cipher; ++cipher) {
        if (contains_cipher(enabled_ciphers, *cipher)) {
            ++ciphers_count;
        }
    }

    int *ciphers = (int *) avs_calloc(ciphers_count + 1, sizeof(int));
    if (!ciphers) {
        LOG(ERROR, "out of memory");
        return NULL;
    }

    int *cipher_it = ciphers;
    for (const int *cipher = all_ciphers; cipher && *cipher; ++cipher) {
        if (contains_cipher(enabled_ciphers, *cipher)) {
            *cipher_it++ = *cipher;
        }
    }

    return ciphers;
}

static uint8_t is_verification_enabled(ssl_socket_t *socket) {
    return socket->security_mode == AVS_NET_SECURITY_CERTIFICATE
           && socket->security.cert.ca_cert != NULL;
}

static avs_error_t initialize_cert_security(ssl_socket_t *socket) {
    avs_free(socket->effective_ciphersuites);
    if (!(socket->effective_ciphersuites =
                  init_cert_ciphersuites(&socket->enabled_ciphersuites))) {
        return avs_errno(AVS_ENOMEM);
    }

    if (socket->security.cert.ca_cert) {
        mbedtls_ssl_conf_authmode(&socket->config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&socket->config,
                                  socket->security.cert.ca_cert, NULL);
    } else {
        mbedtls_ssl_conf_authmode(&socket->config, MBEDTLS_SSL_VERIFY_NONE);
    }

    if (socket->security.cert.client_cert && socket->security.cert.client_key) {
        mbedtls_ssl_conf_own_cert(&socket->config,
                                  socket->security.cert.client_cert,
                                  socket->security.cert.client_key);
    }

    mbedtls_ssl_conf_ciphersuites(&socket->config,
                                  socket->effective_ciphersuites);
    return AVS_OK;
}
#else // WITH_X509
#    define is_verification_enabled(...) 0
#    define initialize_cert_security(...) avs_errno(AVS_ENOTSUP)
#endif // WITH_X509

#ifdef WITH_PSK
static int *init_psk_ciphersuites(
        const avs_net_socket_tls_ciphersuites_t *enabled_ciphers) {
    const int *all_ciphers = mbedtls_ssl_list_ciphersuites();

    size_t ciphers_count = 0;
    for (const int *cipher = all_ciphers; cipher && *cipher; ++cipher) {
        const mbedtls_ssl_ciphersuite_t *info =
                mbedtls_ssl_ciphersuite_from_id(*cipher);
        if (mbedtls_ssl_ciphersuite_uses_psk(info)
                && contains_cipher(enabled_ciphers, *cipher)) {
            ++ciphers_count;
        }
    }

    int *psk_ciphers = (int *) avs_calloc(ciphers_count + 1, sizeof(int));
    if (!psk_ciphers) {
        LOG(ERROR, "out of memory");
        return NULL;
    }

    int *psk_cipher_it = psk_ciphers;
    for (const int *cipher = all_ciphers; cipher && *cipher; ++cipher) {
        const mbedtls_ssl_ciphersuite_t *info =
                mbedtls_ssl_ciphersuite_from_id(*cipher);
        if (mbedtls_ssl_ciphersuite_uses_psk(info)
                && contains_cipher(enabled_ciphers, *cipher)) {
            *psk_cipher_it++ = *cipher;
        }
    }

    return psk_ciphers;
}

static avs_error_t initialize_psk_security(ssl_socket_t *socket) {
    avs_free(socket->effective_ciphersuites);
    if (!(socket->effective_ciphersuites =
                  init_psk_ciphersuites(&socket->enabled_ciphersuites))) {
        return avs_errno(AVS_ENOMEM);
    }

    /* mbedtls_ssl_conf_psk() makes copies of the buffers */
    /* We set the values directly instead, to avoid that. */
    socket->config.psk = (unsigned char *) socket->security.psk.psk;
    socket->config.psk_len = socket->security.psk.psk_size;
    socket->config.psk_identity =
            (unsigned char *) socket->security.psk.identity;
    socket->config.psk_identity_len = socket->security.psk.identity_size;

    mbedtls_ssl_conf_ciphersuites(&socket->config,
                                  socket->effective_ciphersuites);
    return AVS_OK;
}
#else // WITH_PSK
#    define initialize_psk_security(...) \
        (LOG(ERROR, "PSK support disabled"), avs_errno(AVS_ENOTSUP))
#endif // WITH_PSK

static int transport_for_socket_type(avs_net_socket_type_t backend_type) {
    switch (backend_type) {
    case AVS_NET_TCP_SOCKET:
    case AVS_NET_SSL_SOCKET:
        return MBEDTLS_SSL_TRANSPORT_STREAM;
    case AVS_NET_UDP_SOCKET:
    case AVS_NET_DTLS_SOCKET:
        return MBEDTLS_SSL_TRANSPORT_DATAGRAM;
    default:
        AVS_UNREACHABLE("invalid enum value");
        return -1;
    }
}

static avs_error_t
configure_ssl(ssl_socket_t *socket,
              const avs_net_ssl_configuration_t *configuration) {
    mbedtls_ssl_config_init(&socket->config);
    /* HACK: The config is always initialized with MBEDTLS_SSL_IS_SERVER even
     * though it may be later reused in a client context. This is because the
     * default server-side config initializes pretty much everything that the
     * default client-side config does (aside from endpoint, authmode and
     * session_tickets, which are just flags that are trivial to set manually),
     * and more. So it's safer to initialize it with server-side defaults and
     * then repurpose as a client-side config rather than vice versa. Details:
     * https://github.com/ARMmbed/mbedtls/blob/mbedtls-2.6.1/library/ssl_tls.c#L7465
     */
    if (mbedtls_ssl_config_defaults(&socket->config, MBEDTLS_SSL_IS_SERVER,
                                    transport_for_socket_type(
                                            socket->backend_type),
                                    MBEDTLS_SSL_PRESET_DEFAULT)) {
        LOG(ERROR, "mbedtls_ssl_config_defaults() failed");
        return avs_errno(AVS_ENOTSUP);
    }

#ifdef WITH_MBEDTLS_LOGS
    // most verbose logs available
    mbedtls_debug_set_threshold(4);
    mbedtls_ssl_conf_dbg(&socket->config, debug_mbedtls, NULL);
#endif // WITH_MBEDTLS_LOGS

    if (set_min_ssl_version(&socket->config, configuration->version)) {
        LOG(ERROR, "Could not set minimum SSL version");
        return avs_errno(AVS_ENOTSUP);
    }

    mbedtls_ssl_conf_rng(&socket->config, mbedtls_ctr_drbg_random,
                         &AVS_SSL_GLOBAL.rng);

    const avs_net_dtls_handshake_timeouts_t *dtls_handshake_timeouts =
            configuration->dtls_handshake_timeouts
                    ? configuration->dtls_handshake_timeouts
                    : &DEFAULT_DTLS_HANDSHAKE_TIMEOUTS;
    int64_t min_ms, max_ms;
    if (avs_time_duration_to_scalar(&min_ms, AVS_TIME_MS,
                                    dtls_handshake_timeouts->min)
            || avs_time_duration_to_scalar(&max_ms, AVS_TIME_MS,
                                           dtls_handshake_timeouts->max)
            || min_ms < 0 || min_ms > UINT32_MAX || max_ms < 0
            || max_ms > UINT32_MAX) {
        LOG(ERROR, "Invalid DTLS handshake timeouts");
        return avs_errno(AVS_EINVAL);
    }
    mbedtls_ssl_conf_handshake_timeout(&socket->config, (uint32_t) min_ms,
                                       (uint32_t) max_ms);

    if (configuration->session_resumption_buffer_size > 0) {
        assert(configuration->session_resumption_buffer);
#ifdef WITH_TLS_SESSION_PERSISTENCE
        socket->session_resumption_buffer =
                configuration->session_resumption_buffer;
        socket->session_resumption_buffer_size =
                configuration->session_resumption_buffer_size;
#endif // WITH_TLS_SESSION_PERSISTENCE
    }

    if (configuration->server_name_indication) {
        size_t len = strlen(configuration->server_name_indication);
        if (len >= sizeof(socket->server_name_indication)) {
            LOG(ERROR, "SNI is too long (maximum allowed size is %u)",
                (unsigned) sizeof(socket->server_name_indication) - 1);
            return avs_errno(AVS_ERANGE);
        }
        memcpy(socket->server_name_indication,
               configuration->server_name_indication, len + 1);
    }
    socket->use_connection_id = configuration->use_connection_id;

#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
    if (socket->use_connection_id
            && transport_for_socket_type(socket->backend_type)
                           == MBEDTLS_SSL_TRANSPORT_DATAGRAM
            && mbedtls_ssl_conf_cid(&socket->config, 0,
                                    MBEDTLS_SSL_UNEXPECTED_CID_IGNORE)) {
        LOG(ERROR, "cannot configure CID");
        return avs_errno(AVS_ENOTSUP);
    }
#endif // MBEDTLS_SSL_DTLS_CONNECTION_ID

    if (configuration->additional_configuration_clb
            && configuration->additional_configuration_clb(&socket->config)) {
        LOG(ERROR, "Error while setting additional SSL configuration");
        return avs_errno(AVS_EPIPE);
    }

    return AVS_OK;
}

static avs_error_t update_ssl_endpoint_config(ssl_socket_t *socket) {
    avs_net_socket_opt_value_t state_opt;
    avs_error_t err =
            avs_net_socket_get_opt((avs_net_abstract_socket_t *) socket,
                                   AVS_NET_SOCKET_OPT_STATE, &state_opt);
    if (avs_is_err(err)) {
        LOG(ERROR, "initialize_ssl_config: could not get socket state");
        return err;
    }
    if (state_opt.state == AVS_NET_SOCKET_STATE_CONNECTED) {
        mbedtls_ssl_conf_endpoint(&socket->config, MBEDTLS_SSL_IS_CLIENT);
#ifdef MBEDTLS_SSL_SESSION_TICKETS
        mbedtls_ssl_conf_session_tickets(&socket->config,
                                         MBEDTLS_SSL_SESSION_TICKETS_ENABLED);
#endif // MBEDTLS_SSL_SESSION_TICKETS
    } else if (state_opt.state == AVS_NET_SOCKET_STATE_ACCEPTED) {
        mbedtls_ssl_conf_endpoint(&socket->config, MBEDTLS_SSL_IS_SERVER);
#ifdef MBEDTLS_SSL_SESSION_TICKETS
        mbedtls_ssl_conf_session_tickets(&socket->config,
                                         MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
#endif // MBEDTLS_SSL_SESSION_TICKETS
    } else {
        LOG(ERROR, "initialize_ssl_config: invalid socket state");
        return avs_errno(AVS_EINVAL);
    }

    return AVS_OK;
}

#ifdef WITH_TLS_SESSION_PERSISTENCE
static bool sessions_equal(const mbedtls_ssl_session *left,
                           const mbedtls_ssl_session *right) {
    if (!left && !right) {
        return true;
    }
    return left && right && left->ciphersuite == right->ciphersuite
           && left->compression == right->compression
#    ifdef MBEDTLS_HAVE_TIME
           && left->start == right->start
#    endif // MBEDTLS_HAVE_TIME
           && left->id_len == right->id_len
           && memcmp(left->id, right->id, left->id_len) == 0;
}
#else // WITH_TLS_SESSION_PERSISTENCE
#    define sessions_equal(left, right) false
#endif // WITH_TLS_SESSION_PERSISTENCE

static avs_error_t start_ssl(ssl_socket_t *socket, const char *host) {
    int result;
    avs_error_t err;
    if (avs_is_err((err = update_ssl_endpoint_config(socket)))) {
        LOG(ERROR, "could not initialize ssl context");
        return err;
    }
    assert(!socket->flags.context_valid);

    switch (socket->security_mode) {
    case AVS_NET_SECURITY_PSK:
        err = initialize_psk_security(socket);
        break;
    case AVS_NET_SECURITY_CERTIFICATE:
        err = initialize_cert_security(socket);
        break;
    default:
        AVS_UNREACHABLE("invalid enum value");
        err = avs_errno(AVS_EBADF);
    }
    if (avs_is_err(err)) {
        return err;
    }

    bool restore_session = false;
#ifdef WITH_TLS_SESSION_PERSISTENCE
    mbedtls_ssl_session restored_session;
    mbedtls_ssl_session_init(&restored_session);
#endif // WITH_TLS_SESSION_PERSISTENCE

    mbedtls_ssl_init(&socket->context);
    socket->flags.context_valid = true;

    mbedtls_ssl_set_bio(get_context(socket), socket, avs_bio_send, NULL,
                        avs_bio_recv);
    mbedtls_ssl_set_timer_cb(get_context(socket), &socket->timer,
                             mbedtls_timing_set_delay,
                             mbedtls_timing_get_delay);
    if ((result = mbedtls_ssl_setup(get_context(socket), &socket->config))) {
        LOG(ERROR, "mbedtls_ssl_setup() failed: %d", result);
        err = avs_errno(AVS_ENOMEM);
        goto finish;
    }
#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
    // This may seem a bit odd, but the CID draft says:
    //
    // > 3.  The "connection_id" Extension
    // > [...]
    // > A zero-length CID value indicates that the client is prepared to send
    // > with a CID but does not wish the server to use one when sending.
    // > [...]
    // > A server willing to use CIDs will respond with a "connection_id"
    // > extension in the ServerHello, containing the CID it wishes the client
    // > to use when sending messages towards it.
    if (socket->use_connection_id
            && transport_for_socket_type(socket->backend_type)
                           == MBEDTLS_SSL_TRANSPORT_DATAGRAM
            && mbedtls_ssl_set_cid(get_context(socket), MBEDTLS_SSL_CID_ENABLED,
                                   NULL, 0)) {
        LOG(ERROR, "cannot initialize CID to an empty value");
        err = avs_errno(AVS_EIO);
        goto finish;
    }
#endif // MBEDTLS_SSL_DTLS_CONNECTION_ID

#ifdef WITH_X509
    if ((result = mbedtls_ssl_set_hostname(
                 get_context(socket),
                 socket->server_name_indication[0]
                         ? socket->server_name_indication
                         : host))) {
        LOG(ERROR, "mbedtls_ssl_set_hostname() failed: %d", result);
        err = avs_errno(result == MBEDTLS_ERR_SSL_ALLOC_FAILED ? AVS_ENOMEM
                                                               : AVS_EINVAL);
        goto finish;
    }
#else
    (void) host;
#endif // WITH_X509

#ifdef WITH_TLS_SESSION_PERSISTENCE
    if (socket->session_resumption_buffer
            && socket->config.endpoint == MBEDTLS_SSL_IS_CLIENT) {
        if (avs_is_err(_avs_net_mbedtls_session_restore(
                    &restored_session, socket->session_resumption_buffer,
                    socket->session_resumption_buffer_size))) {
            LOG(WARNING,
                "Could not restore session; performing full handshake");
        } else if ((result = mbedtls_ssl_set_session(get_context(socket),
                                                     &restored_session))) {
            LOG(WARNING,
                "mbedtls_ssl_set_session() failed: %d; performing full "
                "handshake",
                result);
        } else {
            restore_session = true;
        }
    }
#endif // WITH_TLS_SESSION_PERSISTENCE

    socket->bio_error = AVS_OK;
    do {
        result = mbedtls_ssl_handshake(get_context(socket));
    } while (result == MBEDTLS_ERR_SSL_WANT_READ
             || result == MBEDTLS_ERR_SSL_WANT_WRITE);

    if (result == 0) {
#if defined(MBEDTLS_SSL_DTLS_CONNECTION_ID)
        if (socket->use_connection_id) {
            unsigned char peer_cid[MBEDTLS_SSL_CID_OUT_LEN_MAX];
            size_t peer_cid_len = 0;
            int enabled = 0;
            (void) mbedtls_ssl_get_peer_cid(get_context(socket), &enabled,
                                            peer_cid, &peer_cid_len);
            if (enabled) {
                char peer_cid_hex[2 * sizeof(peer_cid) + 1] = "";
                (void) avs_hexlify(peer_cid_hex, sizeof(peer_cid_hex), peer_cid,
                                   peer_cid_len);
                LOG(DEBUG, "negotiated CID = %s", peer_cid_hex);
            }
        }
#endif // MBEDTLS_SSL_DTLS_CONNECTION_ID
#ifdef WITH_TLS_SESSION_PERSISTENCE
        if (socket->session_resumption_buffer
                && socket->config.endpoint == MBEDTLS_SSL_IS_CLIENT) {
            // We rely on session renegotation being disabled in
            // configuration.
            _avs_net_mbedtls_session_save(
                    get_context(socket)->session,
                    socket->session_resumption_buffer,
                    socket->session_resumption_buffer_size);
        }
#endif // WITH_TLS_SESSION_PERSISTENCE
        if ((socket->flags.session_restored =
                     (restore_session
                      && sessions_equal(get_context(socket)->session,
                                        &restored_session)))) {
            LOG(TRACE, "handshake success: session restored");
        } else {
            LOG(TRACE, "handshake success: new session started");
        }
    } else {
        if (avs_is_err(socket->bio_error)) {
            err = socket->bio_error;
        } else {
            err = avs_errno(AVS_EPROTO);
        }
        update_alert_if_any(socket);
        LOG(ERROR, "handshake failed: %d", result);
    }

    if (avs_is_ok(err) && !socket->flags.session_restored
            && is_verification_enabled(socket)) {
        uint32_t verify_result =
                mbedtls_ssl_get_verify_result(get_context(socket));
        if (verify_result) {
            LOG(ERROR, "server certificate verification failure: %" PRIu32,
                verify_result);
            err = avs_errno(AVS_EPROTO);
        }
    }
finish:
#ifdef WITH_TLS_SESSION_PERSISTENCE
    mbedtls_ssl_session_free(&restored_session);
#endif // WITH_TLS_SESSION_PERSISTENCE
    if (avs_is_err(err)) {
        mbedtls_ssl_free(get_context(socket));
        socket->flags.context_valid = false;
        return err;
    } else {
        return AVS_OK;
    }
}

static avs_error_t send_ssl(avs_net_abstract_socket_t *socket_,
                            const void *buffer,
                            size_t buffer_length) {
    ssl_socket_t *socket = (ssl_socket_t *) socket_;
    size_t bytes_sent = 0;
    int result = 0;
    avs_error_t err = AVS_OK;
    LOG(TRACE, "send_ssl(socket=%p, buffer=%p, buffer_length=%lu)",
        (void *) socket, buffer, (unsigned long) buffer_length);

    while (bytes_sent < buffer_length) {
        do {
            socket->bio_error = AVS_OK;
            errno = 0;
            result = mbedtls_ssl_write(get_context(socket),
                                       ((const unsigned char *) buffer)
                                               + bytes_sent,
                                       (size_t) (buffer_length - bytes_sent));
        } while (result == MBEDTLS_ERR_SSL_WANT_WRITE
                 || result == MBEDTLS_ERR_SSL_WANT_READ);
        if (result <= 0) {
            break;
        }

        bytes_sent += (size_t) result;
    }

    if (result < 0) {
        if (avs_is_err(socket->bio_error)) {
            err = socket->bio_error;
        } else if (avs_is_ok((err = avs_errno(avs_map_errno(errno))))) {
            err = avs_errno(AVS_EPROTO);
        }
    }
    LOG(DEBUG, "ssl_write result %d", result);

    if (bytes_sent < buffer_length) {
        LOG(ERROR, "send failed (%lu/%lu): %d", (unsigned long) bytes_sent,
            (unsigned long) buffer_length, result);
        assert(avs_is_err(err));
        return err;
    }
    return AVS_OK;
}

static avs_error_t receive_ssl(avs_net_abstract_socket_t *socket_,
                               size_t *out_bytes_received,
                               void *buffer,
                               size_t buffer_length) {
    ssl_socket_t *socket = (ssl_socket_t *) socket_;
    memset(&socket->last_alert, 0, sizeof(socket->last_alert));
    int result = 0;

    LOG(TRACE, "receive_ssl(socket=%p, buffer=%p, buffer_length=%lu)",
        (void *) socket, buffer, (unsigned long) buffer_length);

    if (buffer_length > 0
            && transport_for_socket_type(socket->backend_type)
                           == MBEDTLS_SSL_TRANSPORT_DATAGRAM) {
        // mbed TLS treats datagram connections as if they are stream-based :(
        size_t unread_bytes_from_previous_datagram =
                mbedtls_ssl_get_bytes_avail(get_context(socket));
        while (unread_bytes_from_previous_datagram > 0) {
            if ((result = mbedtls_ssl_read(
                         get_context(socket), (unsigned char *) buffer,
                         AVS_MIN(buffer_length,
                                 unread_bytes_from_previous_datagram)))
                    < 0) {
                break;
            }
            assert((size_t) result <= unread_bytes_from_previous_datagram);
            unread_bytes_from_previous_datagram -= (size_t) result;
        }
    }

    if (result >= 0) {
        do {
            socket->bio_error = AVS_OK;
            errno = 0;
            result = mbedtls_ssl_read(get_context(socket),
                                      (unsigned char *) buffer,
                                      buffer_length);
        } while (result == MBEDTLS_ERR_SSL_WANT_READ
                 || result == MBEDTLS_ERR_SSL_WANT_WRITE);
    }

    if (result < 0) {
        update_alert_if_any(socket);
        *out_bytes_received = 0;
        if (result == MBEDTLS_ERR_SSL_TIMEOUT) {
            LOG(TRACE, "receive_ssl: timed out");
            return avs_errno(AVS_ETIMEDOUT);
        } else if (result != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            avs_error_t err;
            if (avs_is_err(socket->bio_error)) {
                err = socket->bio_error;
            } else if (avs_is_ok((err = avs_errno(avs_map_errno(errno))))) {
                err = avs_errno(AVS_EPROTO);
            }
            LOG(ERROR, "receive failed: %d", result);
            return err;
        }
    } else {
        *out_bytes_received = (size_t) result;
        if (transport_for_socket_type(socket->backend_type)
                        == MBEDTLS_SSL_TRANSPORT_DATAGRAM
                && mbedtls_ssl_get_bytes_avail(get_context(socket)) > 0) {
            LOG(WARNING, "receive_ssl: message truncated");
            return avs_errno(AVS_EMSGSIZE);
        }
    }
    return AVS_OK;
}

#ifdef WITH_X509
static void cleanup_security_cert(ssl_socket_certs_t *certs) {
    if (certs->ca_cert) {
        mbedtls_x509_crt_free(certs->ca_cert);
        avs_free(certs->ca_cert);
    }
    if (certs->client_cert) {
        mbedtls_x509_crt_free(certs->client_cert);
        avs_free(certs->client_cert);
    }
    if (certs->client_key) {
        mbedtls_pk_free(certs->client_key);
        avs_free(certs->client_key);
    }
}
#else // WITH_X509
#    define cleanup_security_cert(...) (void) 0
#endif // WITH_X509

#ifdef WITH_PSK
#    define cleanup_security_psk _avs_net_psk_cleanup
#else // WITH_PSK
#    define cleanup_security_psk(...) (void) 0
#endif // WITH_PSK

static avs_error_t cleanup_ssl(avs_net_abstract_socket_t **socket_) {
    ssl_socket_t **socket = (ssl_socket_t **) socket_;
    LOG(TRACE, "cleanup_ssl(*socket=%p)", (void *) *socket);

    avs_error_t err = close_ssl(*socket_);
    add_err(&err, avs_net_socket_cleanup(&(*socket)->backend_socket));

    switch ((*socket)->security_mode) {
    case AVS_NET_SECURITY_PSK:
        cleanup_security_psk(&(*socket)->security.psk);
        break;
    case AVS_NET_SECURITY_CERTIFICATE:
        cleanup_security_cert(&(*socket)->security.cert);
        break;
    }
    avs_free((*socket)->enabled_ciphersuites.ids);
    avs_free((*socket)->effective_ciphersuites);

#ifdef WITH_PSK
    /* Detach the uncopied PSK values */
    (*socket)->config.psk = NULL;
    (*socket)->config.psk_len = 0;
    (*socket)->config.psk_identity = NULL;
    (*socket)->config.psk_identity_len = 0;
#endif // WITH_PSK
    mbedtls_ssl_config_free(&(*socket)->config);

    avs_free(*socket);
    *socket = NULL;
    return AVS_OK;
}

#ifdef WITH_X509
static avs_error_t
configure_ssl_certs(ssl_socket_certs_t *certs,
                    const avs_net_certificate_info_t *cert_info) {
    LOG(TRACE, "configure_ssl_certs");

    if (cert_info->server_cert_validation) {
        avs_error_t err =
                _avs_net_mbedtls_load_ca_certs(&certs->ca_cert,
                                               &cert_info->trusted_certs);
        if (avs_is_err(err)) {
            LOG(ERROR, "could not load CA chain");
            return err;
        }
    } else {
        LOG(DEBUG, "Server authentication disabled");
    }

    if (cert_info->client_cert.desc.source != AVS_NET_DATA_SOURCE_EMPTY) {
        avs_error_t err;
        if (avs_is_err(
                    (err = _avs_net_mbedtls_load_client_cert(
                             &certs->client_cert, &cert_info->client_cert)))) {
            LOG(ERROR, "could not load client certificate");
            return err;
        }
        if (avs_is_err((err = _avs_net_mbedtls_load_client_key(
                                &certs->client_key, &cert_info->client_key)))) {
            LOG(ERROR, "could not load client private key");
            return err;
        }
    } else {
        LOG(TRACE, "client certificate not specified");
    }

    return AVS_OK;
}

#else // WITH_X509
#    define configure_ssl_certs(...) \
        (LOG(ERROR, "X.509 support disabled"), avs_errno(AVS_ENOTSUP))
#endif // WITH_X509

#ifdef WITH_PSK
static avs_error_t configure_ssl_psk(ssl_socket_t *socket,
                                     const avs_net_psk_info_t *psk) {
    LOG(TRACE, "configure_ssl_psk");
    return _avs_net_psk_copy(&socket->security.psk, psk);
}
#else // WITH_PSK
#    define configure_ssl_psk(...) \
        (LOG(ERROR, "PSK support disabled"), avs_errno(AVS_ENOTSUP))
#endif // WITH_PSK

static avs_error_t
initialize_ssl_socket(ssl_socket_t *socket,
                      avs_net_socket_type_t backend_type,
                      const avs_net_ssl_configuration_t *configuration) {
    avs_error_t err;
    *(const avs_net_socket_v_table_t **) (intptr_t) &socket->operations =
            &ssl_vtable;

    socket->backend_type = backend_type;
    socket->backend_configuration = configuration->backend_configuration;

    socket->security_mode = configuration->security.mode;
    switch (configuration->security.mode) {
    case AVS_NET_SECURITY_PSK:
        err = configure_ssl_psk(socket, &configuration->security.data.psk);
        break;
    case AVS_NET_SECURITY_CERTIFICATE:
        err = configure_ssl_certs(&socket->security.cert,
                                  &configuration->security.data.cert);
        break;
    default:
        AVS_UNREACHABLE("invalid enum value");
        err = avs_errno(AVS_EINVAL);
    }

    return avs_is_ok(err) ? configure_ssl(socket, configuration) : err;
}
