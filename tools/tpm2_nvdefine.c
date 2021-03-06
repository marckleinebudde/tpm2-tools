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

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <limits.h>

#include <sapi/tpm20.h>

#include "tpm2_password_util.h"
#include "files.h"
#include "log.h"
#include "main.h"
#include "options.h"
#include "tpm2_nv_util.h"
#include "tpm2_util.h"

typedef struct tpm_nvdefine_ctx tpm_nvdefine_ctx;
struct tpm_nvdefine_ctx {
    UINT32 nvIndex;
    UINT32 authHandle;
    UINT32 size;
    TPMA_NV nvAttribute;
    TPM2B_AUTH nvAuth;
    TPMS_AUTH_COMMAND session_data;
    TSS2_SYS_CONTEXT *sapi_context;
    char *policy_file;
};

static int nv_space_define(tpm_nvdefine_ctx *ctx) {

    TPM2B_NV_PUBLIC public_info = TPM2B_EMPTY_INIT;

    TPMS_AUTH_RESPONSE session_data_out;
    TSS2_SYS_CMD_AUTHS sessions_data;
    TSS2_SYS_RSP_AUTHS sessions_data_out;

    TPMS_AUTH_COMMAND *session_data_array[1] = {
        &ctx->session_data
    };

    TPMS_AUTH_RESPONSE *session_data_out_array[1] = {
        &session_data_out
    };

    sessions_data_out.rspAuths = &session_data_out_array[0];
    sessions_data.cmdAuths = &session_data_array[0];

    sessions_data_out.rspAuthsCount = 1;
    sessions_data.cmdAuthsCount = 1;

    public_info.t.size = sizeof(TPMI_RH_NV_INDEX) + sizeof(TPMI_ALG_HASH)
            + sizeof(TPMA_NV) + sizeof(UINT16) + sizeof(UINT16);
    public_info.t.nvPublic.nvIndex = ctx->nvIndex;
    public_info.t.nvPublic.nameAlg = TPM_ALG_SHA256;

    // Now set the attributes.
    public_info.t.nvPublic.attributes.val = ctx->nvAttribute.val;

    if (ctx->policy_file) {
        public_info.t.nvPublic.authPolicy.t.size  = BUFFER_SIZE(TPM2B_DIGEST, buffer);
        if(!files_load_bytes_from_path(ctx->policy_file, public_info.t.nvPublic.authPolicy.t.buffer, &public_info.t.nvPublic.authPolicy.t.size )) {
            return false;
        }
    } 

    public_info.t.nvPublic.dataSize = ctx->size;

    TPM_RC rval = Tss2_Sys_NV_DefineSpace(ctx->sapi_context, ctx->authHandle,
            &sessions_data, &ctx->nvAuth, &public_info, &sessions_data_out);
    if (rval != TPM_RC_SUCCESS) {
        LOG_ERR("Failed to define NV area at index 0x%x (%d).Error:0x%x",
                ctx->nvIndex, ctx->nvIndex, rval);
        return false;
    }

    LOG_INFO("Success to define NV area at index 0x%x (%d).", ctx->nvIndex, ctx->nvIndex);

    return true;
}

#define MAX_ARG_CNT ((int)(2 * (sizeof(long_options)/sizeof(long_options[0]) - 1)))

static bool init(int argc, char* argv[], tpm_nvdefine_ctx *ctx) {

    struct option long_options[] = {
        { "index",                  required_argument,  NULL,   'x' },
        { "authHandle",             required_argument,  NULL,   'a' },
        { "size",                   required_argument,  NULL,   's' },
        { "attribute",              required_argument,  NULL,   't' },
        { "handlePasswd",           required_argument,  NULL,   'P' },
        { "indexPasswd",            required_argument,  NULL,   'I' },
        { "passwdInHex",            no_argument,        NULL,   'X' },
        { "policy-file",            required_argument,  NULL,   'L' },
        { "input-session-handle",   required_argument,  NULL,   'S' },
        { NULL,                     no_argument,        NULL,    0  },
    };

    if (argc <= 1 || argc > MAX_ARG_CNT) {
        showArgMismatch(argv[0]);
        return false;
    }

    int opt;
    bool result;
    while ((opt = getopt_long(argc, argv, "x:a:s:t:P:I:rwdL:S:X", long_options, NULL))
            != -1) {
        switch (opt) {
        case 'x':
            result = tpm2_util_string_to_uint32(optarg, &ctx->nvIndex);
            if (!result) {
                LOG_ERR("Could not convert NV index to number, got: \"%s\"",
                        optarg);
                return false;
            }

            if (ctx->nvIndex == 0) {
                LOG_ERR("NV Index cannot be 0");
                return false;
            }
            break;
        case 'a':
            result = tpm2_util_string_to_uint32(optarg, &ctx->authHandle);
            if (!result) {
                LOG_ERR("Could not convert auth handle to number, got: \"%s\"",
                        optarg);
                return false;
            }

            if (ctx->authHandle == 0) {
                LOG_ERR("Auth handle cannot be 0");
                return false;
            }
            break;
        case 'P':
            result = tpm2_password_util_from_optarg(optarg, &ctx->session_data.hmac);
            if (!result) {
                LOG_ERR("Invalid handle password, got\"%s\"", optarg);
                return false;
            }
            break;
        case 's':
            result = tpm2_util_string_to_uint32(optarg, &ctx->size);
            if (!result) {
                LOG_ERR("Could not convert size to number, got: \"%s\"",
                        optarg);
                return false;
            }
            break;
        case 't':
            result = tpm2_util_string_to_uint32(optarg, &ctx->nvAttribute.val);
            if (!result) {
                result = tpm2_nv_util_strtoattr(optarg, &ctx->nvAttribute);
                if (!result) {
                    LOG_ERR("Could not convert NV attribute to number or keyword, got: \"%s\"",
                        optarg);
                    return false;
                }
            }
            break;
        case 'I':
            result = tpm2_password_util_from_optarg(optarg, &ctx->nvAuth);
            if (!result) {
                LOG_ERR("Invalid index password, got\"%s\"", optarg);
                return false;
            }
            break;
        case 'L':
            ctx->policy_file = optarg;
            break;
        case 'S':
             if (!tpm2_util_string_to_uint32(optarg, &ctx->session_data.sessionHandle)) {
                 LOG_ERR("Could not convert session handle to number, got: \"%s\"",
                         optarg);
                 return false;
             }
             break;
        case ':':
            LOG_ERR("Argument %c needs a value!", optopt);
            return false;
        case '?':
            LOG_ERR("Unknown Argument: %c", optopt);
            return false;
        default:
            LOG_ERR("?? getopt returned character code 0%o ??", opt);
            return false;
        }
    }

    return true;
}

int execute_tool(int argc, char *argv[], char *envp[], common_opts_t *opts,
            TSS2_SYS_CONTEXT *sapi_context) {

        (void)opts;
        (void)envp;

        tpm_nvdefine_ctx ctx = {
            .nvIndex = 0,
            .authHandle = TPM_RH_PLATFORM,
            .size = 0,
            .nvAttribute = SESSION_ATTRIBUTES_INIT(0),
            .session_data = TPMS_AUTH_COMMAND_INIT(TPM_RS_PW),
            .nvAuth = TPM2B_EMPTY_INIT,
            .sapi_context = sapi_context,
            .policy_file = NULL,
        };

        bool result = init(argc, argv, &ctx);
        if (!result) {
            return 1;
        }

        return nv_space_define(&ctx) != true;
}
