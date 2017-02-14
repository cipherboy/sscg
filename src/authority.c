/*
    This file is part of sscg.

    sscg is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    sscg is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with sscg.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017 by Stephen Gallagher <sgallagh@redhat.com>
*/

#include "include/sscg.h"
#include "include/authority.h"
#include "include/x509.h"
#include "include/key.h"

int
create_private_CA(TALLOC_CTX *mem_ctx, const struct sscg_options *options,
                  struct sscg_x509_cert **_cacert,
                  struct sscg_evp_pkey **_cakey)
{
    int ret;
    int bits;
    size_t i;
    TALLOC_CTX *tmp_ctx = NULL;
    struct sscg_bignum *e;
    struct sscg_bignum *serial;
    struct sscg_cert_info *ca_certinfo;
    struct sscg_x509_req *csr;
    struct sscg_evp_pkey *pkey;
    struct sscg_x509_cert *cert;
    X509_EXTENSION *ex = NULL;
    X509V3_CTX xctx;
    char *name_constraint;

    tmp_ctx = talloc_new(NULL);
    CHECK_MEM(tmp_ctx);

    /* create a serial number for this certificate */
    ret = sscg_generate_serial(tmp_ctx, &serial);
    CHECK_OK(ret);

    ca_certinfo = sscg_cert_info_new(tmp_ctx, options->hash_fn);
    CHECK_MEM(ca_certinfo);

    /* Populate cert_info from options */
    ca_certinfo->country = talloc_strdup(ca_certinfo, options->country);
    CHECK_MEM(ca_certinfo->country);

    ca_certinfo->state = talloc_strdup(ca_certinfo, options->state);
    CHECK_MEM(ca_certinfo->state);

    ca_certinfo->locality = talloc_strdup(ca_certinfo, options->locality);
    CHECK_MEM(ca_certinfo->locality);

    ca_certinfo->org = talloc_strdup(ca_certinfo, options->org);
    CHECK_MEM(ca_certinfo->org);

    ca_certinfo->org_unit = talloc_strdup(ca_certinfo, options->org_unit);
    CHECK_MEM(ca_certinfo->org_unit);

    ca_certinfo->cn = talloc_asprintf(ca_certinfo,"ca-%lu.%s",
                                      BN_get_word(serial->bn),
                                      options->hostname);
    CHECK_MEM(ca_certinfo->cn);

    /* Make this a CA certificate */

    /* Add key extensions */
    ex = X509V3_EXT_conf_nid(
        NULL, NULL, NID_key_usage,
        "critical,digitalSignature,keyEncipherment,keyCertSign");
    CHECK_MEM(ex);
    sk_X509_EXTENSION_push(ca_certinfo->extensions, ex);

    /* Mark it as a CA */
    ex = X509V3_EXT_conf_nid(NULL, NULL,
                             NID_basic_constraints,
                             "CA:TRUE");
    CHECK_MEM(ex);
    sk_X509_EXTENSION_push(ca_certinfo->extensions, ex);

    /* Restrict this certificate to being able to sign only the hostname
       and SubjectAltNames for the requested service certificate */
    name_constraint = talloc_asprintf(tmp_ctx,
                                      "permitted;DNS:%s",
                                      options->hostname);
    CHECK_MEM(name_constraint);
    ex = X509V3_EXT_conf_nid(NULL, NULL,
                             NID_name_constraints,
                             name_constraint);
    CHECK_MEM(ex);
    sk_X509_EXTENSION_push(ca_certinfo->extensions, ex);
    talloc_free(name_constraint);

    for (i = 0; options->subject_alt_names[i]; i++) {
        name_constraint = talloc_asprintf(tmp_ctx,
                                          "permitted;DNS:%s",
                                          options->subject_alt_names[i]);
        CHECK_MEM(name_constraint);
        ex = X509V3_EXT_conf_nid(NULL, NULL,
                                 NID_name_constraints,
                                 name_constraint);
        CHECK_MEM(ex);
        sk_X509_EXTENSION_push(ca_certinfo->extensions, ex);
        talloc_free(name_constraint);
    }

    /* Also give it privilege to sign itself */
    name_constraint = talloc_asprintf(tmp_ctx,
                                      "permitted;DNS:%s",
                                      ca_certinfo->cn);
    CHECK_MEM(name_constraint);
    ex = X509V3_EXT_conf_nid(NULL, NULL,
                             NID_name_constraints,
                             name_constraint);
    CHECK_MEM(ex);
    sk_X509_EXTENSION_push(ca_certinfo->extensions, ex);
    talloc_free(name_constraint);

    /* For the private CA, we always use 4096 bits and an exponent
       value of RSA F4 aka 0x10001 (65537) */
    bits = 4096;
    ret = sscg_init_bignum(tmp_ctx, RSA_F4, &e);
    CHECK_OK(ret);

    /* Generate an RSA keypair for this CA */
    if (options->verbosity >= SSCG_VERBOSE) {
        fprintf(stdout, "Generating RSA key for private CA.\n");
    }
    /* TODO: support DSA keys as well */
    ret = sscg_generate_rsa_key(tmp_ctx, bits, e, &pkey);
    CHECK_OK(ret);

    /* Create a certificate signing request for the private CA */
    if (options->verbosity >= SSCG_VERBOSE) {
        fprintf(stdout, "Generating CSR for private CA.\n");
    }
    ret = sscg_x509v3_csr_new(tmp_ctx, ca_certinfo, pkey, &csr);
    CHECK_OK(ret);

    if (options->verbosity >= SSCG_DEBUG) {
        fprintf(stderr, "DEBUG: Writing CA CSR to ./debug-ca.csr\n");
        BIO *ca_csr_out = BIO_new_file("./debug-ca.csr","w");
        int sslret = PEM_write_bio_X509_REQ(ca_csr_out, csr->x509_req);
        CHECK_SSL(sslret, PEM_write_bio_X509_REQ);
    }

    X509V3_set_ctx_nodb(&xctx);
    X509V3_set_ctx(&xctx, NULL, NULL, csr->x509_req, NULL, 0);

    /* Set the Subject Key Identifier extension */
    if (options->verbosity >= SSCG_DEBUG) {
        fprintf(stderr, "DEBUG: Creating SubjectKeyIdentifier\n");
    }
    ex = X509V3_EXT_conf_nid(NULL, &xctx,
                             NID_subject_key_identifier,
                             "hash");
    if (!ex) {
        /* Get information about error from OpenSSL */
        fprintf(stderr, "Error occurred in "
                        "X509V3_EXT_conf_nid(SubjectKeyIdentifier): [%s].\n",
                ERR_error_string(ERR_get_error(), NULL));
        ret = EIO;
        goto done;
    }
    sk_X509_EXTENSION_push(ca_certinfo->extensions, ex);

    /* Finalize the CSR */
    ret = sscg_x509v3_csr_finalize(ca_certinfo, pkey, csr);


    /* Self-sign the private CA */
    if (options->verbosity >= SSCG_VERBOSE) {
        fprintf(stdout, "Signing CSR for private CA.\n");
    }
    ret = sscg_sign_x509_csr(tmp_ctx, csr, serial, options->lifetime,
                             NULL, pkey, options->hash_fn, &cert);
    CHECK_OK(ret);

    *_cacert = talloc_steal(mem_ctx, cert);
    *_cakey = talloc_steal(mem_ctx, pkey);

    ret = EOK;

done:
    talloc_zfree(tmp_ctx);
    return ret;
}
