/*
 * Copyright 2014 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <s2n.h>

#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_connection.h"

#include "stuffer/s2n_stuffer.h"

#include "crypto/s2n_dhe.h"

#include "utils/s2n_safety.h"
#include "utils/s2n_random.h"

int s2n_server_key_recv(struct s2n_connection *conn, const char **err)
{
    struct s2n_hash_state signature_hash;
    struct s2n_stuffer *in = &conn->handshake.io;
    struct s2n_blob p, g, Ys, serverDHparams, signature;
    uint16_t p_length;
    uint16_t g_length;
    uint16_t Ys_length;
    uint16_t signature_length;

    /* Keep a copy to the start of the whole structure for the signature check */
    serverDHparams.data = s2n_stuffer_raw_read(in, 0, err);
    notnull_check(serverDHparams.data);

    /* Read each of the three elements in */
    GUARD(s2n_stuffer_read_uint16(in, &p_length, err));
    p.size = p_length;
    p.data = s2n_stuffer_raw_read(in, p.size, err);
    notnull_check(p.data);

    GUARD(s2n_stuffer_read_uint16(in, &g_length, err));
    g.size = g_length;
    g.data = s2n_stuffer_raw_read(in, g.size, err);
    notnull_check(g.data);

    GUARD(s2n_stuffer_read_uint16(in, &Ys_length, err));
    Ys.size = Ys_length;
    Ys.data = s2n_stuffer_raw_read(in, Ys.size, err);
    notnull_check(Ys.data);

    /* Now we know the total size of the structure */
    serverDHparams.size = 2 + p_length + 2 + g_length + 2 + Ys_length;

    if (conn->actual_protocol_version == S2N_TLS12) {
        uint8_t hash_algorithm;
        uint8_t signature_algorithm;

        GUARD(s2n_stuffer_read_uint8(in, &hash_algorithm, err));
        GUARD(s2n_stuffer_read_uint8(in, &signature_algorithm, err));

        if (signature_algorithm != 1) {
            *err = "Unsupported non-RSA signature algorithm";
            return -1;
        }

        if (hash_algorithm != 2) {
            *err = "Unsupported non-SHA1 hash algorithm";
            return -1;
        }
    }

    GUARD(s2n_hash_init(&signature_hash, conn->pending.signature_digest_alg, err));
    GUARD(s2n_hash_update(&signature_hash, conn->pending.client_random, S2N_TLS_RANDOM_DATA_LEN, err));
    GUARD(s2n_hash_update(&signature_hash, conn->pending.server_random, S2N_TLS_RANDOM_DATA_LEN, err));
    GUARD(s2n_hash_update(&signature_hash, serverDHparams.data, serverDHparams.size, err));

    GUARD(s2n_stuffer_read_uint16(in, &signature_length, err));
    signature.size = signature_length;
    signature.data = s2n_stuffer_raw_read(in, signature.size, err);
    notnull_check(signature.data);

    if (s2n_rsa_verify(&conn->pending.server_rsa_public_key, &signature_hash, &signature, err) < 0) {
        *err = "Server signature is invalid";
        return -1;
    }

    /* We don't need the key any more, so free it */
    GUARD(s2n_rsa_public_key_free(&conn->pending.server_rsa_public_key, err));

    /* Copy the DH details */
    GUARD(s2n_dh_p_g_Ys_to_dh_params(&conn->pending.server_dh_params, &p, &g, &Ys, err));

    conn->handshake.next_state = SERVER_HELLO_DONE;

    return 0;
}

int s2n_server_key_send(struct s2n_connection *conn, const char **err)
{
    struct s2n_blob serverDHparams, signature;
    struct s2n_stuffer *out = &conn->handshake.io;
    struct s2n_hash_state signature_hash;

    /* Duplicate the DH key from the config */
    GUARD(s2n_dh_params_copy(conn->config->dhparams, &conn->pending.server_dh_params, err));

    /* Generate an ephemeral key */
    GUARD(s2n_dh_generate_ephemeral_key(&conn->pending.server_dh_params, err));

    /* Write it out */
    GUARD(s2n_dh_params_to_p_g_Ys(&conn->pending.server_dh_params, out, &serverDHparams, err));

    if (conn->actual_protocol_version == S2N_TLS12) {
        /* SHA1 hash alg */
        GUARD(s2n_stuffer_write_uint8(out, 2, err));
        /* RSA signature type */
        GUARD(s2n_stuffer_write_uint8(out, 1, err));
    }

    GUARD(s2n_hash_init(&signature_hash, conn->pending.signature_digest_alg, err));
    GUARD(s2n_hash_update(&signature_hash, conn->pending.client_random, S2N_TLS_RANDOM_DATA_LEN, err));
    GUARD(s2n_hash_update(&signature_hash, conn->pending.server_random, S2N_TLS_RANDOM_DATA_LEN, err));
    GUARD(s2n_hash_update(&signature_hash, serverDHparams.data, serverDHparams.size, err));

    signature.size = s2n_rsa_private_encrypted_size(&conn->config->cert_and_key_pairs->private_key, err);
    GUARD(s2n_stuffer_write_uint16(out, signature.size, err));

    signature.data = s2n_stuffer_raw_write(out, signature.size, err);
    notnull_check(signature.data);

    if (s2n_rsa_sign(&conn->config->cert_and_key_pairs->private_key, &signature_hash, &signature, err) < 0) {
        *err = "Failed to sign DH parameters";
        return -1;
    }

    conn->handshake.next_state = SERVER_HELLO_DONE;

    return 0;
}