/**
 * @file test_webpa_method.c
 *
 * @description Unit tests for the WebPA RDK.Operate method-invocation path
 *              (webpa_method.c). The RBUS invoke wrapper (webpaRbusMethodInvoke)
 *              is stubbed here so the tests exercise detection, operate-payload
 *              decoding, error mapping and response formation without a running
 *              rtrouted/provider.
 *
 * Copyright 2016 Comcast Cable Communications Management, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <CUnit/Basic.h>
#include <cJSON.h>
#include <trower-base64/base64.h>
#include <rbus.h>

#include "../source/include/webpa_method.h"

/*----------------------------------------------------------------------------*/
/*                        Stub for the RBUS invoke wrapper                     */
/*----------------------------------------------------------------------------*/
static rbusError_t g_stubRc = RBUS_ERROR_SUCCESS;
static int g_buildOut = 0; /* when non-zero, produce an out object on success */

rbusError_t webpaRbusMethodInvoke(const char *methodName, rbusObject_t inParams, rbusObject_t *outParams)
{
        (void) methodName;
        (void) inParams;
        if(g_stubRc == RBUS_ERROR_SUCCESS && outParams != NULL && g_buildOut)
        {
                rbusValue_t v = NULL;
                rbusObject_Init(outParams, NULL);
                rbusValue_Init(&v);
                rbusValue_SetString(v, "done");
                rbusObject_SetValue(*outParams, "status", v);
                rbusValue_Release(v);
        }
        else if(outParams != NULL)
        {
                *outParams = NULL;
        }
        return g_stubRc;
}

/*----------------------------------------------------------------------------*/
/*                                 Helpers                                     */
/*----------------------------------------------------------------------------*/

/* Base64-encode a JSON string for use as the RDK.Operate value. */
static char *encodePayload(const char *json)
{
        size_t inLen = strlen(json);
        size_t encSize = b64_get_encoded_buffer_size(inLen);
        char *out = (char *) malloc(encSize + 1);
        b64_encode((const uint8_t *) json, inLen, (uint8_t *) out);
        out[encSize] = '\0';
        return out;
}

/* Build a single-parameter RDK.Operate SET request from an operate payload. */
static set_req_t *buildRequest(const char *json)
{
        set_req_t *req = (set_req_t *) calloc(1, sizeof(set_req_t));
        req->paramCnt = 1;
        req->param = (param_t *) calloc(1, sizeof(param_t));
        req->param[0].name = strdup(RDK_OPERATE_PARAM);
        req->param[0].value = encodePayload(json);
        req->param[0].type = WDMP_BASE64;
        return req;
}

static void freeRequest(set_req_t *req)
{
        size_t i = 0;
        if(req == NULL) return;
        for(i = 0; i < req->paramCnt; i++)
        {
                free(req->param[i].name);
                free(req->param[i].value);
        }
        free(req->param);
        free(req);
}

static void freeMethodRes(res_struct *res)
{
        if(res == NULL)
        {
                return;
        }
        if(res->u.paramRes != NULL)
        {
                if(res->u.paramRes->params != NULL)
                {
                        free(res->u.paramRes->params[0].name);
                        free(res->u.paramRes->params[0].value);
                        free(res->u.paramRes->params);
                }
                free(res->u.paramRes);
        }
        free(res->retStatus);
        memset(res, 0, sizeof(*res));
}

/* Parse a method response payload and return the decoded inner message JSON. */
static cJSON *decodeResponse(const char *payload, int *statusCodeOut)
{
        cJSON *root = cJSON_Parse(payload);
        cJSON *statusCode = NULL;
        cJSON *params = NULL;
        cJSON *entry = NULL;
        cJSON *message = NULL;
        cJSON *inner = NULL;
        char *decoded = NULL;
        size_t decLen = 0;

        CU_ASSERT_PTR_NOT_NULL_FATAL(root);
        statusCode = cJSON_GetObjectItem(root, "statusCode");
        CU_ASSERT_PTR_NOT_NULL_FATAL(statusCode);
        if(statusCodeOut != NULL)
        {
                *statusCodeOut = statusCode->valueint;
        }
        params = cJSON_GetObjectItem(root, "parameters");
        CU_ASSERT_PTR_NOT_NULL_FATAL(params);
        entry = cJSON_GetArrayItem(params, 0);
        CU_ASSERT_PTR_NOT_NULL_FATAL(entry);
        message = cJSON_GetObjectItem(entry, "message");
        CU_ASSERT_PTR_NOT_NULL_FATAL(message);

        decLen = b64_get_decoded_buffer_size(strlen(message->valuestring));
        decoded = (char *) malloc(decLen + 1);
        {
                size_t n = b64_decode((const uint8_t *) message->valuestring,
                        strlen(message->valuestring), (uint8_t *) decoded);
                decoded[n] = '\0';
        }
        inner = cJSON_Parse(decoded);
        free(decoded);
        cJSON_Delete(root);
        return inner;
}

static int getErrorCode(cJSON *inner)
{
        cJSON *err = cJSON_GetObjectItem(inner, "error");
        cJSON *code = NULL;
        CU_ASSERT_PTR_NOT_NULL_FATAL(err);
        code = cJSON_GetObjectItem(err, "code");
        CU_ASSERT_PTR_NOT_NULL_FATAL(code);
        return code->valueint;
}

/*----------------------------------------------------------------------------*/
/*                                 Test cases                                  */
/*----------------------------------------------------------------------------*/

void test_isMethodInvokeRequest_positive(void)
{
        set_req_t *req = buildRequest("{\"method\":\"Device.Test()\"}");
        CU_ASSERT_TRUE(isMethodInvokeRequest(req));
        freeRequest(req);
}

void test_isMethodInvokeRequest_negative(void)
{
        set_req_t req;
        param_t p;
        p.name = "Device.DeviceInfo.ModelName";
        p.value = "abc";
        p.type = WDMP_STRING;
        req.param = &p;
        req.paramCnt = 1;
        req.rspDestination = NULL;
        CU_ASSERT_FALSE(isMethodInvokeRequest(&req));
}

void test_multiParameter_rejected(void)
{
        /* Two parameters -> invalid request (-32600). */
        set_req_t *req = (set_req_t *) calloc(1, sizeof(set_req_t));
        res_struct res;
        memset(&res, 0, sizeof(res));

        req->paramCnt = 2;
        req->param = (param_t *) calloc(2, sizeof(param_t));
        req->param[0].name = strdup(RDK_OPERATE_PARAM);
        req->param[0].value = encodePayload("{\"method\":\"Device.Test()\"}");
        req->param[0].type = WDMP_BASE64;
        req->param[1].name = strdup("Device.Extra");
        req->param[1].value = strdup("x");
        req->param[1].type = WDMP_STRING;

        CU_ASSERT_TRUE(isMethodInvokeRequest(req));
        handleMethodInvoke(req, &res);
        CU_ASSERT_PTR_NOT_NULL_FATAL(res.retStatus);
        CU_ASSERT_EQUAL(res.retStatus[0], WDMP_FAILURE);
        freeMethodRes(&res);
        freeRequest(req);
}

void test_jsonParseFailure(void)
{
        /* Valid base64 but not valid JSON -> -32700. */
        set_req_t *req = buildRequest("this is not json {");
        res_struct res;
        memset(&res, 0, sizeof(res));

        handleMethodInvoke(req, &res);
        CU_ASSERT_PTR_NOT_NULL_FATAL(res.retStatus);
        CU_ASSERT_EQUAL(res.retStatus[0], WDMP_FAILURE);
        freeMethodRes(&res);
        freeRequest(req);
}

void test_missingMethod(void)
{
        set_req_t *req = buildRequest("{\"params\":{}}");
        res_struct res;
        memset(&res, 0, sizeof(res));

        handleMethodInvoke(req, &res);
        CU_ASSERT_PTR_NOT_NULL_FATAL(res.retStatus);
        CU_ASSERT_EQUAL(res.retStatus[0], WDMP_FAILURE);
        freeMethodRes(&res);
        freeRequest(req);
}

void test_destinationNotFound(void)
{
        set_req_t *req = buildRequest("{\"method\":\"Device.NoSuch()\"}");
        res_struct res;
        memset(&res, 0, sizeof(res));

        g_stubRc = RBUS_ERROR_DESTINATION_NOT_FOUND;
        g_buildOut = 0;
        handleMethodInvoke(req, &res);
        CU_ASSERT_PTR_NOT_NULL_FATAL(res.retStatus);
        CU_ASSERT_EQUAL(res.retStatus[0], WDMP_ERR_METHOD_NOT_SUPPORTED);
        freeMethodRes(&res);
        freeRequest(req);
}

void test_invalidInput(void)
{
        set_req_t *req = buildRequest("{\"method\":\"Device.Test()\"}");
        res_struct res;
        memset(&res, 0, sizeof(res));

        g_stubRc = RBUS_ERROR_INVALID_INPUT;
        g_buildOut = 0;
        handleMethodInvoke(req, &res);
        CU_ASSERT_PTR_NOT_NULL_FATAL(res.retStatus);
        CU_ASSERT_EQUAL(res.retStatus[0], WDMP_ERR_INVALID_INPUT_PARAMETER);
        freeMethodRes(&res);
        freeRequest(req);
}

void test_internalError(void)
{
        set_req_t *req = buildRequest("{\"method\":\"Device.Test()\"}");
        res_struct res;
        memset(&res, 0, sizeof(res));

        g_stubRc = RBUS_ERROR_BUS_ERROR;
        g_buildOut = 0;
        handleMethodInvoke(req, &res);
        CU_ASSERT_PTR_NOT_NULL_FATAL(res.retStatus);
        CU_ASSERT_EQUAL(res.retStatus[0], WDMP_FAILURE);
        freeMethodRes(&res);
        freeRequest(req);
}

void test_successResponse(void)
{
        set_req_t *req = buildRequest(
                "{\"method\":\"Device.Test()\",\"params\":{\"arg1\":{\"value\":\"5\",\"dataType\":1}}}");
        res_struct res;
        memset(&res, 0, sizeof(res));

        g_stubRc = RBUS_ERROR_SUCCESS;
        g_buildOut = 1;
        handleMethodInvoke(req, &res);
        CU_ASSERT_PTR_NOT_NULL_FATAL(res.retStatus);
        CU_ASSERT_EQUAL(res.retStatus[0], WDMP_SUCCESS);
        CU_ASSERT_PTR_NOT_NULL_FATAL(res.u.paramRes);
        CU_ASSERT_PTR_NOT_NULL_FATAL(res.u.paramRes->params);
        CU_ASSERT_STRING_EQUAL(res.u.paramRes->params[0].name, "Device.Test()");
        freeMethodRes(&res);
        freeRequest(req);
}

void test_rspDestinationAsyncRejected(void)
{
        /* rspDestination in operate payload indicates async mode which is
         * not supported in Phase 1 and is rejected as invalid request. */
        set_req_t *req = buildRequest(
                "{\"method\":\"Device.Test()\",\"rspDestination\":\"event:csi-stream/results\"}");
        res_struct res;
        memset(&res, 0, sizeof(res));

        g_stubRc = RBUS_ERROR_SUCCESS;
        g_buildOut = 1;
        handleMethodInvoke(req, &res);
        CU_ASSERT_PTR_NOT_NULL_FATAL(res.retStatus);
        CU_ASSERT_EQUAL(res.retStatus[0], WDMP_FAILURE);
        freeMethodRes(&res);
        freeRequest(req);
}

int main(void)
{
        CU_pSuite suite = NULL;

        if(CUE_SUCCESS != CU_initialize_registry())
        {
                return CU_get_error();
        }
        suite = CU_add_suite("webpa_method_suite", NULL, NULL);
        if(NULL == suite)
        {
                CU_cleanup_registry();
                return CU_get_error();
        }

        CU_add_test(suite, "test_isMethodInvokeRequest_positive", test_isMethodInvokeRequest_positive);
        CU_add_test(suite, "test_isMethodInvokeRequest_negative", test_isMethodInvokeRequest_negative);
        CU_add_test(suite, "test_multiParameter_rejected", test_multiParameter_rejected);
        CU_add_test(suite, "test_jsonParseFailure", test_jsonParseFailure);
        CU_add_test(suite, "test_missingMethod", test_missingMethod);
        CU_add_test(suite, "test_destinationNotFound", test_destinationNotFound);
        CU_add_test(suite, "test_invalidInput", test_invalidInput);
        CU_add_test(suite, "test_internalError", test_internalError);
        CU_add_test(suite, "test_successResponse", test_successResponse);
        CU_add_test(suite, "test_rspDestinationAsyncRejected", test_rspDestinationAsyncRejected);

        CU_basic_set_mode(CU_BRM_VERBOSE);
        CU_basic_run_tests();
        CU_cleanup_registry();
        return CU_get_error();
}
