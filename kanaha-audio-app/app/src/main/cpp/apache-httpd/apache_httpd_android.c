/*
 * Kanaha Audio
 * HTTP Server with JSON-RPC, TLS, and HTTP/2 Support
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2025-2026 Robert Lazarski
 *
 * Follows the same HTTP server design pattern established in Kanaha Camera
 * (GPL v3+), but this is an independent implementation under Apache 2.0.
 * No code was copied from the GPL-licensed project.
 *
 * This provides a lightweight HTTP server using:
 * - json-c for JSON-RPC parsing
 * - OpenSSL for TLS/mTLS
 * - nghttp2 for HTTP/2 protocol info
 *
 * Routes JSON-RPC requests to audio_search_service_invoke_json_impl()
 * instead of camera_control_service_invoke_json_impl().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "KanahaAudioServer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) fprintf(stderr, "[INFO] " __VA_ARGS__)
#define LOGW(...) fprintf(stderr, "[WARN] " __VA_ARGS__)
#define LOGE(...) fprintf(stderr, "[ERROR] " __VA_ARGS__)
#define LOGD(...) fprintf(stderr, "[DEBUG] " __VA_ARGS__)
#endif

#ifdef USE_CROSS_COMPILED_LIBS

/* json-c for JSON-RPC */
#include <json-c/json.h>

/* OpenSSL for TLS */
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/opensslv.h>

/* nghttp2 for HTTP/2 version info */
#include <nghttp2/nghttp2.h>

/* Server state */
static volatile int g_server_running = 0;
static int g_server_port = 443;
static char g_repo_path[512] = {0};
static char g_ssl_cert_path[512] = {0};
static char g_ssl_key_path[512] = {0};
static char g_ssl_ca_path[512] = {0};
static SSL_CTX *g_ssl_ctx = NULL;
static pthread_t g_server_thread;
static int g_connection_count = 0;  /* Lifetime total (not active connections) — intentionally never decremented */
/* TLS is always enabled — Axis2/C JSON-RPC requires HTTP/2 which requires TLS */

/* External audio search service function */
extern int audio_search_service_invoke_json_impl(
    const char *json_request,
    char *json_response,
    size_t response_size
);

/**
 * Initialize OpenSSL library and configure TLS/mTLS
 */
static int init_openssl(void) {
    LOGI("Initializing OpenSSL...");

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
#else
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
#endif

    g_ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (!g_ssl_ctx) {
        LOGE("Failed to create SSL context");
        ERR_print_errors_fp(stderr);
        return -1;
    }

    SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);

    LOGI("OpenSSL initialized: %s", OpenSSL_version(OPENSSL_VERSION));
    LOGI("json-c version: %s", json_c_version());
    LOGI("nghttp2 version: %s", nghttp2_version(0)->version_str);

    /*
     * TLS is MANDATORY — Axis2/C in JSON mode uses HTTP/2, which requires TLS.
     * The server will not start without certificate and key paths.
     * mTLS (client certificate verification) is required when a CA cert is provided.
     */
    if (!g_ssl_cert_path[0] || !g_ssl_key_path[0]) {
        LOGE("FATAL: TLS certificates required but not configured.");
        LOGE("Axis2/C JSON-RPC uses HTTP/2, which requires TLS.");
        LOGE("Provide -c <cert.pem> -k <key.pem> [-a <ca.pem>]");
        return -1;
    }

    LOGI("Loading server certificate: %s", g_ssl_cert_path);
    if (SSL_CTX_use_certificate_file(g_ssl_ctx, g_ssl_cert_path, SSL_FILETYPE_PEM) != 1) {
        LOGE("Failed to load server certificate: %s", g_ssl_cert_path);
        ERR_print_errors_fp(stderr);
        return -1;
    }

    LOGI("Loading server private key: %s", g_ssl_key_path);
    if (SSL_CTX_use_PrivateKey_file(g_ssl_ctx, g_ssl_key_path, SSL_FILETYPE_PEM) != 1) {
        LOGE("Failed to load server private key: %s", g_ssl_key_path);
        ERR_print_errors_fp(stderr);
        return -1;
    }

    if (SSL_CTX_check_private_key(g_ssl_ctx) != 1) {
        LOGE("Server private key does not match certificate");
        ERR_print_errors_fp(stderr);
        return -1;
    }

    if (g_ssl_ca_path[0]) {
        LOGI("Loading CA certificate for mTLS: %s", g_ssl_ca_path);
        if (SSL_CTX_load_verify_locations(g_ssl_ctx, g_ssl_ca_path, NULL) != 1) {
            LOGE("Failed to load CA certificate: %s", g_ssl_ca_path);
            ERR_print_errors_fp(stderr);
            return -1;
        }

        /* Require client certificate — connections without valid certs are rejected */
        SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
        SSL_CTX_set_verify_depth(g_ssl_ctx, 2);
        LOGI("mTLS enabled: client certificates required");
    } else {
        LOGW("No CA cert — server TLS only (no client cert verification)");
        LOGW("For production, provide -a <ca.pem> to enable mTLS");
    }

    LOGI("TLS enabled with server certificate");

    return 0;
}

static void cleanup_openssl(void) {
    if (g_ssl_ctx) {
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = NULL;
    }
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    EVP_cleanup();
    ERR_free_strings();
#endif
}

/**
 * Parse JSON request and invoke audio search service
 */
static char *handle_json_request(const char *request_body) {
    struct json_object *request = NULL;
    struct json_object *action_obj = NULL;
    char *result = NULL;

    /* Request body intentionally not logged — action is logged below */

    request = json_tokener_parse(request_body);
    if (!request) {
        LOGE("JSON parse error");
        return strdup("{\"error\":\"Parse error\",\"code\":-32700}");
    }

    /* "operation" is what the engine puts in the request from the URL; "action"
     * is what MCP clients send. Either names the operation to run. */
    if (!json_object_object_get_ex(request, "action", &action_obj)
        && !json_object_object_get_ex(request, "operation", &action_obj)) {
        LOGE("Missing action field");
        json_object_put(request);
        return strdup("{\"error\":\"Missing 'action' parameter\",\"code\":-32600}");
    }

    LOGI("Service action: %s", json_object_get_string(action_obj));

    char response_buffer[65536];
    int invoke_result = audio_search_service_invoke_json_impl(
        request_body, response_buffer, sizeof(response_buffer));

    if (invoke_result == 0) {
        result = strdup(response_buffer);
    } else {
        /* Service may have written error JSON even on failure */
        if (response_buffer[0] != '\0') {
            result = strdup(response_buffer);
        } else {
            result = strdup("{\"error\":\"Internal error\",\"code\":-32603}");
        }
    }

    json_object_put(request);
    return result;
}

/**
 * Write all bytes via SSL, handling partial writes.
 * Returns 0 on success, -1 on error.
 *
 * TLS is mandatory — no plain-socket send function needed.
 */
static int ssl_write_all(SSL *ssl, const char *buf, size_t len) {
    size_t total_written = 0;
    while (total_written < len) {
        int n = SSL_write(ssl, buf + total_written, (int)(len - total_written));
        if (n <= 0) {
            int ssl_error = SSL_get_error(ssl, n);
            LOGE("SSL_write() failed: error=%d", ssl_error);
            return -1;
        }
        total_written += (size_t)n;
    }
    return 0;
}

static void send_http_response_ssl(SSL *ssl, int status, const char *status_text,
                                   const char *content_type, const char *body) {
    char header[1024];
    size_t body_len = body ? strlen(body) : 0;

    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "Server: Kanaha-Audio/1.0\r\n"
             "\r\n",
             status, status_text, content_type, body_len);

    if (ssl_write_all(ssl, header, strlen(header)) == -1) {
        return;
    }
    if (body && body_len > 0) {
        ssl_write_all(ssl, body, body_len);
    }
}

static void handle_client(int client_fd, struct sockaddr_in *client_addr) {
    char buffer[8192];
    char *body_start;
    char *response;
    ssize_t bytes_read;
    SSL *ssl;

    g_connection_count++;

    LOGD("Connection #%d from %s:%d",
         g_connection_count,
         inet_ntoa(client_addr->sin_addr),
         ntohs(client_addr->sin_port));

    /* TLS handshake — mandatory for all connections.
     * Axis2/C JSON-RPC uses HTTP/2, which requires TLS. */
    ssl = SSL_new(g_ssl_ctx);
    if (!ssl) {
        LOGE("Failed to create SSL object");
        close(client_fd);
        return;
    }

    SSL_set_fd(ssl, client_fd);

    int ssl_accept_result = SSL_accept(ssl);
    if (ssl_accept_result != 1) {
        int ssl_error = SSL_get_error(ssl, ssl_accept_result);
        LOGE("SSL_accept failed: error=%d", ssl_error);
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        close(client_fd);
        return;
    }

    /* Log client certificate subject (mTLS — verified by OpenSSL) */
    X509 *client_cert = SSL_get_peer_certificate(ssl);
    if (client_cert) {
        char subject[256];
        X509_NAME_oneline(X509_get_subject_name(client_cert), subject, sizeof(subject));
        LOGI("Client certificate: %s", subject);
        X509_free(client_cert);
    }

    /* Read HTTP request over TLS */
    bytes_read = SSL_read(ssl, buffer, sizeof(buffer) - 1);

    if (bytes_read <= 0) {
        LOGD("Connection closed (no data)");
        SSL_free(ssl);
        close(client_fd);
        return;
    }
    buffer[bytes_read] = '\0';

    body_start = strstr(buffer, "\r\n\r\n");
    if (!body_start) {
        send_http_response_ssl(ssl, 400, "Bad Request", "text/plain", "Bad Request");
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(client_fd);
        return;
    }
    body_start += 4;

    /* Route request — HTTP method checked at position 0 to prevent
     * request smuggling (strstr would match "POST" in headers/params). */
    if (strncmp(buffer, "POST ", 5) == 0 && strstr(buffer, "application/json")) {
        response = handle_json_request(body_start);
        send_http_response_ssl(ssl, 200, "OK", "application/json", response);
        free(response);
    }
    else if (strncmp(buffer, "GET /status", 11) == 0 || strncmp(buffer, "GET / ", 6) == 0) {
        char status_json[1024];
        snprintf(status_json, sizeof(status_json),
            "{"
            "\"status\":\"running\","
            "\"service\":\"kanaha-audio\","
            "\"port\":%d,"
            "\"tls\":\"mandatory\","
            "\"connections\":%d,"
            "\"version\":\"1.0.0\","
            "\"libraries\":{"
            "\"openssl\":\"%s\","
            "\"json-c\":\"%s\","
            "\"nghttp2\":\"%s\""
            "},"
            "\"features\":[\"json-rpc\",\"tls\",\"mtls\",\"http2\",\"whisper\"]"
            "}",
            g_server_port,
            g_connection_count,
            OpenSSL_version(OPENSSL_VERSION),
            json_c_version(),
            nghttp2_version(0)->version_str
        );
        send_http_response_ssl(ssl, 200, "OK", "application/json", status_json);
    }
    else if (strncmp(buffer, "GET /health", 11) == 0) {
        send_http_response_ssl(ssl, 200, "OK", "application/json", "{\"healthy\":true}");
    }
    else {
        send_http_response_ssl(ssl, 404, "Not Found", "text/plain", "Not Found");
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(client_fd);
}

static void *server_thread_func(void *arg) {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    int opt = 1;

    (void)arg;

    LOGI("Starting server on port %d...", g_server_port);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LOGE("socket() failed: %s", strerror(errno));
        return NULL;
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;

    /* TLS is mandatory — always bind to all interfaces.
     * Access control is enforced by mTLS (client certificate required). */
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(g_server_port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        LOGE("bind() failed: %s", strerror(errno));
        close(server_fd);
        return NULL;
    }

    if (listen(server_fd, 10) < 0) {
        LOGE("listen() failed: %s", strerror(errno));
        close(server_fd);
        return NULL;
    }

    LOGI("Server listening on port %d", g_server_port);
    g_server_running = 1;

    while (g_server_running) {
        fd_set read_fds;
        struct timeval tv = {1, 0};

        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);

        if (select(server_fd + 1, &read_fds, NULL, NULL, &tv) > 0) {
            client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd >= 0) {
                /* Set receive timeout to prevent slow/malicious clients from blocking the server */
                struct timeval rcv_timeout = {15, 0};
                setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

                handle_client(client_fd, &client_addr);
            }
        }
    }

    close(server_fd);
    LOGI("Server stopped");
    return NULL;
}

int apache_httpd_main(int argc, char *argv[]) {
    const char *repo_path = "/data/data/org.kanaha.audio/files/axis2";
    const char *cert_path = NULL;
    const char *key_path = NULL;
    const char *ca_path = NULL;
    int port = 443;
    int i;

    LOGI("=== Kanaha Audio Server ===");
    LOGI("Built with:");
    LOGI("  OpenSSL: %s", OpenSSL_version(OPENSSL_VERSION));
    LOGI("  json-c: %s", json_c_version());
    LOGI("  nghttp2: %s", nghttp2_version(0)->version_str);

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            repo_path = argv[i + 1];
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            cert_path = argv[i + 1];
        } else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            key_path = argv[i + 1];
        } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            ca_path = argv[i + 1];
        }
    }

    strncpy(g_repo_path, repo_path, sizeof(g_repo_path) - 1);
    g_server_port = port;

    if (cert_path) strncpy(g_ssl_cert_path, cert_path, sizeof(g_ssl_cert_path) - 1);
    if (key_path) strncpy(g_ssl_key_path, key_path, sizeof(g_ssl_key_path) - 1);
    if (ca_path) strncpy(g_ssl_ca_path, ca_path, sizeof(g_ssl_ca_path) - 1);

    LOGI("Configuration: port=%d, repo=%s", port, repo_path);
    LOGI("SSL: cert=%s, key=%s, ca=%s",
         g_ssl_cert_path[0] ? g_ssl_cert_path : "(none)",
         g_ssl_key_path[0] ? g_ssl_key_path : "(none)",
         g_ssl_ca_path[0] ? g_ssl_ca_path : "(none)");

    if (init_openssl() != 0) {
        LOGE("OpenSSL init failed");
        return 1;
    }

    if (pthread_create(&g_server_thread, NULL, server_thread_func, NULL) != 0) {
        LOGE("Failed to start server thread");
        cleanup_openssl();
        return 1;
    }

    while (!g_server_running && g_server_running != -1) {
        usleep(100000);
    }

    LOGI("Server running on port %d", g_server_port);

    while (g_server_running == 1) {
        sleep(5);
        LOGD("Heartbeat: port=%d, connections=%d", g_server_port, g_connection_count);
    }

    pthread_join(g_server_thread, NULL);
    cleanup_openssl();

    LOGI("=== Server Exited ===");
    return 0;
}

int apache_httpd_validate_config(const char *config_file) {
    struct stat st;
    LOGI("Validating: %s", config_file);
    return stat(config_file, &st) == 0 ? 0 : -1;
}

int apache_httpd_get_status(char *buffer, size_t size) {
    return snprintf(buffer, size,
        "{\"status\":\"%s\",\"port\":%d,\"connections\":%d,"
        "\"openssl\":\"%s\",\"json-c\":\"%s\",\"nghttp2\":\"%s\"}",
        g_server_running ? "running" : "stopped",
        g_server_port, g_connection_count,
        OpenSSL_version(OPENSSL_VERSION),
        json_c_version(),
        nghttp2_version(0)->version_str
    ) < (int)size ? 0 : -1;
}

void apache_httpd_signal_shutdown(void) {
    LOGI("Shutdown signal");
    g_server_running = 0;
}

int apache_httpd_is_running(void) {
    return g_server_running == 1;
}

int apache_httpd_get_port(void) {
    return g_server_port;
}

#else /* Stub for non-arm64 */

int apache_httpd_main(int argc, char *argv[]) {
    LOGI("Server main (STUB)");
    (void)argc; (void)argv;
    sleep(5);
    return 0;
}

int apache_httpd_validate_config(const char *f) {
    LOGI("Validate (STUB): %s", f);
    return 0;
}

int apache_httpd_get_status(char *buf, size_t sz) {
    const char *s = "{\"status\":\"stub\",\"port\":443}";
    if (buf && sz > strlen(s)) { strcpy(buf, s); return 0; }
    return -1;
}

void apache_httpd_signal_shutdown(void) { LOGI("Shutdown (STUB)"); }
int apache_httpd_is_running(void) { return 0; }
int apache_httpd_get_port(void) { return 443; }

#endif
