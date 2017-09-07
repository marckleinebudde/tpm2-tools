//**********************************************************************;
// Copyright (c) 2015, Intel Corporation
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// 3. Neither the name of Intel Corporation nor the names of its contributors
// may be used to endorse or promote products derived from this software without
// specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
//**********************************************************************;
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <curl/curl.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <sapi/tpm20.h>

#include "tpm2_options.h"
#include "log.h"
#include "files.h"
#include "tpm_hash.h"
#include "tpm2_alg_util.h"
#include "tpm2_tool.h"
#include "tpm2_util.h"

typedef struct tpm_getmanufec_ctx tpm_getmanufec_ctx;
struct tpm_getmanufec_ctx {
    char *output_file;
    char *owner_passwd;
    char *endorse_passwd;
    char *ek_passwd;
    char *ec_cert_path;
    bool hex_passwd;
    TPM_HANDLE persistent_handle;
    UINT32 algorithm_type;
    FILE *ec_cert_file;
    char *ek_server_addr;
    unsigned int non_persistent_read;
    unsigned int SSL_NO_VERIFY;
    unsigned int offline_prov;
    bool is_session_based_auth;
    TPMI_SH_AUTH_SESSION auth_session_handle;
    bool verbose;
};

static tpm_getmanufec_ctx ctx = {
    .algorithm_type = TPM_ALG_RSA
};

BYTE authPolicy[] = {0x83, 0x71, 0x97, 0x67, 0x44, 0x84, 0xB3, 0xF8,
                     0x1A, 0x90, 0xCC, 0x8D, 0x46, 0xA5, 0xD7, 0x24,
                     0xFD, 0x52, 0xD7, 0x6E, 0x06, 0x52, 0x0B, 0x64,
                     0xF2, 0xA1, 0xDA, 0x1B, 0x33, 0x14, 0x69, 0xAA};

int set_key_algorithm(TPM2B_PUBLIC *inPublic) {
    inPublic->t.publicArea.nameAlg = TPM_ALG_SHA256;
    // First clear attributes bit field.
    *(UINT32 *)&(inPublic->t.publicArea.objectAttributes) = 0;
    inPublic->t.publicArea.objectAttributes.restricted = 1;
    inPublic->t.publicArea.objectAttributes.userWithAuth = 0;
    inPublic->t.publicArea.objectAttributes.adminWithPolicy = 1;
    inPublic->t.publicArea.objectAttributes.sign = 0;
    inPublic->t.publicArea.objectAttributes.decrypt = 1;
    inPublic->t.publicArea.objectAttributes.fixedTPM = 1;
    inPublic->t.publicArea.objectAttributes.fixedParent = 1;
    inPublic->t.publicArea.objectAttributes.sensitiveDataOrigin = 1;
    inPublic->t.publicArea.authPolicy.t.size = 32;
    memcpy(inPublic->t.publicArea.authPolicy.t.buffer, authPolicy, 32);

    inPublic->t.publicArea.type = ctx.algorithm_type;

    switch (ctx.algorithm_type) {
    case TPM_ALG_RSA:
        inPublic->t.publicArea.parameters.rsaDetail.symmetric.algorithm = TPM_ALG_AES;
        inPublic->t.publicArea.parameters.rsaDetail.symmetric.keyBits.aes = 128;
        inPublic->t.publicArea.parameters.rsaDetail.symmetric.mode.aes = TPM_ALG_CFB;
        inPublic->t.publicArea.parameters.rsaDetail.scheme.scheme = TPM_ALG_NULL;
        inPublic->t.publicArea.parameters.rsaDetail.keyBits = 2048;
        inPublic->t.publicArea.parameters.rsaDetail.exponent = 0x0;
        inPublic->t.publicArea.unique.rsa.t.size = 256;
        break;
    case TPM_ALG_KEYEDHASH:
        inPublic->t.publicArea.parameters.keyedHashDetail.scheme.scheme = TPM_ALG_XOR;
        inPublic->t.publicArea.parameters.keyedHashDetail.scheme.details.exclusiveOr.hashAlg = TPM_ALG_SHA256;
        inPublic->t.publicArea.parameters.keyedHashDetail.scheme.details.exclusiveOr.kdf = TPM_ALG_KDF1_SP800_108;
        inPublic->t.publicArea.unique.keyedHash.t.size = 0;
        break;
    case TPM_ALG_ECC:
        inPublic->t.publicArea.parameters.eccDetail.symmetric.algorithm = TPM_ALG_AES;
        inPublic->t.publicArea.parameters.eccDetail.symmetric.keyBits.aes = 128;
        inPublic->t.publicArea.parameters.eccDetail.symmetric.mode.sym = TPM_ALG_CFB;
        inPublic->t.publicArea.parameters.eccDetail.scheme.scheme = TPM_ALG_NULL;
        inPublic->t.publicArea.parameters.eccDetail.curveID = TPM_ECC_NIST_P256;
        inPublic->t.publicArea.parameters.eccDetail.kdf.scheme = TPM_ALG_NULL;
        inPublic->t.publicArea.unique.ecc.x.t.size = 32;
        inPublic->t.publicArea.unique.ecc.y.t.size = 32;
        break;
    case TPM_ALG_SYMCIPHER:
        inPublic->t.publicArea.parameters.symDetail.sym.algorithm = TPM_ALG_AES;
        inPublic->t.publicArea.parameters.symDetail.sym.keyBits.aes = 128;
        inPublic->t.publicArea.parameters.symDetail.sym.mode.sym = TPM_ALG_CFB;
        inPublic->t.publicArea.unique.sym.t.size = 0;
        break;
    default:
        LOG_ERR("\nThe algorithm type input(%4.4x) is not supported!", ctx.algorithm_type);
        return -1;
    }

    return 0;
}

int createEKHandle(TSS2_SYS_CONTEXT *sapi_context)
{
    UINT32 rval;
    TPMS_AUTH_COMMAND sessionData = {
            .sessionHandle = TPM_RS_PW,
            .nonce = TPM2B_EMPTY_INIT,
            .hmac = TPM2B_EMPTY_INIT,
            .sessionAttributes = SESSION_ATTRIBUTES_INIT(0),
    };
    if (ctx.is_session_based_auth) {
        sessionData.sessionHandle = ctx.auth_session_handle;
    }
    TPMS_AUTH_RESPONSE sessionDataOut;
    TSS2_SYS_CMD_AUTHS sessionsData;
    TSS2_SYS_RSP_AUTHS sessionsDataOut;
    TPMS_AUTH_COMMAND *sessionDataArray[1];
    TPMS_AUTH_RESPONSE *sessionDataOutArray[1];

    TPM2B_SENSITIVE_CREATE inSensitive = TPM2B_TYPE_INIT(TPM2B_SENSITIVE_CREATE, sensitive);
    TPM2B_PUBLIC inPublic = TPM2B_TYPE_INIT(TPM2B_PUBLIC, publicArea);

    TPM2B_DATA outsideInfo = TPM2B_EMPTY_INIT;
    TPML_PCR_SELECTION creationPCR;

    TPM2B_NAME name = TPM2B_TYPE_INIT(TPM2B_NAME, name);

    TPM2B_PUBLIC outPublic = TPM2B_EMPTY_INIT;
    TPM2B_CREATION_DATA creationData = TPM2B_EMPTY_INIT;
    TPM2B_DIGEST creationHash = TPM2B_TYPE_INIT(TPM2B_DIGEST, buffer);
    TPMT_TK_CREATION creationTicket = TPMT_TK_CREATION_EMPTY_INIT;

    TPM_HANDLE handle2048ek;

    sessionDataArray[0] = &sessionData;
    sessionDataOutArray[0] = &sessionDataOut;

    sessionsDataOut.rspAuths = &sessionDataOutArray[0];
    sessionsData.cmdAuths = &sessionDataArray[0];

    sessionsDataOut.rspAuthsCount = 1;
    sessionsData.cmdAuthsCount = 1;

    /*
     * use enAuth in Tss2_Sys_CreatePrimary
     */
    if (strlen(ctx.endorse_passwd) > 0 && !ctx.hex_passwd) {
            sessionData.hmac.t.size = strlen(ctx.endorse_passwd);
            memcpy( &sessionData.hmac.t.buffer[0], ctx.endorse_passwd, sessionData.hmac.t.size );
    }
    else {
        if (strlen(ctx.endorse_passwd) > 0 && ctx.hex_passwd) {
                sessionData.hmac.t.size = sizeof(sessionData.hmac) - 2;

                if (tpm2_util_hex_to_byte_structure(ctx.endorse_passwd, &sessionData.hmac.t.size,
                                      sessionData.hmac.t.buffer) != 0) {
                        LOG_ERR("Failed to convert Hex format password for endorsePasswd.");
                        return -1;
                }
        }
    }

    if (strlen(ctx.ek_passwd) > 0 && !ctx.hex_passwd) {
        inSensitive.t.sensitive.userAuth.t.size = strlen(ctx.ek_passwd);
        memcpy(&inSensitive.t.sensitive.userAuth.t.buffer[0], ctx.ek_passwd,
               inSensitive.t.sensitive.userAuth.t.size );
    }
    else {
        if (strlen(ctx.ek_passwd) > 0 && ctx.hex_passwd) {
             inSensitive.t.sensitive.userAuth.t.size = sizeof(inSensitive.t.sensitive.userAuth) - 2;
             if (tpm2_util_hex_to_byte_structure(ctx.ek_passwd, &inSensitive.t.sensitive.userAuth.t.size,
                                   inSensitive.t.sensitive.userAuth.t.buffer) != 0) {
                  LOG_ERR("Failed to convert Hex format password for ekPasswd.");
                  return -1;
            }
        }
    }

    inSensitive.t.sensitive.data.t.size = 0;
    inSensitive.t.size = inSensitive.t.sensitive.userAuth.b.size + 2;

    if (set_key_algorithm(&inPublic) )
          return -1;

    creationPCR.count = 0;

    rval = Tss2_Sys_CreatePrimary(sapi_context, TPM_RH_ENDORSEMENT, &sessionsData,
                                  &inSensitive, &inPublic, &outsideInfo,
                                  &creationPCR, &handle2048ek, &outPublic,
                                  &creationData, &creationHash, &creationTicket,
                                  &name, &sessionsDataOut);
    if (rval != TPM_RC_SUCCESS ) {
          LOG_ERR("\nTPM2_CreatePrimary Error. TPM Error:0x%x", rval);
          return -2;
    }
    LOG_INFO("\nEK create succ.. Handle: 0x%8.8x", handle2048ek);

    if (!ctx.non_persistent_read) {
         /*
          * To make EK persistent, use own auth
          */
         sessionData.hmac.t.size = 0;
         if (strlen(ctx.owner_passwd) > 0 && !ctx.hex_passwd) {
             sessionData.hmac.t.size = strlen(ctx.owner_passwd);
             memcpy(&sessionData.hmac.t.buffer[0], ctx.owner_passwd, sessionData.hmac.t.size );
         } else {
             if (strlen(ctx.owner_passwd) > 0 && ctx.hex_passwd) {
                sessionData.hmac.t.size = sizeof(sessionData.hmac) - 2;
                if (tpm2_util_hex_to_byte_structure(ctx.owner_passwd, &sessionData.hmac.t.size,
                                   sessionData.hmac.t.buffer) != 0) {
                 LOG_ERR("Failed to convert Hex format password for ownerPasswd.");
                 return -1;
                }
            }
        }

        rval = Tss2_Sys_EvictControl(sapi_context, TPM_RH_OWNER, handle2048ek,
                                     &sessionsData, ctx.persistent_handle, &sessionsDataOut);
        if (rval != TPM_RC_SUCCESS ) {
            LOG_ERR("\nEvictControl:Make EK persistent Error. TPM Error:0x%x", rval);
            return -3;
        }
        LOG_INFO("EvictControl EK persistent succ.");
    }

    rval = Tss2_Sys_FlushContext(sapi_context,
                                 handle2048ek);
    if (rval != TPM_RC_SUCCESS ) {
        LOG_ERR("\nFlush transient EK failed. TPM Error:0x%x", rval);
        return -4;
    }

    LOG_INFO("Flush transient EK succ.");

    /* TODO this serialization is not correct */
    if (!files_save_bytes_to_file(ctx.output_file, (UINT8 *)&outPublic, sizeof(outPublic))) {
        LOG_ERR("\nFailed to save EK pub key into file(%s)", ctx.output_file);
        return -5;
    }

    return 0;
}

unsigned char *HashEKPublicKey(void)
{
    unsigned char *hash = NULL;
    FILE *fp = NULL;

    unsigned char EKpubKey[259];

    LOG_INFO("Calculating the SHA256 hash of the Endorsement Public Key");

    fp = fopen(ctx.output_file, "rb");
    if (!fp) {
        LOG_ERR("Could not open file: \"%s\"", ctx.output_file);
        return NULL;
    }

    int rc = fseek(fp, 0x66, 0);
    if (rc < 0) {
        LOG_ERR("Could not perform fseek: %s", strerror(errno));
        goto out;
    }

    size_t read = fread(EKpubKey, 1, 256, fp);
    if (read != 256) {
        LOG_ERR ("Could not read whole file.");
        goto out;
    }

    hash = (unsigned char*)malloc(SHA256_DIGEST_LENGTH);
    if (hash == NULL) {
        LOG_ERR ("OOM");
        goto out;
    }

    EKpubKey[256] = 0x01;
    EKpubKey[257] = 0x00;
    EKpubKey[258] = 0x01; //Exponent
    SHA256_CTX sha256;
    int is_success = SHA256_Init(&sha256);
    if (!is_success) {
        LOG_ERR ("SHA256_Init failed");
        goto hash_out;
    }

    is_success = SHA256_Update(&sha256, EKpubKey, sizeof(EKpubKey));
    if (!is_success) {
        LOG_ERR ("SHA256_Update failed");
        goto hash_out;
    }

    is_success = SHA256_Final(hash, &sha256);
    if (!is_success) {
        LOG_ERR ("SHA256_Final failed");
        goto hash_out;
    }

    if (ctx.verbose) {
        unsigned i;
        for (i = 0; i < SHA256_DIGEST_LENGTH; i++) {
            printf("%02X", hash[i]);
        }
        printf("\n");
    }
    goto out;

hash_out:
    free(hash);
    hash = NULL;
out:
    fclose(fp);
    return hash;
}

char *Base64Encode(const unsigned char* buffer)
{
    BIO *bio, *b64;
    BUF_MEM *bufferPtr;

    LOG_INFO("Calculating the Base64Encode of the hash of the Endorsement Public Key:");

    if (buffer == NULL) {
        LOG_ERR("HashEKPublicKey returned null");
        return NULL;
    }

    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, buffer, SHA256_DIGEST_LENGTH);
    BIO_flush(bio);
    BIO_get_mem_ptr(bio, &bufferPtr);
    BIO_set_close(bio, BIO_NOCLOSE);

    /* these are not NULL terminated */
    char *b64text = bufferPtr->data;
    size_t len = bufferPtr->length;

    size_t i;
    for (i = 0; i < len; i++) {
        if (b64text[i] == '+') {
            b64text[i] = '-';
        }
        if (b64text[i] == '/') {
            b64text[i] = '_';
        }
    }

    char *final_string = NULL;

    CURL *curl = curl_easy_init();
    if (curl) {
        char *output = curl_easy_escape(curl, b64text, len);
        if (output) {
            final_string = strdup(output);
            curl_free(output);
        }
    }
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    BIO_free_all(bio);

    /* format to a proper NULL terminated string */
    return final_string;
}

int RetrieveEndorsementCredentials(char *b64h)
{
    int ret = -1;

    size_t len = 1 + strlen(b64h) + strlen(ctx.ek_server_addr);
    char *weblink = (char *)malloc(len);
    if (!weblink) {
        LOG_ERR("oom");
        return ret;
    }

    snprintf(weblink, len, "%s%s", ctx.ek_server_addr, b64h);

    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) {
        LOG_ERR("curl_global_init failed: %s", curl_easy_strerror(rc));
        goto out_memory;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        LOG_ERR("curl_easy_init failed");
        goto out_global_cleanup;
    }

    /*
     * should not be used - Used only on platforms with older CA certificates.
     */
    if (ctx.SSL_NO_VERIFY) {
        rc = curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
        if (rc != CURLE_OK) {
            LOG_ERR("curl_easy_setopt for CURLOPT_SSL_VERIFYPEER failed: %s", curl_easy_strerror(rc));
            goto out_easy_cleanup;
        }
    }

    rc = curl_easy_setopt(curl, CURLOPT_URL, weblink);
    if (rc != CURLE_OK) {
        LOG_ERR("curl_easy_setopt for CURLOPT_URL failed: %s", curl_easy_strerror(rc));
        goto out_easy_cleanup;
    }

    /*
     * If verbose is set, add in diagnostic information for debugging connections.
     * https://curl.haxx.se/libcurl/c/CURLOPT_VERBOSE.html
     */
    rc = curl_easy_setopt(curl, CURLOPT_VERBOSE, (long)ctx.verbose);
    if (rc != CURLE_OK) {
        LOG_ERR("curl_easy_setopt for CURLOPT_VERBOSE failed: %s", curl_easy_strerror(rc));
        goto out_easy_cleanup;
    }

    /*
     * If an output file is specified, write to the file, else curl will use stdout:
     * https://curl.haxx.se/libcurl/c/CURLOPT_WRITEDATA.html
     */
    if (ctx.ec_cert_file) {
        rc = curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx.ec_cert_file);
        if (rc != CURLE_OK) {
            LOG_ERR("curl_easy_setopt for CURLOPT_WRITEDATA failed: %s", curl_easy_strerror(rc));
            goto out_easy_cleanup;
        }
    }

    rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        LOG_ERR("curl_easy_perform() failed: %s", curl_easy_strerror(rc));
        goto out_easy_cleanup;
    }

    ret = 0;

out_easy_cleanup:
    curl_easy_cleanup(curl);
out_global_cleanup:
    curl_global_cleanup();
out_memory:
    free(weblink);

    return ret;
}


int TPMinitialProvisioning(void)
{
    if (ctx.ek_server_addr == NULL) {
        LOG_ERR("TPM Manufacturer Endorsement Credential Server Address cannot be NULL");
        return -99;
    }

    char *b64 = Base64Encode(HashEKPublicKey());
    if (!b64) {
        LOG_ERR("Base64Encode returned null");
        return -1;
    }

    LOG_INFO("%s", b64);

    int rc = RetrieveEndorsementCredentials(b64);
    free(b64);
    return rc;
}

static bool on_option(char key, char *value) {

    bool return_val;

    switch (key) {
    case 'H':
        if (!tpm2_util_string_to_uint32(value, &ctx.persistent_handle)) {
            LOG_ERR("\nPlease input the handle used to make EK persistent(hex) in correct format.");
            return false;
        }
        break;
    case 'e':
        if (value == NULL || (strlen(value) >= sizeof(TPMU_HA)) ) {
            LOG_ERR("\nPlease input the endorsement password(optional,no more than %d characters).",
                    (int)sizeof(TPMU_HA) - 1);
            return false;
        }
        ctx.endorse_passwd = value;
        break;
    case 'o':
        if (value == NULL || (strlen(value) >= sizeof(TPMU_HA)) ) {
            LOG_ERR("\nPlease input the owner password(optional,no more than %d characters).",
                    (int)sizeof(TPMU_HA) - 1);
            return false;
        }
        ctx.owner_passwd = value;
        break;
    case 'P':
        if (value == NULL || (strlen(value) >= sizeof(TPMU_HA)) ) {
            LOG_ERR("\nPlease input the EK password(optional,no more than %d characters).",
                    (int)sizeof(TPMU_HA) - 1);
            return false;
        }
        ctx.ek_passwd = value;
        break;
    case 'g':
        ctx.algorithm_type = tpm2_alg_util_from_optarg(value);
        if (ctx.algorithm_type == TPM_ALG_ERROR) {
             LOG_ERR("\nPlease input the algorithm type in correct format.");
            return false;
        }
        break;
    case 'f':
        if (value == NULL ) {
            LOG_ERR("\nPlease input the file used to save the pub ek.");
            return false;
        }
        ctx.output_file = value;
        break;
    case 'X':
        ctx.hex_passwd = true;
        break;
    case 'E':
        ctx.ec_cert_path = value;
        break;
    case 'N':
        ctx.non_persistent_read = 1;
        break;
    case 'O':
        ctx.offline_prov = 1;
        break;
    case 'U':
        ctx.SSL_NO_VERIFY = 1;
        LOG_WARN("TLS communication with the said TPM manufacturer server setup with SSL_NO_VERIFY!");
        break;
    case 'S':
        if (ctx.ek_server_addr) {
            LOG_ERR("Multiple specifications of -S");
            return false;
        }
        ctx.ek_server_addr = value;
        break;
    case 'i':
        return_val = tpm2_util_string_to_uint32(value, &ctx.auth_session_handle);
        if (!return_val) {
            LOG_ERR("Could not convert session handle to number, got: \"%s\"",
                    value);
            return false;
        }
        ctx.is_session_based_auth = true;
        break;
    }

    return true;
}

bool tpm2_tool_onstart(tpm2_options **opts) {

    const struct option topts[] =
    {
        { "endorsePasswd", 1, NULL, 'e' },
        { "ownerPasswd"  , 1, NULL, 'o' },
        { "handle"       , 1, NULL, 'H' },
        { "ekPasswd"     , 1, NULL, 'P' },
        { "alg"          , 1, NULL, 'g' },
        { "file"         , 1, NULL, 'f' },
        { "passwdInHex"  , 0, NULL, 'X' },
        { "NonPersistent", 0, NULL, 'N' },
        { "OfflineProv"  , 0, NULL, 'O' },
        { "ECcertFile"   , 1, NULL, 'E' },
        { "EKserverAddr" , 1, NULL, 'S' },
        { "SSL_NO_VERIFY", 0, NULL, 'U' },
        {"input-session-handle",1,NULL,'i'},
        { NULL           , 0, NULL,  0  },
    };

    *opts = tpm2_options_new("e:o:H:P:g:f:X:N:O:E:S:i:U", ARRAY_LEN(topts), topts, on_option, NULL);

    return *opts != NULL;
}

int tpm2_tool_onrun(TSS2_SYS_CONTEXT *sapi_context, tpm2_option_flags flags) {

    UNUSED(flags);
    int return_val = 1;
    int provisioning_return_val = 0;

    ctx.verbose = flags.verbose;

    if (ctx.ec_cert_path) {
        ctx.ec_cert_file = fopen(ctx.ec_cert_path, "wb");
        if (!ctx.ec_cert_file) {
            LOG_ERR("Could not open file for writing: \"%s\"", ctx.ec_cert_path);
            return 1;
        }
    }

    if (!ctx.offline_prov) {
        return_val = createEKHandle(sapi_context);
    }

    provisioning_return_val = TPMinitialProvisioning();

    if (return_val && provisioning_return_val) {
        goto err;
    }

    return_val = 0;

err:
    if (ctx.ec_cert_file) {
        fclose(ctx.ec_cert_file);
    }

    return return_val;
}
