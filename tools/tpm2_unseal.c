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

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <getopt.h>
#include <sapi/tpm20.h>

#include "tpm2_password_util.h"
#include "files.h"
#include "log.h"
#include "main.h"
#include "options.h"
#include "pcr.h"
#include "tpm2_policy.h"
#include "tpm2_util.h"
#include "tpm_hash.h"
#include "tpm_session.h"

typedef struct tpm_unseal_ctx tpm_unseal_ctx;
struct tpm_unseal_ctx {
    TPMS_AUTH_COMMAND sessionData;
    TPMI_DH_OBJECT itemHandle;
    char *outFilePath;
    TSS2_SYS_CONTEXT *sapi_context;
    SESSION *policy_session;
};

bool unseal_and_save(tpm_unseal_ctx *ctx) {

    TPMS_AUTH_RESPONSE session_data_out;
    TSS2_SYS_CMD_AUTHS sessions_data;
    TSS2_SYS_RSP_AUTHS sessions_data_out;
    TPMS_AUTH_COMMAND *session_data_array[1];
    TPMS_AUTH_RESPONSE *session_data_out_array[1];

    TPM2B_SENSITIVE_DATA outData = TPM2B_TYPE_INIT(TPM2B_SENSITIVE_DATA, buffer);

    session_data_array[0] = &ctx->sessionData;
    session_data_out_array[0] = &session_data_out;

    sessions_data_out.rspAuths = &session_data_out_array[0];
    sessions_data.cmdAuths = &session_data_array[0];

    sessions_data_out.rspAuthsCount = 1;
    sessions_data.cmdAuthsCount = 1;

    TPM_RC rval = Tss2_Sys_Unseal(ctx->sapi_context, ctx->itemHandle,
            &sessions_data, &outData, &sessions_data_out);
    if (rval != TPM_RC_SUCCESS) {
        LOG_ERR("Sys_Unseal failed. Error Code: 0x%x", rval);
        return false;
    }

    if (ctx->outFilePath) {
        return files_save_bytes_to_file(ctx->outFilePath, (UINT8 *)
                                        outData.t.buffer, outData.t.size);
    } else {
        return files_write_bytes(stdout, (UINT8 *) outData.t.buffer,
                                 outData.t.size);
    }
}

static bool init(int argc, char *argv[], tpm_unseal_ctx *ctx) {

    static const char *optstring = "H:P:o:c:S:L:F:";
    static const struct option long_options[] = {
      {"item",1,NULL,'H'},
      {"pwdi",1,NULL,'P'},
      {"outfile",1,NULL,'o'},
      {"itemContext",1,NULL,'c'},
      {"input-session-handle",1,NULL,'S'},
      {"set-list", 1, NULL, 'L' },
      {"pcr-input-file", 1, NULL, 'F' },
      {0,0,0,0}
    };

    if (argc == 1) {
        showArgMismatch(argv[0]);
        return false;
    }

    union {
        struct {
            UINT8 H : 1;
            UINT8 c : 1;
            UINT8 P : 1;
            UINT8 L : 1;
            UINT8 F : 1;
        };
        UINT8 all;
    } flags = { .all = 0 };

    int opt;
    char *contextItemFile = NULL;
    char *raw_pcrs_file = NULL;
    TPML_PCR_SELECTION pcr_selections;
    while ((opt = getopt_long(argc, argv, optstring, long_options, NULL)) != -1) {
        switch (opt) {
        case 'H': {
            bool result = tpm2_util_string_to_uint32(optarg, &ctx->itemHandle);
            if (!result) {
                LOG_ERR("Could not convert item handle to number, got: \"%s\"",
                        optarg);
                return false;
            }
            flags.H = 1;
        }
            break;
        case 'P': {
            bool result = tpm2_password_util_from_optarg(optarg, &ctx->sessionData.hmac);
            if (!result) {
                LOG_ERR("Invalid item handle password, got\"%s\"", optarg);
                return false;
            }
            flags.P = 1;
        }
            break;
        case 'o': {
            bool result = files_does_file_exist(optarg);
            if (result) {
                return false;
            }
            ctx->outFilePath = optarg;
        }
            break;
        case 'c':
            contextItemFile = optarg;
            flags.c = 1;
            break;
        case 'S': {
            bool result = tpm2_util_string_to_uint32(optarg,
                &ctx->sessionData.sessionHandle);
            if (!result) {
                LOG_ERR("Could not convert session handle to number, got: \"%s\"",
                        optarg);
                return false;
            }
        }
            break;
        case 'L':
            if (!pcr_parse_selections(optarg, &pcr_selections)) {
                showArgError(optarg, argv[0]);
                return false;
            }
            flags.L = 1;
            break;
        case 'F':
            raw_pcrs_file = optarg;
            flags.F = 1;
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

    if (!(flags.H || flags.c)) {
        LOG_ERR("Expected options H or c");
        return false;
    }

    if (flags.c) {
        bool result = files_load_tpm_context_from_file(ctx->sapi_context, &ctx->itemHandle,
                contextItemFile);
        if (!result) {
            return false;
        }
    }

    if (flags.L && flags.F) {
        TPM2B_DIGEST pcr_digest = TPM2B_TYPE_INIT(TPM2B_DIGEST, buffer);

        TPM_RC rval = tpm2_policy_build(ctx->sapi_context, &ctx->policy_session,
                                        TPM_SE_POLICY, TPM_ALG_SHA256, pcr_selections,
                                        raw_pcrs_file, &pcr_digest, true,
                                        tpm2_policy_pcr_build);
        if (rval != TPM_RC_SUCCESS) {
            LOG_ERR("Building PCR policy failed: 0x%x", rval);
            return false;
        }

        ctx->sessionData.sessionHandle = ctx->policy_session->sessionHandle;
        ctx->sessionData.sessionAttributes.continueSession = 1;
    }

    return true;
}

int execute_tool(int argc, char *argv[], char *envp[], common_opts_t *opts,
        TSS2_SYS_CONTEXT *sapi_context) {

    /* opts and envp are unused, avoid compiler warning */
    (void)opts;
    (void) envp;

    tpm_unseal_ctx ctx = {
            .sessionData = TPMS_AUTH_COMMAND_INIT(TPM_RS_PW),
            .sapi_context = sapi_context,
            .policy_session = NULL
    };

    bool result = init(argc, argv, &ctx);
    if (!result) {
        return 1;
    }

    if (!unseal_and_save(&ctx)) {
        LOG_ERR("Unseal failed!");
        return 1;
    }

    if (ctx.policy_session) {
        TPM_RC rval = Tss2_Sys_FlushContext(ctx.sapi_context,
                                            ctx.policy_session->sessionHandle);
        if (rval != TPM_RC_SUCCESS) {
            LOG_ERR("Failed Flush Context: 0x%x", rval);
            return 1;
        }

        tpm_session_auth_end(ctx.policy_session);
    }

    return 0;
}
