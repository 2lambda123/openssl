/*
 * Copyright 2021 The OpenSSL Project Authors. All Rights Reserved.
 * Copyright Siemens AG 2021
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <openssl/http.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <string.h>

#include "testutil.h"

typedef struct {
    const char *server;
    const char *port;
    int use_proxy;
    long timeout;
    SSL_CTX *ssl_ctx;
} http_tls_info;

/* HTTP callback function that supports TLS connection also via HTTPS proxy */
static BIO *http_tls_cb(BIO *bio, void *arg, int connect, int detail)
{
    http_tls_info *info = (http_tls_info *)arg;
    SSL_CTX *ssl_ctx = info->ssl_ctx;

    if (ssl_ctx == NULL)
        return bio; /* not using TLS */
    if (connect) {
        SSL *ssl;
        BIO *sbio = NULL;

        /* To adapt after fixing callback design flaw, see #17088 */
        if ((info->use_proxy
             && !OSSL_HTTP_proxy_connect(bio, info->server, info->port,
                                         NULL, NULL, /* no proxy credentials */
                                         info->timeout, bio_err, opt_getprog()))
                || (sbio = BIO_new(BIO_f_ssl())) == NULL) {
            return NULL;
        }
        if ((ssl = SSL_new(ssl_ctx)) == NULL) {
            BIO_free(sbio);
            return NULL;
        }

        /* To adapt after fixing callback design flaw, see #17088 */
        SSL_set_tlsext_host_name(ssl, info->server); /* not critical to do */

        SSL_set_connect_state(ssl);
        BIO_set_ssl(sbio, ssl, BIO_CLOSE);

        bio = BIO_push(sbio, bio);
    } else { /* disconnect */
        BIO *cbio = BIO_pop(bio); /* connect(+HTTP write) BIO */

        BIO_free(bio); /* SSL BIO */
        bio = cbio;
    }
    return bio;
}

static int test_http_wget(void)
{
    const char *url = test_get_argument(0);
    char *server = NULL;
    char *port = NULL; /* default: "80" or "443" */
    char *path = NULL;
    int use_ssl = HAS_PREFIX(url, OSSL_HTTPS_PREFIX);
    const char *proxy = NULL; /* defaults to env. var. "http_proxy" */
    const char *no_proxy = NULL; /* defaults to env. var. "no_proxy" */
    BIO *bio = NULL, *rbio = NULL;
    int buf_size = -1;
    STACK_OF(CONF_VALUE) *headers = NULL;
    const char *expected_ct = NULL; /* in general, no expectations here */
    const size_t max_resp_len = 0; /* no limit */
    int flags = 0; /* no keep-alive, retry, redirect, nor ASN.1 */
    long timeout = 2; /* should be sufficient */
#ifndef USE_HIGH_LEVEL_API
    OSSL_HTTP_REQ_CTX *rctx = NULL;
#endif
    BIO *resp = NULL;
    char buf[1024];
    http_tls_info info = { NULL, NULL, 0, 0, NULL };
    int res = 0, len = 0, n_read = 0;

    /* To adapt after fixing callback design flaw, see #17088 */
    if (!TEST_true(OSSL_HTTP_parse_url(url, &use_ssl, NULL /* usr */, &server,
                                       &port, NULL /* port_num */, &path,
                                       NULL, NULL)))
        return 0;
    info.server = server;
    info.port = port;
    info.use_proxy = /* workaround for callback design flaw, see #17088 */
        OSSL_HTTP_adapt_proxy(proxy, no_proxy, server, use_ssl) != NULL;
    info.timeout = timeout;

    if (use_ssl && !TEST_ptr((info.ssl_ctx = SSL_CTX_new(TLS_client_method()))))
        goto err;
    /* trivial TLS connection won't do any checks */
    if (!TEST_true(X509V3_add_value("Dummy", "4711", &headers)))
        goto err;
#ifdef USE_HIGH_LEVEL_API
    res = 1;
    resp = OSSL_HTTP_get_ex(url, proxy, no_proxy,
                            bio, rbio, http_tls_cb, &info,
                            buf_size, headers,
                            expected_ct, flags, max_resp_len,
                            0 /* default_retry_after */, timeout);
#else
    {
        BIO *req = NULL;
        const char *content_type = NULL;
        int expect_asn1 = 0;

        rctx = OSSL_HTTP_open(server, port, proxy, no_proxy, use_ssl,
                              bio, rbio, use_ssl ? http_tls_cb : NULL,
                              &info, buf_size, (int)timeout);
        res = TEST_ptr(rctx)
            && TEST_true(OSSL_HTTP_set_overall_timeout(rctx, timeout))
            && TEST_true(OSSL_HTTP_set1_request(rctx, path, headers,
                                                content_type, req,
                                                expected_ct, expect_asn1,
                                                max_resp_len,
                                                -1 /* timeout */, flags));
        if (rctx != NULL)
            resp = OSSL_HTTP_exchange(rctx, NULL);
    }
#endif

    if (resp != NULL)
        do {
            len = BIO_read(resp, buf, sizeof(buf));
            if (len > 0) {
                fprintf(stdout, "%.*s", len,  buf);
                n_read += len;
            }
        } while (len > 0);
    res = res && TEST_ptr(resp) && n_read > 0;
    test_note("Got %d bytes of HTTP content from %s\n", n_read, url);

#ifdef USE_HIGH_LEVEL_API
    BIO_free(resp);
#else
    res = res && TEST_true(OSSL_HTTP_close(rctx, resp != NULL));
#endif

 err:
    sk_CONF_VALUE_pop_free(headers, X509V3_conf_free);
    SSL_CTX_free(info.ssl_ctx);
    OPENSSL_free(server);
    OPENSSL_free(port);
    OPENSSL_free(path);
    return res;
}

OPT_TEST_DECLARE_USAGE("<url>\n")

int setup_tests(void)
{
    if (!test_skip_common_options())
        return 0;

    ADD_TEST(test_http_wget);
    return 1;
}
