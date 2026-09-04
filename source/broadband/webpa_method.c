/**
 * @file webpa_method.c
 *
 * @description Implements the WebPA cloud-to-CPE RPC method invocation path.
 *              A method request arrives as an ordinary WebPA PATCH/SET carrying
 *              a single reserved parameter named RDK.Operate (dataType 5,
 *              WDMP_BASE64) whose value is a Base64-encoded UTF-8 JSON operate
 *              payload: { "method": "...", "params": { ... },
 *              "rspDestination": "..." }. When rspDestination is absent the
 *              method is invoked synchronously via rbusMethod_Invoke and the
 *              result is returned directly in the HTTP response.  When
 *              rspDestination is present the method is dispatched asynchronously
 *              via rbusMethod_InvokeAsync; WebPA immediately ACKs the cloud and
 *              later delivers the provider result as a WRP event notification.
 *
 * Copyright (c) 2026  Comcast
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>

#include <cJSON.h>
#include <trower-base64/base64.h>

#include "webpa_method.h"
#include "webpa_adapter.h"
#include "webpa_rbus.h"
#include "webpa_notification.h"

/*----------------------------------------------------------------------------*/
/*                             Function Prototypes                            */
/*----------------------------------------------------------------------------*/
static char *base64Decode(const char *in, size_t *outLen);
static char *base64Encode(const char *in);
static char *buildErrorObject(int code, const char *data);
static char *buildErrorObjectFromJson(int code, cJSON *dataObj);
static char *buildMethodResponse(const char *name, int statusCode, cJSON *messageObj);
static rbusValueType_t wdmpToRbusType(int wdmpType);
static int mapRbusErrorToMethodError(rbusError_t rc);
static int jsonScalarToRbusValue(cJSON *val, rbusValue_t *out);
static int jsonLeafToRbusValue(cJSON *val, int wdmpType, rbusValue_t *out);
static int jsonObjectToRbus(cJSON *jsonObj, rbusObject_t rbusObj);
static cJSON *rbusValueToJson(rbusValue_t val);
static int rbusObjectToJson(rbusObject_t obj, cJSON *jsonOut);
#if 0
static void dumpRbusObjectWithTimestamp(const char *path, const char *label,
                const char *responseName, rbusObject_t obj);
#endif                
static int parseOperatePayload(set_req_t *setReq,
                char **methodName,
                rbusObject_t *inParams, bool *isAsync,
                char **rspDestination, char **errorObj);

/* Async method handling */
static void enqueueMethodResponse(MethodAsyncResponse *resp);
static MethodAsyncResponse *dequeueMethodResponse(void);
static int handleAsyncMethodInvoke(const char *methodName, const char *rspDestination,
                                    rbusObject_t inParams, res_struct *resObj,
                                    char **errorObj);
static void webpaAsyncMethodCallback(rbusHandle_t handle, char const *methodName,
                                      rbusError_t error, rbusObject_t outParams,
                                      void *userData);
static void *methodAsyncResponseConsumer(void *arg);

#define METHOD_ASYNC_TIMEOUT_SEC        1800  /* 30 minutes; passed to rbusMethod_InvokeAsync */

/*----------------------------------------------------------------------------*/
/*                         Async state – response queue                       */
/*----------------------------------------------------------------------------*/
typedef struct MethodRespMsg {
        MethodAsyncResponse    *resp;
        struct MethodRespMsg   *next;
} MethodRespMsg;

static MethodRespMsg   *s_methodRespQ     = NULL;
static pthread_mutex_t  s_methodRespMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   s_methodRespCond  = PTHREAD_COND_INITIALIZER;

/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/

bool isMethodInvokeRequest(set_req_t *setReq)
{
    if(setReq == NULL || setReq->param == NULL)
        return false;

    return (setReq->param[0].name != NULL &&
            strcmp(setReq->param[0].name, RDK_OPERATE_PARAM) == 0);
}

void handleMethodInvoke(set_req_t *setReq, res_struct *resObj)
{
        const char *responseName = RDK_OPERATE_PARAM;
        char *methodName = NULL;
        char *rspDestination = NULL;
        char *errorObj = NULL;
        char *resultStr = NULL;
        char *b64message = NULL;
        cJSON *resultObj = NULL;
        rbusObject_t inParams = NULL;
        rbusObject_t outParams = NULL;
        WDMP_STATUS methodStatus = WDMP_FAILURE;
        rbusError_t rc = RBUS_ERROR_SUCCESS;
        bool isAsync = false;

        WalPrint("************** handleMethodInvoke *****************\n");

        /* Validate and parse the operate payload into method name and input params. */
        if(parseOperatePayload(setReq, &methodName,
                              &inParams, &isAsync, &rspDestination, &errorObj) != 0)
        {
                goto respond;
        }
        responseName = methodName;

        if(isAsync)
        {
                int asyncRet = handleAsyncMethodInvoke(responseName, rspDestination,
                                                        inParams, resObj, &errorObj);
                WAL_FREE(rspDestination);
                if(asyncRet == 0)
                {
                        /* resObj already populated with ACK; release remaining locals */
                        WAL_FREE(methodName);
                        if(inParams != NULL)
                                rbusObject_Release(inParams);
                        WalPrint("************** handleMethodInvoke (async ACK sent) *****************\n");
                        return;
                }
                /* Async invoke failed; errorObj is set; fall through to respond */
                goto respond;
        }

        /* Invoke the method synchronously (blocking) and return the result
         * directly in the response payload carried by the HTTP response. */
        rc = webpaRbusMethodInvoke(responseName, inParams, &outParams);
#if 0
        dumpRbusObjectWithTimestamp("/tmp/webpa_method_inParams.txt", "inParams", responseName, inParams);
        dumpRbusObjectWithTimestamp("/tmp/webpa_method_outParams.txt", "outParams", responseName, outParams);
#endif
        if((rc != RBUS_ERROR_SUCCESS))
        {
                int code = mapRbusErrorToMethodError(rc);
                const char *rbusDetail = rbusError_ToString(rc);
                char detail[256] = {'\0'};
                snprintf(detail, sizeof(detail), "RBUS error: %s", (rbusDetail != NULL && rbusDetail[0] != '\0') ? rbusDetail : "unknown");                
                WalError("rbusMethod_Invoke failed for method '%s' with error code %d: %s\n", responseName, rc, detail);
                if(outParams != NULL)
                {
                        resultObj = cJSON_CreateObject();
                        if(rbusObjectToJson(outParams, resultObj) == 0)
                        {
                                errorObj = buildErrorObjectFromJson(code, resultObj);                                
                        }
                        else
                        {
                                WalError("Failed to convert RBUS outParams to JSON\n");
                                errorObj = buildErrorObject(METHOD_ERR_INTERNAL, "Failed to convert RBUS result object to JSON");
                        }
                }
                else
                {
                        errorObj = buildErrorObject(code, detail);
                }
                goto respond;
        }

        /* Convert the RBUS result object back into JSON. */
        resultObj = cJSON_CreateObject();
        if(outParams != NULL)
        {       
                if(rbusObjectToJson(outParams, resultObj) != 0)
                {
                        WalError("Failed to convert RBUS result object to JSON\n");
                        cJSON_Delete(resultObj);
                        resultObj = NULL;
                        errorObj = buildErrorObject(METHOD_ERR_INTERNAL,
                                "Failed to convert RBUS result object to JSON");
                        goto respond;
                }
        }
        else
        {
                WalInfo("method outParams is empty\n");
        }

        cJSON *resultWrapper = cJSON_CreateObject();
        if(resultWrapper == NULL)
        {
                WalError("Failed to allocate result wrapper JSON object\n");
                errorObj = buildErrorObject(METHOD_ERR_INTERNAL,"Failed to serialize method result JSON");
                goto respond;
        }
        cJSON_AddItemToObject(resultWrapper, "result", resultObj);
        resultObj = resultWrapper;
        resultStr = cJSON_PrintUnformatted(resultObj);

        if(resultStr == NULL)
        {
                WalError("Failed to serialize method result JSON\n");
                errorObj = buildErrorObject(METHOD_ERR_INTERNAL,
                        "Failed to serialize method result JSON");
                goto respond;
        }
        WalInfo("Method response: %s\n", resultStr);

        methodStatus = WDMP_SUCCESS;
        WalPrint("Method %s invoked successfully\n", responseName);

respond:
        if(resObj != NULL)
        {
                resObj->reqType = METHOD;
                resObj->paramCnt = 1;

                if(resObj->retStatus == NULL)
                {
                        resObj->retStatus = (WDMP_STATUS *) malloc(sizeof(WDMP_STATUS));
                }
                if(resObj->retStatus != NULL)
                {
                        resObj->retStatus[0] = methodStatus;
                }

                if(resObj->u.paramRes == NULL)
                {
                        resObj->u.paramRes = (param_res_t *) malloc(sizeof(param_res_t));
                        if(resObj->u.paramRes != NULL)
                        {
                                memset(resObj->u.paramRes, 0, sizeof(param_res_t));
                        }
                }

                if(resObj->u.paramRes != NULL && resObj->u.paramRes->params == NULL)
                {
                        resObj->u.paramRes->params = (param_t *) malloc(sizeof(param_t));
                        if(resObj->u.paramRes->params != NULL)
                        {
                                memset(resObj->u.paramRes->params, 0, sizeof(param_t));
                        }
                }

                if(resObj->u.paramRes != NULL && resObj->u.paramRes->params != NULL)
                {
                        WAL_FREE(resObj->u.paramRes->params[0].name);
                        WAL_FREE(resObj->u.paramRes->params[0].value);

                        resObj->u.paramRes->params[0].name = strdup(responseName);
                        if(errorObj != NULL)
                        {
                                WalPrint("Encoding method error payload for %s: %s\n", responseName, errorObj);
                                b64message = base64Encode(errorObj);
                                if(b64message != NULL)
                                {
                                        resObj->u.paramRes->params[0].value = strdup(b64message);
                                        WAL_FREE(b64message);
                                }
                                else
                                {
                                        WalError("Failed to base64-encode method error payload for %s\n",
                                                responseName);
                                        resObj->u.paramRes->params[0].value = NULL;
                                }
                                resObj->u.paramRes->params[0].type = WDMP_BASE64;
                        }
                        else if(resultStr != NULL)
                        {
                                WalPrint("Encoding method result payload for %s: %s\n", responseName, resultStr);
                                b64message = base64Encode(resultStr);
                                if(b64message != NULL)
                                {
                                        resObj->u.paramRes->params[0].value = strdup(b64message);
                                        WAL_FREE(b64message);
                                }
                                else
                                {
                                        WalError("Failed to base64-encode method result payload for %s\n",
                                                responseName);
                                        resObj->u.paramRes->params[0].value = NULL;
                                }
                                resObj->u.paramRes->params[0].type = WDMP_BASE64;
                        }
                        else
                        {
                                WalError("No method result/error payload available for %s\n",
                                        responseName);
                                resObj->u.paramRes->params[0].value = NULL;
                                resObj->u.paramRes->params[0].type = WDMP_NONE;
                        }
                }
        }

        WAL_FREE(errorObj);
        WAL_FREE(methodName);
        WAL_FREE(rspDestination);
        WAL_FREE(resultStr);
        if(resultObj != NULL)
        {
                cJSON_Delete(resultObj);
        }
        WAL_FREE(b64message);
        if(inParams != NULL)
        {
                rbusObject_Release(inParams);
        }
        if(outParams != NULL)
        {
                rbusObject_Release(outParams);
        }
        WalPrint("************** handleMethodInvoke *****************\n");
}

/**
 * @brief parseOperatePayload validates the set request and parses the Base64-encoded
 *        operate JSON payload into the method name and RBUS input parameters.
 *
 * @param[in]  setReq      incoming set request
 * @param[out] methodName  extracted method name (caller frees)
 * @param[out] inParams    RBUS input object (caller releases)
 * @param[out] errorObj    error JSON string on failure (caller frees)
 * @return 0 on success, -1 on failure (errorObj is set)
 */
static int parseOperatePayload(set_req_t *setReq,
                char **methodName,
                rbusObject_t *inParams, bool *isAsync,
                char **rspDestination, char **errorObj)
{
        char *decoded = NULL;
        size_t decodedLen = 0;
        cJSON *operateJson = NULL;
        cJSON *methodItem = NULL;
        cJSON *paramsItem = NULL;
        cJSON *rspDestinationItem = NULL;
        const char *rspDest = NULL;
        param_t *p = NULL;

        /* A method request carries exactly one RDK.Operate parameter. */
        if(setReq->paramCnt != 1)
        {
                WalError("RDK.Operate method request must carry exactly one parameter\n");
                *errorObj = buildErrorObject(METHOD_ERR_INVALID_REQUEST,
                        "RDK.Operate method request must carry exactly one parameter");
                return -1;
        }

        p = &setReq->param[0];
        if(p->type != WDMP_BASE64)
        {
                WalError("RDK.Operate parameter must be Base64 (dataType 5)\n");
                *errorObj = buildErrorObject(METHOD_ERR_INVALID_REQUEST,
                        "RDK.Operate parameter must be Base64 (dataType 5)");
                return -1;
        }
        if(p->value == NULL)
        {
                WalError("RDK.Operate parameter value is missing\n");
                *errorObj = buildErrorObject(METHOD_ERR_INVALID_REQUEST,
                        "RDK.Operate parameter value is missing");
                return -1;
        }

        /* Base64-decode the operate payload. */
        decoded = base64Decode(p->value, &decodedLen);
        if(decoded == NULL)
        {
                WalError("Failed to Base64-decode operate payload\n");
                *errorObj = buildErrorObject(METHOD_ERR_PARSE,
                        "Failed to Base64-decode operate payload");
                return -1;
        }
        WalPrint("Base64-decoded operate payload (%zu bytes): %s\n", decodedLen, decoded);

        /* Parse the decoded operate payload JSON. */
        operateJson = cJSON_Parse(decoded);
        WAL_FREE(decoded);
        if(operateJson == NULL)
        {
                WalError("Operate payload is not valid JSON\n");
                *errorObj = buildErrorObject(METHOD_ERR_PARSE,
                        "Operate payload is not valid JSON");
                return -1;
        }

        /* Extract the mandatory method name. */
        methodItem = cJSON_GetObjectItem(operateJson, "method");
        if(methodItem == NULL || !cJSON_IsString(methodItem) ||
                methodItem->valuestring == NULL || methodItem->valuestring[0] == '\0')
        {
                WalError("Operate payload is missing the method name\n");
                *errorObj = buildErrorObject(METHOD_ERR_INVALID_REQUEST,
                        "Operate payload is missing the method name");
                cJSON_Delete(operateJson);
                return -1;
        }
        *methodName = strdup(methodItem->valuestring);
        if(*methodName == NULL)
        {
                WalError("Failed to allocate memory for method name\n");
                *errorObj = buildErrorObject(METHOD_ERR_INTERNAL,
                        "Failed to allocate memory for method name");
                cJSON_Delete(operateJson);
                return -1;
        }
        WalPrint("Method invocation target: %s\n", *methodName);

        /* Convert the optional params object into an RBUS input object. */
        rbusObject_Init(inParams, NULL);
        paramsItem = cJSON_GetObjectItem(operateJson, "params");
        if(paramsItem != NULL && cJSON_IsObject(paramsItem))
        {
                if(jsonObjectToRbus(paramsItem, *inParams) != 0)
                {
                        WalError("Failed to convert params to RBUS input object\n");
                        *errorObj = buildErrorObject(METHOD_ERR_INTERNAL,
                                "Failed to convert params to RBUS input object");
                        cJSON_Delete(operateJson);
                        return -1;
                }
        }

        /* Optional rspDestination controls invocation mode. When provided,
         * the request is asynchronous. */
        *isAsync = false;
        *rspDestination = NULL;
        rspDestinationItem = cJSON_GetObjectItem(operateJson, "rspDestination");
        if(rspDestinationItem != NULL)
        {
                if(!cJSON_IsString(rspDestinationItem) || rspDestinationItem->valuestring == NULL)
                {
                        WalError("Operate payload has invalid rspDestination\n");
                        *errorObj = buildErrorObject(METHOD_ERR_INVALID_REQUEST,
                                "Operate payload has invalid rspDestination");
                        cJSON_Delete(operateJson);
                        return -1;
                }
                rspDest = rspDestinationItem->valuestring;
                if(rspDest[0] != '\0')
                {
                        *isAsync = true;
                        *rspDestination = strdup(rspDest);
                        if(*rspDestination == NULL)
                        {
                                WalError("Failed to allocate memory for rspDestination\n");
                                *errorObj = buildErrorObject(METHOD_ERR_INTERNAL,
                                        "Failed to allocate memory for rspDestination");
                                cJSON_Delete(operateJson);
                                return -1;
                        }
                }
        }

        cJSON_Delete(operateJson);
        return 0;
}

/*----------------------------------------------------------------------------*/
/*                         Async state – pending request context              */
/*----------------------------------------------------------------------------*/

/* Carries the cloud correlation context through to the rbus async callback. */
typedef struct PendingAsyncReq {
        char     *methodName;
        char     *rspDestination;
} PendingAsyncReq;

/*----------------------------------------------------------------------------*/
/*                         Async response queue                               */
/*----------------------------------------------------------------------------*/

static void enqueueMethodResponse(MethodAsyncResponse *resp)
{
        MethodRespMsg *msg = (MethodRespMsg *) malloc(sizeof(MethodRespMsg));
        if(msg == NULL)
        {
                WalError("enqueueMethodResponse: malloc failed\n");
                return;
        }
        msg->resp = resp;
        msg->next = NULL;

        pthread_mutex_lock(&s_methodRespMutex);
        if(s_methodRespQ == NULL)
        {
                s_methodRespQ = msg;
        }
        else
        {
                MethodRespMsg *tail = s_methodRespQ;
                while(tail->next != NULL) tail = tail->next;
                tail->next = msg;
        }
        WalInfo("Enqueued async method response for %s\n",
                resp && resp->methodName ? resp->methodName : "<null>");
        pthread_cond_signal(&s_methodRespCond);
        pthread_mutex_unlock(&s_methodRespMutex);
}

/* Blocks until a response is available; caller owns the returned pointer. */
static MethodAsyncResponse *dequeueMethodResponse(void)
{
        MethodRespMsg *msg;
        MethodAsyncResponse *resp;

        pthread_mutex_lock(&s_methodRespMutex);
        while(s_methodRespQ == NULL)
                pthread_cond_wait(&s_methodRespCond, &s_methodRespMutex);
        msg           = s_methodRespQ;
        s_methodRespQ = msg->next;
        pthread_mutex_unlock(&s_methodRespMutex);

        resp = msg->resp;
        free(msg);
        return resp;
}

/*----------------------------------------------------------------------------*/
/*                         RBUS async callback                                */
/*----------------------------------------------------------------------------*/

static void webpaAsyncMethodCallback(rbusHandle_t handle, char const *methodName,
                                      rbusError_t error, rbusObject_t outParams,
                                      void *userData)
{
        (void) handle;
        MethodAsyncResponse *resp = NULL;
        PendingAsyncReq    *pending = (PendingAsyncReq *) userData;
        cJSON *payloadJson        = NULL;
        char *resultPayload       = NULL;

        WalInfo("webpaAsyncMethodCallback: method=%s error=%d\n",
                methodName != NULL ? methodName : "<null>", error);

        if(methodName == NULL)
        {
                WalError("webpaAsyncMethodCallback: NULL method name in callback\n");
                if(pending != NULL)
                {
                        WAL_FREE(pending->methodName);
                        WAL_FREE(pending->rspDestination);
                        WAL_FREE(pending);
                }
                return;
        }

        if(pending == NULL)
        {
                WalError("webpaAsyncMethodCallback: NULL userData for %s\n", methodName);
                return;
        }

        if(error != RBUS_ERROR_SUCCESS)
        {
                int         code   = mapRbusErrorToMethodError(error);
                const char *detail = rbusError_ToString(error);
                char        msg[256] = {'\0'};
                cJSON      *errInner, *errObj;

                snprintf(msg, sizeof(msg), "RBUS error: %s",
                         (detail != NULL && detail[0] != '\0') ? detail : "unknown");
                WalError("webpaAsyncMethodCallback: method '%s' failed: %s\n", methodName, msg);

                errObj   = cJSON_CreateObject();
                errInner = cJSON_CreateObject();
                cJSON_AddNumberToObject(errInner, "code", code);
                if(outParams != NULL)
                {
                        cJSON *outJson = cJSON_CreateObject();
                        if(rbusObjectToJson(outParams, outJson) == 0)
                                cJSON_AddItemToObject(errInner, "data", outJson);
                        else
                        {
                                cJSON_Delete(outJson);
                                cJSON_AddStringToObject(errInner, "data", msg);
                        }
                }
                else
                {
                        cJSON_AddStringToObject(errInner, "data", msg);
                }
                cJSON_AddItemToObject(errObj, "error", errInner);
                payloadJson = errObj;
        }
        else
        {
                cJSON *resultObj    = cJSON_CreateObject();
                cJSON *resultWrapper = cJSON_CreateObject();

                if(outParams != NULL && rbusObjectToJson(outParams, resultObj) != 0)
                {
                        WalError("webpaAsyncMethodCallback: rbusObjectToJson failed for %s\n",
                                 methodName);
                        cJSON_Delete(resultObj);
                        resultObj = cJSON_CreateObject();
                }
                cJSON_AddItemToObject(resultWrapper, "result", resultObj);
                payloadJson = resultWrapper;
                WalInfo("webpaAsyncMethodCallback: method '%s' succeeded\n", methodName);
        }

        /* WRP event payload is the raw result/error JSON per spec */
        resultPayload = cJSON_PrintUnformatted(payloadJson);
        cJSON_Delete(payloadJson);

        if(resultPayload == NULL)
        {
                WalError("webpaAsyncMethodCallback: failed to serialize payload for %s\n", methodName);
                WAL_FREE(pending->rspDestination);
                WAL_FREE(pending->methodName);
                WAL_FREE(pending);
                return;
        }

        resp = (MethodAsyncResponse *) malloc(sizeof(MethodAsyncResponse));
        if(resp == NULL)
        {
                WalError("webpaAsyncMethodCallback: malloc failed\n");
                WAL_FREE(pending->rspDestination);
                WAL_FREE(pending->methodName);
                WAL_FREE(pending);
                WAL_FREE(resultPayload);
                return;
        }
        resp->methodName     = pending->methodName;
        resp->rspDestination = pending->rspDestination;
        resp->resultPayload  = resultPayload;
        free(pending); /* fields ownership transferred to resp */

        enqueueMethodResponse(resp);
}

/*----------------------------------------------------------------------------*/
/*                      Async response consumer thread                        */
/*----------------------------------------------------------------------------*/

static void *methodAsyncResponseConsumer(void *arg)
{
        (void) arg;
        pthread_detach(pthread_self());
        WalInfo("methodAsyncResponseConsumer thread started\n");

        while(1)
        {
                MethodAsyncResponse *resp = dequeueMethodResponse();
                if(resp == NULL) continue;

                /* Validate before dispatching */
                if(resp->rspDestination == NULL || resp->rspDestination[0] == '\0')
                {
                        WalError("methodAsyncResponseConsumer: missing rspDestination for %s\n",
                                 resp->methodName ? resp->methodName : "<null>");
                        WAL_FREE(resp->methodName);
                        WAL_FREE(resp->rspDestination);
                        WAL_FREE(resp->resultPayload);
                        WAL_FREE(resp);
                        continue;
                }
                if(resp->resultPayload == NULL || resp->resultPayload[0] == '\0')
                {
                        WalError("methodAsyncResponseConsumer: missing resultPayload for %s\n",
                                 resp->methodName ? resp->methodName : "<null>");
                        WAL_FREE(resp->methodName);
                        WAL_FREE(resp->rspDestination);
                        WAL_FREE(resp->resultPayload);
                        WAL_FREE(resp);
                        continue;
                }

                WalInfo("methodAsyncResponseConsumer: dispatching response for %s -> %s\n",
                        resp->methodName, resp->rspDestination);

                notifyCB cb = (notifyCB) getNotifyCB();
                if(cb == NULL)
                {
                        WalError("methodAsyncResponseConsumer: notifyCB not registered, dropping response for %s\n",
                                 resp->methodName);
                        WAL_FREE(resp->methodName);
                        WAL_FREE(resp->rspDestination);
                        WAL_FREE(resp->resultPayload);
                        WAL_FREE(resp);
                        continue;
                }

                NotifyData *notifyData = (NotifyData *) malloc(sizeof(NotifyData));
                if(notifyData == NULL)
                {
                        WalError("methodAsyncResponseConsumer: malloc failed for NotifyData\n");
                        WAL_FREE(resp->methodName);
                        WAL_FREE(resp->rspDestination);
                        WAL_FREE(resp->resultPayload);
                        WAL_FREE(resp);
                        continue;
                }
                memset(notifyData, 0, sizeof(NotifyData));
                notifyData->type         = METHOD_ASYNC_RESPONSE;
                notifyData->u.methodResp = resp;

                cb(notifyData);
        }
        return NULL;
}

/*----------------------------------------------------------------------------*/
/*                      handleAsyncMethodInvoke                               */
/*----------------------------------------------------------------------------*/

static int handleAsyncMethodInvoke(const char *methodName, const char *rspDestination,
                                    rbusObject_t inParams, res_struct *resObj,
                                    char **errorObj)
{
        rbusError_t rc;
        char *b64ack = NULL;

        /* Allocate correlation context; rbus delivers it back in the callback. */
        PendingAsyncReq *pending = (PendingAsyncReq *) malloc(sizeof(PendingAsyncReq));
        if(pending == NULL)
        {
                WalError("handleAsyncMethodInvoke: malloc failed\n");
                *errorObj = buildErrorObject(METHOD_ERR_INTERNAL, "Memory allocation failed");
                return -1;
        }
        pending->methodName     = strdup(methodName);
        pending->rspDestination = rspDestination ? strdup(rspDestination) : NULL;

        if(pending->methodName == NULL || (rspDestination != NULL && pending->rspDestination == NULL))
        {
                WalError("handleAsyncMethodInvoke: strdup failed for %s\n", methodName);
                WAL_FREE(pending->methodName);
                WAL_FREE(pending->rspDestination);
                WAL_FREE(pending);
                *errorObj = buildErrorObject(METHOD_ERR_INTERNAL, "Memory allocation failed");
                return -1;
        }

        /* Pass METHOD_ASYNC_TIMEOUT_SEC as rbus's own transport timeout; if the
         * provider crashes/deadlocks, rbus itself calls back with RBUS_ERROR_TIMEOUT. */
        rc = webpaRbusMethodInvokeAsync(methodName, inParams, webpaAsyncMethodCallback,
                                         METHOD_ASYNC_TIMEOUT_SEC, pending);
        if(rc != RBUS_ERROR_SUCCESS)
        {
                WAL_FREE(pending->methodName);
                WAL_FREE(pending->rspDestination);
                WAL_FREE(pending);

                int         code   = mapRbusErrorToMethodError(rc);
                const char *detail = rbusError_ToString(rc);
                char        msg[256] = {'\0'};
                snprintf(msg, sizeof(msg), "RBUS async invoke error: %s",
                         (detail != NULL && detail[0] != '\0') ? detail : "unknown");
                WalError("handleAsyncMethodInvoke: %s for %s\n", msg, methodName);
                *errorObj = buildErrorObject(code, msg);
                return -1;
        }

        /* ACK: base64({"rspDestination":"<dest>"}) — echoes caller's destination back */
        char ackJson[512] = {'\0'};
        snprintf(ackJson, sizeof(ackJson), "{\"rspDestination\":\"%s\"}",
                 rspDestination ? rspDestination : "");
        b64ack = base64Encode(ackJson);

        if(resObj != NULL)
        {
                resObj->reqType  = METHOD;
                resObj->paramCnt = 1;

                if(resObj->retStatus == NULL)
                        resObj->retStatus = (WDMP_STATUS *) malloc(sizeof(WDMP_STATUS));
                if(resObj->retStatus != NULL)
                        resObj->retStatus[0] = WDMP_SUCCESS;

                if(resObj->u.paramRes == NULL)
                {
                        resObj->u.paramRes = (param_res_t *) malloc(sizeof(param_res_t));
                        if(resObj->u.paramRes != NULL)
                                memset(resObj->u.paramRes, 0, sizeof(param_res_t));
                }
                if(resObj->u.paramRes != NULL && resObj->u.paramRes->params == NULL)
                {
                        resObj->u.paramRes->params = (param_t *) malloc(sizeof(param_t));
                        if(resObj->u.paramRes->params != NULL)
                                memset(resObj->u.paramRes->params, 0, sizeof(param_t));
                }
                if(resObj->u.paramRes != NULL && resObj->u.paramRes->params != NULL)
                {
                        resObj->u.paramRes->params[0].name  = strdup(methodName);
                        resObj->u.paramRes->params[0].value = b64ack;
                        resObj->u.paramRes->params[0].type  = WDMP_BASE64;
                        b64ack = NULL; /* ownership transferred */
                }
        }
        WAL_FREE(b64ack);

        WalInfo("handleAsyncMethodInvoke: ACK sent for async method %s -> %s\n",
                methodName, rspDestination);
        return 0;
}

/*----------------------------------------------------------------------------*/
/*                         Public init function                               */
/*----------------------------------------------------------------------------*/

void initMethodAsyncThread(void)
{
        pthread_t tid;
        int err = pthread_create(&tid, NULL, methodAsyncResponseConsumer, NULL);
        if(err != 0)
                WalError("initMethodAsyncThread: consumer thread failed: %s\n", strerror(err));
        else
                WalInfo("initMethodAsyncThread: methodAsyncResponseConsumer started\n");
}

/*----------------------------------------------------------------------------*/
/*                             Internal functions                             */
/*----------------------------------------------------------------------------*/

/**
 * @brief base64Decode decodes a Base64 string into a NUL-terminated buffer.
 *
 * @param[in]  in     Base64-encoded NUL-terminated string
 * @param[out] outLen number of decoded bytes
 * @return newly-allocated decoded buffer (caller frees) or NULL on failure
 */
static char *base64Decode(const char *in, size_t *outLen)
{
        size_t inLen = 0;
        size_t decSize = 0;
        size_t n = 0;
        uint8_t *out = NULL;

        if(in == NULL || in[0] == '\0')
        {
                return NULL;
        }
        inLen = strlen(in);
        decSize = b64_get_decoded_buffer_size(inLen);
        out = (uint8_t *) malloc(decSize + 1);
        if(out == NULL)
        {
                return NULL;
        }
        n = b64_decode((const uint8_t *) in, inLen, out);
        if(n == 0)
        {
                free(out);
                return NULL;
        }
        out[n] = '\0';
        if(outLen != NULL)
        {
                *outLen = n;
        }
        return (char *) out;
}

/**
 * @brief base64Encode Base64-encodes a NUL-terminated string.
 *
 * @param[in] in NUL-terminated input string
 * @return newly-allocated Base64 string (caller frees) or NULL on failure
 */
static char *base64Encode(const char *in)
{
        size_t inLen = 0;
        size_t encSize = 0;
        char *out = NULL;

        if(in == NULL)
        {
                return NULL;
        }
        inLen = strlen(in);
        encSize = b64_get_encoded_buffer_size(inLen);
        out = (char *) malloc(encSize + 1);
        if(out == NULL)
        {
                return NULL;
        }
        b64_encode((const uint8_t *) in, inLen, (uint8_t *) out);
        out[encSize] = '\0';
        return out;
}

/**
 * @brief buildErrorObject builds a { "error": { "code", "data" } } cJSON object.
 */
static char *buildErrorObject(int code, const char *data)
{
        cJSON *root = cJSON_CreateObject();
        cJSON *err = cJSON_CreateObject();
        char *out = NULL;

        cJSON_AddNumberToObject(err, "code", code);
        if(data != NULL)
        {
                cJSON_AddStringToObject(err, "data", data);
        }
        cJSON_AddItemToObject(root, "error", err);
        out = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        return out;
}

static char *buildErrorObjectFromJson(int code, cJSON *dataObj)
{
        cJSON *root = cJSON_CreateObject();
        cJSON *err = cJSON_CreateObject();
        char *out = NULL;

        cJSON_AddNumberToObject(err, "code", code);
        if(dataObj != NULL)
        {
                cJSON_AddItemToObject(err, "data", cJSON_Duplicate(dataObj, 1));
        }
        cJSON_AddItemToObject(root, "error", err);
        out = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        return out;
}

/**
 * @brief buildMethodResponse builds the method response payload
 *        { "statusCode", "parameters": [ { "name", "message" } ] } where
 *        message is the Base64-encoded result/error object.
 *
 * @param[in] name       invoked method name (or RDK.Operate if unknown)
 * @param[in] statusCode METHOD_STATUS_OK (200) on success, METHOD_STATUS_FAILURE (520) on failure
 * @param[in] messageObj result/error cJSON object (not consumed)
 * @return newly-allocated payload string (caller frees)
 */
static char *buildMethodResponse(const char *name, int statusCode, cJSON *messageObj)
{
        char *messageStr = NULL;
        char *b64message = NULL;
        char *payload = NULL;
        cJSON *root = NULL;
        cJSON *paramsArr = NULL;
        cJSON *entry = NULL;

        WalInfo("buildMethodResponse: name='%s', statusCode=%d\n",
                name != NULL ? name : RDK_OPERATE_PARAM, statusCode);

        if(messageObj != NULL)
        {
                messageStr = cJSON_PrintUnformatted(messageObj);
                if(messageStr == NULL)
                {
                        WalError("buildMethodResponse: failed to serialize message object\n");
                }
                else
                {
                        WalInfo("buildMethodResponse: message JSON: %s\n", messageStr);
                }
        }
        if(messageStr != NULL)
        {
                b64message = base64Encode(messageStr);
                if(b64message == NULL)
                {
                        WalError("buildMethodResponse: failed to base64-encode message\n");
                }
                else
                {
                        WalInfo("buildMethodResponse: message base64: %s\n", b64message);
                }
                free(messageStr);
        }

        root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "statusCode", statusCode);
        paramsArr = cJSON_CreateArray();
        entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", name != NULL ? name : RDK_OPERATE_PARAM);
        cJSON_AddStringToObject(entry, "message", b64message != NULL ? b64message : "");
        cJSON_AddItemToArray(paramsArr, entry);
        cJSON_AddItemToObject(root, "parameters", paramsArr);

        payload = cJSON_PrintUnformatted(root);
        if(payload == NULL)
        {
                WalError("buildMethodResponse: failed to serialize response payload\n");
        }
        else
        {
                WalInfo("buildMethodResponse: payload generated (%zu bytes)\n", strlen(payload));
                WalInfo("buildMethodResponse: payload: %s\n", payload);
        }
        cJSON_Delete(root);
        WAL_FREE(b64message);
        return payload;
}

/**
 * @brief wdmpToRbusType maps a WDMP DATA_TYPE to the closest rbusValueType_t.
 */
static rbusValueType_t wdmpToRbusType(int wdmpType)
{
        switch(wdmpType)
        {
                case WDMP_INT:     return RBUS_INT32;
                case WDMP_UINT:    return RBUS_UINT32;
                case WDMP_BOOLEAN: return RBUS_BOOLEAN;
                case WDMP_LONG:    return RBUS_INT64;
                case WDMP_ULONG:   return RBUS_UINT64;
                case WDMP_FLOAT:   return RBUS_SINGLE;
                case WDMP_DOUBLE:  return RBUS_DOUBLE;
                case WDMP_DATETIME:return RBUS_DATETIME;
                case WDMP_BASE64:  return RBUS_BYTES;
                case WDMP_BYTE:    return RBUS_BYTES;
                case WDMP_NONE:    return RBUS_NONE;
                case WDMP_STRING:
                default:           return RBUS_STRING;
        }
}

static int mapRbusErrorToMethodError(rbusError_t rc)
{
        switch(rc)
        {
                case RBUS_ERROR_DESTINATION_NOT_FOUND:
                        return METHOD_ERR_METHOD_NOT_FOUND;
                case RBUS_ERROR_INVALID_INPUT:
                        return METHOD_ERR_INVALID_PARAMS;
                default:
                        return METHOD_ERR_INTERNAL;
        }
}

/**
 * @brief jsonScalarToRbusValue converts an untyped JSON scalar to an rbusValue.
 */
static int jsonScalarToRbusValue(cJSON *val, rbusValue_t *out)
{
        rbusValue_Init(out);
        if(cJSON_IsString(val))
        {
                rbusValue_SetString(*out, val->valuestring != NULL ? val->valuestring : "");
        }
        else if(cJSON_IsBool(val))
        {
                rbusValue_SetBoolean(*out, cJSON_IsTrue(val) ? true : false);
        }
        else if(cJSON_IsNumber(val))
        {
                if(val->valuedouble == (double) val->valueint)
                {
                        rbusValue_SetInt32(*out, val->valueint);
                }
                else
                {
                        rbusValue_SetDouble(*out, val->valuedouble);
                }
        }
        else if(cJSON_IsNull(val))
        {
                rbusValue_SetString(*out, "");
        }
        else
        {
                rbusValue_Release(*out);
                *out = NULL;
                return -1;
        }
        return 0;
}

/**
 * @brief jsonLeafToRbusValue converts a typed JSON leaf { "value", "dataType" }
 *        into an rbusValue of the mapped type.
 */
static int jsonLeafToRbusValue(cJSON *val, int wdmpType, rbusValue_t *out)
{
        rbusValueType_t rt = wdmpToRbusType(wdmpType);
        const char *str = NULL;

        if(cJSON_IsString(val))
        {
                str = val->valuestring != NULL ? val->valuestring : "";
        }
        else
        {
                /* The operate payload always encodes parameter values as JSON strings
                 * (e.g. "value": "<string>"), regardless of the target dataType.
                 * If the value is not a string, it is invalid; return -1 for error. */
                return -1;
        }

        rbusValue_Init(out);
        if(!rbusValue_SetFromString(*out, rt, str))
        {
                /* rbusValue_SetFromString failed to parse the string into the
                 * requested rbus type; release and return -1 for error. */
                rbusValue_Release(*out);
                *out = NULL;
                return -1;
        }
        return 0;
}

/**
 * @brief jsonObjectToRbus converts a JSON params object into an rbusObject,
 *        mapping typed leaves and nested objects recursively.
 *
 * @return 0 on success, -1 on failure
 */
static int jsonObjectToRbus(cJSON *jsonObj, rbusObject_t rbusObj)
{
        cJSON *child = NULL;

        for(child = jsonObj->child; child != NULL; child = child->next)
        {
                const char *key = child->string;
                rbusValue_t rv = NULL;

                if(key == NULL)
                {
                        return -1;
                }

                if(cJSON_IsObject(child))
                {
                        cJSON *dt = cJSON_GetObjectItem(child, "dataType");
                        cJSON *leafVal = cJSON_GetObjectItem(child, "value");

                        if(dt != NULL && cJSON_IsNumber(dt) && leafVal != NULL)
                        {
                                /* Typed leaf: { "value": ..., "dataType": <n> }. */
                                if(jsonLeafToRbusValue(leafVal, dt->valueint, &rv) != 0)
                                {
                                        return -1;
                                }
                                rbusObject_SetValue(rbusObj, key, rv);
                                rbusValue_Release(rv);
                        }
                        else
                        {
                                /* Nested params object. */
                                rbusObject_t nested = NULL;
                                rbusObject_Init(&nested, NULL);
                                if(jsonObjectToRbus(child, nested) != 0)
                                {
                                        rbusObject_Release(nested);
                                        return -1;
                                }
                                rbusValue_Init(&rv);
                                rbusValue_SetObject(rv, nested);
                                rbusObject_SetValue(rbusObj, key, rv);
                                rbusValue_Release(rv);
                                rbusObject_Release(nested);
                        }
                }
                else
                {
                        /* Untyped scalar leaf. */
                        if(jsonScalarToRbusValue(child, &rv) != 0)
                        {
                                return -1;
                        }
                        rbusObject_SetValue(rbusObj, key, rv);
                        rbusValue_Release(rv);
                }
        }
        return 0;
}

/**
 * @brief rbusValueToJson converts a single rbusValue into a JSON scalar
 *        or nested object for RBUS_OBJECT.
 *
 * @return newly-allocated cJSON node or NULL on failure
 */
static cJSON *rbusValueToJson(rbusValue_t val)
{
        rbusValueType_t rt = rbusValue_GetType(val);
        cJSON *leaf = NULL;

        if(rt == RBUS_OBJECT)
        {
                rbusObject_t child = rbusValue_GetObject(val);
                cJSON *nested = cJSON_CreateObject();
                if(child != NULL && rbusObjectToJson(child, nested) != 0)
                {
                        cJSON_Delete(nested);
                        return NULL;
                }
                return nested;
        }

        switch(rt)
        {
                case RBUS_BOOLEAN:
                        leaf = cJSON_CreateBool(rbusValue_GetBoolean(val));
                        break;
                case RBUS_INT8:
                        leaf = cJSON_CreateNumber(rbusValue_GetInt8(val));
                        break;
                case RBUS_UINT8:
                        leaf = cJSON_CreateNumber(rbusValue_GetUInt8(val));
                        break;
                case RBUS_INT16:
                        leaf = cJSON_CreateNumber(rbusValue_GetInt16(val));
                        break;
                case RBUS_UINT16:
                        leaf = cJSON_CreateNumber(rbusValue_GetUInt16(val));
                        break;
                case RBUS_INT32:
                        leaf = cJSON_CreateNumber(rbusValue_GetInt32(val));
                        break;
                case RBUS_UINT32:
                        leaf = cJSON_CreateNumber((double) rbusValue_GetUInt32(val));
                        break;
                case RBUS_INT64:
                        leaf = cJSON_CreateNumber((double) rbusValue_GetInt64(val));
                        break;
                case RBUS_UINT64:
                        leaf = cJSON_CreateNumber((double) rbusValue_GetUInt64(val));
                        break;
                case RBUS_SINGLE:
                        leaf = cJSON_CreateNumber(rbusValue_GetSingle(val));
                        break;
                case RBUS_DOUBLE:
                        leaf = cJSON_CreateNumber(rbusValue_GetDouble(val));
                        break;
                case RBUS_STRING:
                {
                        int len = 0;
                        const char *s = rbusValue_GetString(val, &len);
                        if(s != NULL && (s[0] == '{' || s[0] == '[' || s[0] == '"'))
                        {
                                cJSON *parsed = cJSON_Parse(s);
                                if(parsed != NULL)
                                {
                                        leaf = parsed;
                                        break;
                                }
                        }
                        leaf = cJSON_CreateString(s != NULL ? s : "");
                        break;
                }
                default:
                {
                        char buf[512] = {'\0'};
                        rbusValue_ToString(val, buf, sizeof(buf));
                        leaf = cJSON_CreateString(buf);
                        break;
                }
        }
        return leaf;
}

/**
 * @brief rbusObjectToJson converts an rbusObject's properties into JSON.
 *
 * @return 0 on success, -1 on failure
 */
static int rbusObjectToJson(rbusObject_t obj, cJSON *jsonOut)
{
        if(obj == NULL || jsonOut == NULL)
        {
                WalError("rbusObjectToJson: invalid input (obj=%p, jsonOut=%p)\n", obj, jsonOut);
                return -1;
        }

        WalPrint("rbusObjectToJson: start converting RBUS object to JSON\n");
        rbusProperty_t prop = rbusObject_GetProperties(obj);

        if(prop == NULL)
        {
                WalInfo("rbusObjectToJson: RBUS object has no properties\n");
        }

        while(prop != NULL)
        {
                const char *name = rbusProperty_GetName(prop);
                rbusValue_t val = rbusProperty_GetValue(prop);

                if(name != NULL && val != NULL)
                {
                        WalPrint("rbusObjectToJson: converting property '%s'\n", name);
                        cJSON *leaf = rbusValueToJson(val);
                        if(leaf == NULL)
                        {
                                WalError("rbusObjectToJson: failed converting property '%s'\n", name);
                                return -1;
                        }
                        cJSON_AddItemToObject(jsonOut, name, leaf);
                }
                else
                {
                        WalError("rbusObjectToJson: skipping property with missing name/value (name=%s, value=%p)\n",
                                name != NULL ? name : "<null>", val);
                }                             
                prop = rbusProperty_GetNext(prop);
        }
        WalPrint("rbusObjectToJson: conversion completed successfully\n");
        return 0;
}
#if 0
static void dumpRbusObjectWithTimestamp(const char *path, const char *label,
                const char *responseName, rbusObject_t obj)
{
        FILE *tmpOut = NULL;
        time_t now = 0;
        struct tm localTm;
        char timestamp[32] = {'\0'};

        if(path == NULL || label == NULL || responseName == NULL || obj == NULL)
        {
                return;
        }

        tmpOut = fopen(path, "a+");
        if(tmpOut == NULL)
        {
                WalError("Failed to open %s for writing %s\n", path, label);
                return;
        }

        now = time(NULL);
        if(now != (time_t) -1 && localtime_r(&now, &localTm) != NULL &&
                strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &localTm) > 0)
        {
                fprintf(tmpOut, "[%s] method=%s %s\n", timestamp, responseName, label);
        }
        else
        {
                fprintf(tmpOut, "[timestamp unavailable] method=%s %s\n", responseName, label);
        }

        rbusObject_fwrite(obj, 1, tmpOut);
        fputc('\n', tmpOut);
        fclose(tmpOut);

        if(timestamp[0] != '\0')
        {
                WalPrint("Dumped method %s to %s at %s\n", label, path, timestamp);
        }
        else
        {
                WalPrint("Dumped method %s to %s\n", label, path);
        }
}
#endif
