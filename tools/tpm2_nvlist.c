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

#include <sapi/tpm20.h>

#include "tpm2_nv_util.h"
#include "tpm2_util.h"
#include "log.h"
#include "main.h"
#include "options.h"

static void print_nv_public(TPM2B_NV_PUBLIC *nv_public) {

    char *attrs = tpm2_nv_util_attrtostr(nv_public->t.nvPublic.attributes);
    if (!attrs) {
        LOG_ERR("Could not convert attributes to string form");
    }

    printf("  {\n");
    printf("\tHash algorithm(nameAlg):%d\n ", nv_public->t.nvPublic.nameAlg);
    printf("\tattributes: %s(0x%X)\n ", attrs,
            tpm2_util_ntoh_32(nv_public->t.nvPublic.attributes.val));
    printf("\tThe size of the data area(dataSize):%d\n ",
            nv_public->t.nvPublic.dataSize);
    printf("\tAuthorization Policy for R/W/D: ");
    int i;
    for(i=0; i<nv_public->t.nvPublic.authPolicy.t.size; i++) {
        printf("%02X", nv_public->t.nvPublic.authPolicy.t.buffer[i] );
    }
    printf("\n  }\n");

    free(attrs);
}

static bool nv_list(TSS2_SYS_CONTEXT *sapi_context) {

    TPMI_YES_NO moreData;
    TPMS_CAPABILITY_DATA capabilityData;
    UINT32 property = tpm2_util_endian_swap_32(TPM_HT_NV_INDEX);
    TPM_RC rval = Tss2_Sys_GetCapability(sapi_context, 0, TPM_CAP_HANDLES,
            property, TPM_PT_NV_INDEX_MAX, &moreData, &capabilityData, 0);
    if (rval != TPM_RC_SUCCESS) {
        LOG_ERR("GetCapability:Get NV Index list Error. TPM Error:0x%x", rval);
        return false;
    }

    printf("%d NV indexes defined.\n", capabilityData.data.handles.count);

    UINT32 i;
    for (i = 0; i < capabilityData.data.handles.count; i++) {
        TPMI_RH_NV_INDEX index = capabilityData.data.handles.handle[i];

        printf("\n  %d. NV Index: 0x%x\n", i, index);

        TPM2B_NV_PUBLIC nv_public = TPM2B_EMPTY_INIT;
        rval = tpm2_util_nv_read_public(sapi_context, index, &nv_public);
        if (rval != TPM_RC_SUCCESS) {
            LOG_ERR("Reading the public part of the nv index failed with: 0x%x", rval);
            return false;
        }
        print_nv_public(&nv_public);
    }
    printf("\n");

    return true;
}

int execute_tool(int argc, char *argv[], char *envp[], common_opts_t *opts,
        TSS2_SYS_CONTEXT *sapi_context) {

    (void) argc;
    (void) argv;
    (void) envp;
    (void) opts;

    return !nv_list(sapi_context);
}
