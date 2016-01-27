/*
 * AVSystem Commons Library
 *
 * Copyright (C) 2014-2016 AVSystem <http://www.avsystem.com/>
 *
 * This code is free and open source software licensed under the MIT License.
 * See the LICENSE file for details.
 */

#include <config.h>

#include <assert.h>
#include <inttypes.h>
#include <string.h>

#if !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 199901L)
#define inline
#endif

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net.h>
#include <mbedtls/ssl.h>

#include "net.h"

#ifdef HAVE_VISIBILITY
#pragma GCC visibility push(hidden)
#endif

typedef struct {
    const avs_net_socket_v_table_t * const operations;
    mbedtls_ssl_context context;
    mbedtls_ssl_config config;
    mbedtls_x509_crt *ca_cert;
    mbedtls_x509_crt *client_cert;
    mbedtls_pk_context *pk_key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context rng;
    avs_net_abstract_socket_t *tcp_socket;
    avs_net_ssl_version_t version;
    avs_ssl_additional_configuration_clb_t *additional_configuration_clb;
    avs_net_socket_configuration_t backend_configuration;
} ssl_socket_t;

static int connect_ssl(avs_net_abstract_socket_t *ssl_socket,
                       const char* host,
                       const char *port);
static int decorate_ssl(avs_net_abstract_socket_t *socket,
                        avs_net_abstract_socket_t *backend_socket);
static int configure_ssl(ssl_socket_t *socket,
                         const avs_net_ssl_configuration_t *configuration);
static int send_ssl(avs_net_abstract_socket_t *ssl_socket,
                    const void *buffer,
                    size_t buffer_length);
static int receive_ssl(avs_net_abstract_socket_t *ssl_socket,
                       size_t *out,
                       void *buffer,
                       size_t buffer_length);
static int shutdown_ssl(avs_net_abstract_socket_t *socket);
static int close_ssl(avs_net_abstract_socket_t *ssl_socket);
static int cleanup_ssl(avs_net_abstract_socket_t **ssl_socket);
static int system_socket_ssl(avs_net_abstract_socket_t *ssl_socket,
                             const void **out);
static int interface_name_ssl(avs_net_abstract_socket_t *ssl_socket,
                              avs_net_socket_interface_name_t *if_name);
static int remote_host_ssl(avs_net_abstract_socket_t *socket,
                           char *out_buffer, size_t ouf_buffer_size);
static int remote_port_ssl(avs_net_abstract_socket_t *socket,
                           char *out_buffer, size_t ouf_buffer_size);
static int local_port_ssl(avs_net_abstract_socket_t *socket,
                          char *out_buffer, size_t ouf_buffer_size);
static int get_opt_ssl(avs_net_abstract_socket_t *ssl_socket_,
                       avs_net_socket_opt_key_t option_key,
                       avs_net_socket_opt_value_t *out_option_value);
static int set_opt_ssl(avs_net_abstract_socket_t *net_socket,
                       avs_net_socket_opt_key_t option_key,
                       avs_net_socket_opt_value_t option_value);

static int unimplemented() {
    return -1;
}

static const avs_net_socket_v_table_t ssl_vtable = {
    connect_ssl,
    decorate_ssl,
    send_ssl,
    (avs_net_socket_send_to_t) unimplemented,
    receive_ssl,
    (avs_net_socket_receive_from_t) unimplemented,
    (avs_net_socket_bind_t) unimplemented,
    (avs_net_socket_accept_t) unimplemented,
    close_ssl,
    shutdown_ssl,
    cleanup_ssl,
    system_socket_ssl,
    interface_name_ssl,
    remote_host_ssl,
    remote_port_ssl,
    local_port_ssl,
    get_opt_ssl,
    set_opt_ssl
};

static int avs_bio_recv(void *ctx, unsigned char *buf, size_t len,
                        uint32_t timeout_ms) {
    ssl_socket_t *socket = (ssl_socket_t *) ctx;
    avs_net_socket_opt_value_t orig_timeout;
    avs_net_socket_opt_value_t new_timeout;
    size_t read_bytes;
    int result;
    avs_net_socket_get_opt(socket->tcp_socket,
                           AVS_NET_SOCKET_OPT_RECV_TIMEOUT, &orig_timeout);
    new_timeout = orig_timeout;
    if (timeout_ms) {
        new_timeout.recv_timeout = (int) timeout_ms;
    }
    avs_net_socket_set_opt(socket->tcp_socket, AVS_NET_SOCKET_OPT_RECV_TIMEOUT,
                           new_timeout);
    if (avs_net_socket_receive(socket->tcp_socket, &read_bytes, buf, len)) {
        result = MBEDTLS_ERR_NET_RECV_FAILED;
    } else {
        result = (int) read_bytes;
    }
    avs_net_socket_set_opt(socket->tcp_socket, AVS_NET_SOCKET_OPT_RECV_TIMEOUT,
                           orig_timeout);
    return result;
}

static int avs_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    if (avs_net_socket_send(((ssl_socket_t *) ctx)->tcp_socket, buf, len)) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    } else {
        return (int) len;
    }
}

static int interface_name_ssl(avs_net_abstract_socket_t *ssl_socket_,
                              avs_net_socket_interface_name_t *if_name) {
    ssl_socket_t *ssl_socket = (ssl_socket_t *) ssl_socket_;
    if (ssl_socket->tcp_socket) {
        return avs_net_socket_interface_name(
                        (avs_net_abstract_socket_t *) ssl_socket->tcp_socket,
                        if_name);
    } else {
        return -1;
    }
}

static int remote_host_ssl(avs_net_abstract_socket_t *socket_,
                           char *out_buffer, size_t out_buffer_size) {
    ssl_socket_t *socket = (ssl_socket_t *) socket_;
    if (!socket->tcp_socket) {
        return -1;
    }
    return avs_net_socket_get_remote_host(socket->tcp_socket,
                                          out_buffer, out_buffer_size);
}

static int remote_port_ssl(avs_net_abstract_socket_t *socket_,
                           char *out_buffer, size_t out_buffer_size) {
    ssl_socket_t *socket = (ssl_socket_t *) socket_;
    if (!socket->tcp_socket) {
        return -1;
    }
    return avs_net_socket_get_remote_port(socket->tcp_socket,
                                          out_buffer, out_buffer_size);
}

static int local_port_ssl(avs_net_abstract_socket_t *socket_,
                          char *out_buffer, size_t out_buffer_size) {
    ssl_socket_t *socket = (ssl_socket_t *) socket_;
    if (!socket->tcp_socket) {
        return -1;
    }
    return avs_net_socket_get_local_port(socket->tcp_socket,
                                         out_buffer, out_buffer_size);
}

static int get_opt_ssl(avs_net_abstract_socket_t *ssl_socket_,
                       avs_net_socket_opt_key_t option_key,
                       avs_net_socket_opt_value_t *out_option_value) {
    ssl_socket_t *ssl_socket = (ssl_socket_t *) ssl_socket_;
    return avs_net_socket_get_opt(ssl_socket->tcp_socket, option_key,
                                  out_option_value);
}

static int set_opt_ssl(avs_net_abstract_socket_t *ssl_socket_,
                       avs_net_socket_opt_key_t option_key,
                       avs_net_socket_opt_value_t option_value) {
    ssl_socket_t *ssl_socket = (ssl_socket_t *) ssl_socket_;
    return avs_net_socket_set_opt(ssl_socket->tcp_socket, option_key,
                                  option_value);
}

static int system_socket_ssl(avs_net_abstract_socket_t *socket_,
                             const void **out) {
    ssl_socket_t *socket = (ssl_socket_t *) socket_;
    if (socket->tcp_socket) {
        *out = avs_net_socket_get_system(socket->tcp_socket);
    } else {
        *out = NULL;
    }
    return *out ? 0 : -1;
}

static int set_min_ssl_version(mbedtls_ssl_config *config,
                               avs_net_ssl_version_t version) {
    switch (version) {
    case AVS_NET_SSL_VERSION_SSLv2_OR_3:
    case AVS_NET_SSL_VERSION_SSLv3:
        mbedtls_ssl_conf_min_version(config,
                                     MBEDTLS_SSL_MAJOR_VERSION_3,
                                     MBEDTLS_SSL_MINOR_VERSION_0);
        return 0;
    case AVS_NET_SSL_VERSION_TLSv1:
        mbedtls_ssl_conf_min_version(config,
                                     MBEDTLS_SSL_MAJOR_VERSION_3,
                                     MBEDTLS_SSL_MINOR_VERSION_1);
        return 0;
    case AVS_NET_SSL_VERSION_TLSv1_1:
        mbedtls_ssl_conf_min_version(config,
                                     MBEDTLS_SSL_MAJOR_VERSION_3,
                                     MBEDTLS_SSL_MINOR_VERSION_2);
        return 0;
    case AVS_NET_SSL_VERSION_TLSv1_2:
        mbedtls_ssl_conf_min_version(config,
                                     MBEDTLS_SSL_MAJOR_VERSION_3,
                                     MBEDTLS_SSL_MINOR_VERSION_3);
        return 0;
    default:
        LOG(ERROR, "Unsupported SSL version");
        return -1;
    }
}

static uint8_t is_verification_enabled(ssl_socket_t *socket) {
    return socket->ca_cert != NULL;
}

static int initialize_ssl_config(ssl_socket_t *socket) {
    avs_net_socket_opt_value_t state_opt;
    int endpoint;
    if (avs_net_socket_get_opt(socket->tcp_socket,
                               AVS_NET_SOCKET_OPT_STATE, &state_opt)) {
        LOG(ERROR, "initialize_ssl_config: could not get socket state");
        return -1;
    }
    if (state_opt.state == AVS_NET_SOCKET_STATE_CONSUMING) {
        endpoint = MBEDTLS_SSL_IS_CLIENT;
    } else if (state_opt.state == AVS_NET_SOCKET_STATE_SERVING) {
        endpoint = MBEDTLS_SSL_IS_SERVER;
    } else {
        LOG(ERROR, "initialize_ssl_config: invalid socket state");
        return -1;
    }

    mbedtls_ssl_config_init(&socket->config);
    if (mbedtls_ssl_config_defaults(&socket->config, endpoint,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT)) {
        LOG(ERROR, "mbedtls_ssl_config_defaults() failed");
        return -1;
    }

    if (set_min_ssl_version(&socket->config, socket->version)) {
        LOG(ERROR, "Could not set max SSL version");
        return -1;
    }

    mbedtls_ssl_conf_rng(&socket->config,
                         mbedtls_ctr_drbg_random, &socket->rng);

    if (socket->ca_cert) {
        mbedtls_ssl_conf_authmode(&socket->config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&socket->config, socket->ca_cert, NULL);
    } else {
        mbedtls_ssl_conf_authmode(&socket->config, MBEDTLS_SSL_VERIFY_NONE);
    }

    if (socket->client_cert && socket->pk_key) {
        mbedtls_ssl_conf_own_cert(&socket->config,
                                  socket->client_cert, socket->pk_key);
    }

    if (socket->additional_configuration_clb
            && socket->additional_configuration_clb(&socket->config)) {
        LOG(ERROR, "Error while setting additional SSL configuration");
        return -1;
    }

    return 0;
}

static int start_ssl(ssl_socket_t *socket, const char *host) {
    int result;
    mbedtls_entropy_init(&socket->entropy);

    mbedtls_ctr_drbg_init(&socket->rng);
    if ((result = mbedtls_ctr_drbg_seed(&socket->rng, mbedtls_entropy_func,
                                        &socket->entropy, NULL, 0))) {
        LOG(ERROR, "mbedtls_ctr_drbg_seed() failed: %d", result);
        return -1;
    }

    if (initialize_ssl_config(socket)) {
        LOG(ERROR, "could not initialize ssl context");
        return -1;
    }

    mbedtls_ssl_init(&socket->context);
    mbedtls_ssl_set_bio(&socket->context, socket,
                        avs_bio_send, NULL, avs_bio_recv);
    if ((result = mbedtls_ssl_setup(&socket->context, &socket->config))) {
        LOG(ERROR, "mbedtls_ssl_setup() failed: %d", result);
        return -1;
    }

    if ((result = mbedtls_ssl_set_hostname(&socket->context, host))) {
        LOG(ERROR, "mbedtls_ssl_set_hostname() failed: %d", result);
    }

    for (;;) {
        int result = mbedtls_ssl_handshake(&socket->context);
        if (result == 0) {
            LOG(TRACE, "handshake success");
            break;
        } else if (result != MBEDTLS_ERR_SSL_WANT_READ
                && result != MBEDTLS_ERR_SSL_WANT_WRITE) {
            LOG(ERROR, "handshake failed: %d", result);
            break;
        }
    }

    if (is_verification_enabled(socket)) {
        uint32_t verify_result =
                mbedtls_ssl_get_verify_result(&socket->context);
        if (verify_result) {
            LOG(ERROR, "server certificate verification failure: %" PRIu32,
                verify_result);
            result = -1;
        }
    }
    return result ? -1 : 0;
}

static int connect_ssl(avs_net_abstract_socket_t *socket_,
                       const char *host,
                       const char *port) {
    int result;
    ssl_socket_t *socket = (ssl_socket_t *) socket_;
    LOG(TRACE, "connect_ssl(socket=%p, host=%s, port=%s)",
        (void *) socket, host, port);

    if (avs_net_socket_create(&socket->tcp_socket, AVS_NET_TCP_SOCKET,
                              &socket->backend_configuration)) {
        return -1;
    }
    if (avs_net_socket_connect(socket->tcp_socket, host, port)) {
        LOG(ERROR, "cannot establish TCP connection");
        return -1;
    }

    result = start_ssl(socket, host);
    if (result) {
        avs_net_socket_cleanup(&socket->tcp_socket);
    }
    return result;
}

#define CREATE_OR_FAIL(type, ptr) \
do {\
    free(*ptr);\
    *ptr = (type *) calloc(1, sizeof(**ptr));\
    if (!*ptr) {\
        LOG(ERROR, "memory allocation error");\
        return -1;\
    }\
} while (0)

static int load_ca_certs(mbedtls_x509_crt **out,
                         const char *ca_cert_path,
                         const char *ca_cert_file,
                         const avs_net_ssl_raw_cert_t *ca_cert) {
    const int has_raw_cert = ca_cert && ca_cert->cert_der;

    if (!ca_cert_path && !ca_cert_file && !has_raw_cert) {
        LOG(ERROR, "no certificate for CA provided");
        return -1;
    }

    CREATE_OR_FAIL(mbedtls_x509_crt, out);

    if (ca_cert_path) {
        int failed = mbedtls_x509_crt_parse_path(*out, ca_cert_path);
        if (failed) {
            LOG(WARNING,
                "failed to parse %d certs in path <%s>", failed, ca_cert_path);
        }
    }
    if (ca_cert_file) {
        int failed = mbedtls_x509_crt_parse_file(*out, ca_cert_file);
        if (failed) {
            LOG(WARNING,
                "failed to parse %d certs in file <%s>", failed, ca_cert_file);
        }
    }
    if (has_raw_cert) {
        int failed = mbedtls_x509_crt_parse_der(
                *out,
                (const unsigned char *) ca_cert->cert_der, ca_cert->cert_size);
        if (failed) {
            LOG(WARNING, "failed to parse DER certificate: %d", failed);
        }
    }
    return 0;
}

static int is_private_key_valid(const avs_net_private_key_t *key) {
    assert(key);

    switch (key->source) {
    case AVS_NET_DATA_SOURCE_FILE:
        if (!key->data.file.path || !key->data.file.password) {
            LOG(ERROR, "private key with password not specified");
            return 0;
        }
        return 1;
    case AVS_NET_DATA_SOURCE_BUFFER:
        if (!key->data.buffer.private_key) {
            LOG(ERROR, "private key not specified");
            return 0;
        }
        return 1;
    }
    assert(!"invalid enum value");
    return 0;
}

static int load_client_private_key(mbedtls_pk_context **pk_key,
                                   const avs_net_private_key_t *key) {
    if (!is_private_key_valid(key)) {
        return -1;
    }

    CREATE_OR_FAIL(mbedtls_pk_context, pk_key);

    switch (key->source) {
    case AVS_NET_DATA_SOURCE_FILE:
        return mbedtls_pk_parse_keyfile(*pk_key, key->data.file.path,
                                        key->data.file.password);
    case AVS_NET_DATA_SOURCE_BUFFER:
#warning "TODO: FIXME"
    default:
        assert(!"invalid enum value");
        return -1;
    }
}

static int is_client_cert_empty(const avs_net_client_cert_t *cert) {
    switch (cert->source) {
    case AVS_NET_DATA_SOURCE_FILE:
        return !cert->data.file;
    case AVS_NET_DATA_SOURCE_BUFFER:
        return !cert->data.buffer.cert_der;
    }
    assert(!"invalid enum value");
    return 1;
}

static int load_client_cert(mbedtls_x509_crt **client_cert,
                            mbedtls_pk_context **pk_key,
                            const avs_net_client_cert_t *cert,
                            const avs_net_private_key_t *key) {
    int failed;

    if (is_client_cert_empty(cert)) {
        LOG(TRACE, "client certificate not specified");
        return 0;
    }

    CREATE_OR_FAIL(mbedtls_x509_crt, client_cert);

    switch (cert->source) {
    case AVS_NET_DATA_SOURCE_FILE:
        failed = mbedtls_x509_crt_parse_file(*client_cert, cert->data.file);
        if (failed) {
            LOG(WARNING, "failed to parse %d certs in file <%s>",
                failed, cert->data.file);
        }
        break;
    case AVS_NET_DATA_SOURCE_BUFFER:
        failed = mbedtls_x509_crt_parse_der(
                *client_cert,
                (const unsigned char *) cert->data.buffer.cert_der,
                cert->data.buffer.cert_size);
        if (failed) {
            LOG(WARNING, "failed to parse DER certificate: %d", failed);
        }
    default:
        assert(!"invalid enum value");
        return -1;
    }

    if (load_client_private_key(pk_key, key)) {
        LOG(ERROR, "Error loading client private key");
        return -1;
    }

    return 0;
}

    static int server_auth_enabled(const avs_net_certificate_info_t *cert_info) {
    return cert_info->ca_cert_file
        || cert_info->ca_cert_path
        || cert_info->ca_cert_raw.cert_der;
}

static int configure_ssl_certs(ssl_socket_t *socket,
                               const avs_net_certificate_info_t *cert_info) {
    LOG(TRACE, "configure_ssl_certs");

    if (server_auth_enabled(cert_info)) {
        if (load_ca_certs(&socket->ca_cert,
                          cert_info->ca_cert_path,
                          cert_info->ca_cert_file,
                          &cert_info->ca_cert_raw)) {
            LOG(ERROR, "error loading CA certs");
            return -1;
        }
    } else {
        LOG(DEBUG, "Server authentication disabled");
    }

    if (load_client_cert(&socket->client_cert,
                         &socket->pk_key,
                         &cert_info->client_cert,
                         &cert_info->client_key)) {
        LOG(ERROR, "error loading client certificate");
        return -1;
    }

    return 0;
}

static int configure_ssl(ssl_socket_t *socket,
                         const avs_net_ssl_configuration_t *configuration) {
    LOG(TRACE, "configure_ssl(socket=%p, configuration=%p)",
              (void *) socket, (const void *) configuration);

    if (!configuration) {
        LOG(WARNING, "configuration not provided");
        return 0;
    }

    socket->version = configuration->version;
    socket->additional_configuration_clb =
            configuration->additional_configuration_clb;
    socket->backend_configuration = configuration->backend_configuration;

    switch (configuration->security.mode) {
    case AVS_NET_SECURITY_PSK:
#warning "TODO"
        assert(!"PSK not supported for now");
        return -1;
    case AVS_NET_SECURITY_CERTIFICATE:
        if (configure_ssl_certs(socket, &configuration->security.data.cert)) {
            return -1;
        }
        break;
    default:
        assert(!"invalid enum value");
        return -1;
    }

    return 0;
}

static int initialize_ssl_socket(ssl_socket_t *socket,
                                 const avs_net_ssl_configuration_t *configuration) {
    memset(socket, 0, sizeof (ssl_socket_t));
    *(const avs_net_socket_v_table_t **) (intptr_t) &socket->operations =
            &ssl_vtable;

    return configure_ssl(socket, configuration);
}

int _avs_net_create_ssl_socket(avs_net_abstract_socket_t **socket,
                               const void *socket_configuration) {
    LOG(TRACE, "create_ssl_socket(socket=%p)", (void *) socket);

    *socket = (avs_net_abstract_socket_t *) malloc(sizeof (ssl_socket_t));
    if (*socket) {
        if (initialize_ssl_socket((ssl_socket_t *) * socket,
                                  (const avs_net_ssl_configuration_t *)
                                  socket_configuration)) {
            LOG(ERROR, "socket initialization error");
            avs_net_socket_cleanup(socket);
            return -1;
        } else {
            return 0;
        }
    } else {
        LOG(ERROR, "memory allocation error");
        return -1;
    }
}

int _avs_net_create_dtls_socket(avs_net_abstract_socket_t **socket,
                                const void *socket_configuration) {
}
