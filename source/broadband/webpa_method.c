/**
 * @file webpa_method.c
 *
 * @description Implements the WebPA cloud-to-CPE RPC method invocation path.
 *              A method request arrives as an ordinary WebPA PATCH/SET carrying
 *              a single reserved parameter named RDK.Operate (dataType 5,
 *              WDMP_BASE64) whose value is a Base64-encoded UTF-8 JSON operate
 *              payload: { "method": "...", "params": { ... },
 *              "rspDestination": "..." }. The target RDK
 *              method is invoked synchronously via rbusMethod_Invoke and the
 *              result (or error) is returned to the cloud using the method
 *              response shape.
 *
 * Copyright (c) 2015  Comcast
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include <cJSON.h>
#include <trower-base64/base64.h>

#include "webpa_method.h"
#include "webpa_adapter.h"
#include "webpa_rbus.h"

/*----------------------------------------------------------------------------*/
/*                             Function Prototypes                            */
/*----------------------------------------------------------------------------*/
static char *base64Decode(const char *in, size_t *outLen);
static char *base64Encode(const char *in);
static char *buildErrorObject(int code, const char *data);
static char *buildMethodResponse(const char *name, int statusCode, cJSON *messageObj);
static rbusValueType_t wdmpToRbusType(int wdmpType);
static int rbusToWdmpType(rbusValueType_t rt);
static int jsonScalarToRbusValue(cJSON *val, rbusValue_t *out);
static int jsonLeafToRbusValue(cJSON *val, int wdmpType, rbusValue_t *out);
static int jsonObjectToRbus(cJSON *jsonObj, rbusObject_t rbusObj);
static cJSON *rbusValueToJson(rbusValue_t val);
static int rbusObjectToJson(rbusObject_t obj, cJSON *jsonOut);
static rbusHandle_t rbus_handle;
/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/

bool isMethodInvokeRequest(set_req_t *setReq)
{
    if(setReq == NULL || setReq->param == NULL || setReq->paramCnt != 1)
        return false;

    return (setReq->param[0].name != NULL &&
            strcmp(setReq->param[0].name, RDK_OPERATE_PARAM) == 0);
}

void handleMethodInvoke(set_req_t *setReq, res_struct *resObj)
{
        const char *responseName = RDK_OPERATE_PARAM;
        char *decoded = NULL;
        char *errorObj = NULL;
        char *resultStr = NULL;
        char *b64message = NULL;
        size_t decodedLen = 0;
        cJSON *operateJson = NULL;
        cJSON *methodItem = NULL;
        cJSON *paramsItem = NULL;
        cJSON *rspDestinationItem = NULL;
        cJSON *resultObj = NULL;
        const char *rspDestination = NULL;
        rbusObject_t inParams = NULL;
        rbusObject_t outParams = NULL;
        WDMP_STATUS methodStatus = WDMP_FAILURE;
        rbusError_t rc = RBUS_ERROR_SUCCESS;
        param_t *p = NULL;

        WalInfo("************** handleMethodInvoke *****************\n");

        /* A method request carries exactly one RDK.Operate parameter. */
        if(setReq == NULL || setReq->paramCnt != 1 || setReq->param == NULL)
        {
                WalError("RDK.Operate method request must carry exactly one parameter\n");
                errorObj = buildErrorObject(METHOD_ERR_INVALID_REQUEST,
                        "RDK.Operate method request must carry exactly one parameter");
                goto respond;
        }

        p = &setReq->param[0];
        if(p->type != WDMP_BASE64)
        {
                WalError("RDK.Operate parameter must be Base64 (dataType 5)\n");
                errorObj = buildErrorObject(METHOD_ERR_INVALID_REQUEST,
                        "RDK.Operate parameter must be Base64 (dataType 5)");
                goto respond;
        }
        if(p->value == NULL)
        {
                WalError("RDK.Operate parameter value is missing\n");
                errorObj = buildErrorObject(METHOD_ERR_INVALID_REQUEST,
                        "RDK.Operate parameter value is missing");
                goto respond;
        }

        /* Base64-decode the operate payload. */
        decoded = base64Decode(p->value, &decodedLen);
        if(decoded == NULL)
        {
                WalError("Failed to Base64-decode operate payload\n");
                errorObj = buildErrorObject(METHOD_ERR_PARSE,
                        "Failed to Base64-decode operate payload");
                goto respond;
        }
        WalInfo("Decoded operate payload (%zu bytes): %s\n", decodedLen, decoded);

        /* Parse the decoded operate payload JSON. */
        operateJson = cJSON_Parse(decoded);
        if(operateJson == NULL)
        {
                WalError("Operate payload is not valid JSON\n");
                errorObj = buildErrorObject(METHOD_ERR_PARSE,
                        "Operate payload is not valid JSON");
                goto respond;
        }

        /* Extract the mandatory method name. */
        methodItem = cJSON_GetObjectItem(operateJson, "method");
        if(methodItem == NULL || !cJSON_IsString(methodItem) ||
                methodItem->valuestring == NULL || methodItem->valuestring[0] == '\0')
        {
                WalError("Operate payload is missing the method name\n");
                errorObj = buildErrorObject(METHOD_ERR_INVALID_REQUEST,
                        "Operate payload is missing the method name");
                goto respond;
        }
        responseName = methodItem->valuestring;
        WalInfo("Method invocation target: %s\n", responseName);

        /* Convert the optional params object into an RBUS input object. */
        rbusObject_Init(&inParams, NULL);
        paramsItem = cJSON_GetObjectItem(operateJson, "params");
        if(paramsItem != NULL && cJSON_IsObject(paramsItem))
        {
                if(jsonObjectToRbus(paramsItem, inParams) != 0)
                {
                        WalError("Failed to convert params to RBUS input object\n");
                        errorObj = buildErrorObject(METHOD_ERR_INTERNAL,
                                "Failed to convert params to RBUS input object");
                        goto respond;
                }
        }

        /* Optional rspDestination controls invocation mode. When provided,
         * the request is asynchronous and not supported in Phase 1. */
        rspDestinationItem = cJSON_GetObjectItem(operateJson, "rspDestination");
        if(rspDestinationItem != NULL)
        {
                if(!cJSON_IsString(rspDestinationItem) || rspDestinationItem->valuestring == NULL)
                {
                        WalError("Operate payload has invalid rspDestination\n");
                        errorObj = buildErrorObject(METHOD_ERR_INVALID_REQUEST,
                                "Operate payload has invalid rspDestination");
                        goto respond;
                }
                rspDestination = rspDestinationItem->valuestring;
        }

        if(rspDestination != NULL && rspDestination[0] != '\0')
        {
                WalError("received async request, and is not supported (rspDestination '%s')\n",
                        rspDestination);
                errorObj = buildErrorObject(METHOD_ERR_INVALID_REQUEST,
                        "The request is not valid / properly formatted");
                goto respond;
        }

        /* Invoke the method synchronously (blocking) and return the result
         * directly in the response payload carried by the HTTP response. */
        //rc = webpaRbusMethodInvoke(responseName, inParams, &outParams);
        rc = rbusMethod_Invoke(get_rbus_handle(), responseName, inParams, &outParams);
                //for debugging purpose, dump the inParams to a file
                if(inParams != NULL)
                {
                        FILE *tmpOut = fopen("/tmp/webpa_method_inParams.txt", "a+");
                        if(tmpOut != NULL)
                        {
                                rbusObject_fwrite(inParams, 1, tmpOut);
                                fclose(tmpOut);
                                WalInfo("Dumped method inParams to /tmp/webpa_method_inParams.txt\n");
                        }
                        else
                        {
                                WalError("Failed to open /tmp/webpa_method_inParams.txt for writing inParams\n");
                        }
                }
                //for debugging purpose, dump the outParams to a file
                if(outParams != NULL)
                {
                        FILE *tmpOut = fopen("/tmp/webpa_method_outParams.txt", "a+");
                        if(tmpOut != NULL)
                        {
                                rbusObject_fwrite(outParams, 1, tmpOut);
                                fclose(tmpOut);
                                WalInfo("Dumped method outParams to /tmp/webpa_method_outParams.txt\n");
                        }
                        else
                        {
                                WalError("Failed to open /tmp/webpa_method_outParams.txt for writing outParams\n");
                        }
                }        
        if(rc != RBUS_ERROR_SUCCESS)
        {
                char detail[256] = {'\0'};
                int code = METHOD_ERR_INTERNAL;
                WalError("rbusMethod_Invoke failed for method '%s' with error code %d\n", responseName, rc);

                if(rc == RBUS_ERROR_DESTINATION_NOT_FOUND)
                {
                        code = METHOD_ERR_METHOD_NOT_FOUND;
                        snprintf(detail, sizeof(detail),
                                "No RBUS provider for method %s", responseName);
                }
                else if(rc == RBUS_ERROR_INVALID_INPUT)
                {
                        code = METHOD_ERR_INVALID_PARAMS;
                        snprintf(detail, sizeof(detail),
                                "Provider rejected input (RBUS_ERROR_INVALID_INPUT)");
                }
                else
                {
                        code = METHOD_ERR_INTERNAL;
                        snprintf(detail, sizeof(detail),
                                "RBUS invocation failed with error code %d", rc);
                }
                WalError("%s\n", detail);
                errorObj = buildErrorObject(code, detail);
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
                WalInfo("Method %s returned no RBUS output parameters\n", responseName);
        }

        resultStr = cJSON_PrintUnformatted(resultObj);
        if(resultStr == NULL)
        {
                WalError("Failed to serialize method result JSON\n");
                errorObj = buildErrorObject(METHOD_ERR_INTERNAL,
                        "Failed to serialize method result JSON");
                goto respond;
        }

        methodStatus = WDMP_SUCCESS;
        WalInfo("Method %s invoked successfully\n", responseName);

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
                        if(resObj->u.paramRes->params[0].name != NULL)
                        {
                                free(resObj->u.paramRes->params[0].name);
                        }
                        if(resObj->u.paramRes->params[0].value != NULL)
                        {
                                free(resObj->u.paramRes->params[0].value);
                        }
                        resObj->u.paramRes->params[0].name = strdup(responseName);
                        if(errorObj != NULL)
                        {
                                WalInfo("Encoding method error payload for %s: %s\n", responseName, errorObj);
                                b64message = base64Encode(errorObj);
                                if(b64message != NULL)
                                {
                                        resObj->u.paramRes->params[0].value = strdup(b64message);
                                        free(b64message);
                                        b64message = NULL;
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
                                WalInfo("Encoding method result payload for %s: %s\n", responseName, resultStr);
                                b64message = base64Encode(resultStr);
                                if(b64message != NULL)
                                {
                                        resObj->u.paramRes->params[0].value = strdup(b64message);
                                        free(b64message);
                                        b64message = NULL;
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

        if(errorObj != NULL)
        {
                free(errorObj);
        }
        if(operateJson != NULL)
        {
                cJSON_Delete(operateJson);
        }
        if(decoded != NULL)
        {
                free(decoded);
        }
        if(resultStr != NULL)
        {
                free(resultStr);
        }
        if(resultObj != NULL)
        {
                cJSON_Delete(resultObj);
        }
        if(b64message != NULL)
        {
                free(b64message);
        }
        if(inParams != NULL)
        {
                rbusObject_Release(inParams);
        }
        if(outParams != NULL)
        {
                rbusObject_Release(outParams);
        }
        WalInfo("************** handleMethodInvoke *****************\n");
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

/**
 * @brief buildMethodResponse builds the method response payload
 *        { "statusCode", "parameters": [ { "name", "message" } ] } where
 *        message is the Base64-encoded result/error object.
 *
 * @param[in] name       invoked method name (or RDK.Operate if unknown)
 * @param[in] statusCode WDMP_SUCCESS on success, WDMP_FAILURE on failure
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
        if(b64message != NULL)
        {
                free(b64message);
        }
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
                case WDMP_STRING:
                case WDMP_DATETIME:
                case WDMP_BASE64:
                case WDMP_BYTE:
                default:           return RBUS_STRING;
        }
}

/**
 * @brief rbusToWdmpType maps an rbusValueType_t back to a WDMP DATA_TYPE.
 */
static int rbusToWdmpType(rbusValueType_t rt)
{
        switch(rt)
        {
                case RBUS_BOOLEAN: return WDMP_BOOLEAN;
                case RBUS_INT8:
                case RBUS_INT16:
                case RBUS_INT32:   return WDMP_INT;
                case RBUS_BYTE:
                case RBUS_UINT8:
                case RBUS_UINT16:
                case RBUS_UINT32:  return WDMP_UINT;
                case RBUS_INT64:   return WDMP_LONG;
                case RBUS_UINT64:  return WDMP_ULONG;
                case RBUS_SINGLE:  return WDMP_FLOAT;
                case RBUS_DOUBLE:  return WDMP_DOUBLE;
                case RBUS_STRING:
                default:           return WDMP_STRING;
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
        char numbuf[64] = {'\0'};
        const char *str = NULL;

        if(cJSON_IsString(val))
        {
                str = val->valuestring != NULL ? val->valuestring : "";
        }
        else if(cJSON_IsNumber(val))
        {
                if(val->valuedouble == (double) val->valueint)
                {
                        snprintf(numbuf, sizeof(numbuf), "%d", val->valueint);
                }
                else
                {
                        snprintf(numbuf, sizeof(numbuf), "%g", val->valuedouble);
                }
                str = numbuf;
        }
        else if(cJSON_IsBool(val))
        {
                str = cJSON_IsTrue(val) ? "true" : "false";
        }
        else if(cJSON_IsNull(val))
        {
                str = "";
        }
        else
        {
                return -1;
        }

        rbusValue_Init(out);
        if(!rbusValue_SetFromString(*out, rt, str))
        {
                /* Fall back to a plain string if coercion fails. */
                rbusValue_SetString(*out, str);
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
 * @brief rbusValueToJson converts a single rbusValue into a JSON leaf
 *        { "value", "dataType" } or a nested object for RBUS_OBJECT.
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

        leaf = cJSON_CreateObject();
        cJSON_AddNumberToObject(leaf, "dataType", rbusToWdmpType(rt));

        switch(rt)
        {
                case RBUS_BOOLEAN:
                        cJSON_AddBoolToObject(leaf, "value", rbusValue_GetBoolean(val));
                        break;
                case RBUS_INT8:
                        cJSON_AddNumberToObject(leaf, "value", rbusValue_GetInt8(val));
                        break;
                case RBUS_UINT8:
                        cJSON_AddNumberToObject(leaf, "value", rbusValue_GetUInt8(val));
                        break;
                case RBUS_INT16:
                        cJSON_AddNumberToObject(leaf, "value", rbusValue_GetInt16(val));
                        break;
                case RBUS_UINT16:
                        cJSON_AddNumberToObject(leaf, "value", rbusValue_GetUInt16(val));
                        break;
                case RBUS_INT32:
                        cJSON_AddNumberToObject(leaf, "value", rbusValue_GetInt32(val));
                        break;
                case RBUS_UINT32:
                        cJSON_AddNumberToObject(leaf, "value", (double) rbusValue_GetUInt32(val));
                        break;
                case RBUS_INT64:
                        cJSON_AddNumberToObject(leaf, "value", (double) rbusValue_GetInt64(val));
                        break;
                case RBUS_UINT64:
                        cJSON_AddNumberToObject(leaf, "value", (double) rbusValue_GetUInt64(val));
                        break;
                case RBUS_SINGLE:
                        cJSON_AddNumberToObject(leaf, "value", rbusValue_GetSingle(val));
                        break;
                case RBUS_DOUBLE:
                        cJSON_AddNumberToObject(leaf, "value", rbusValue_GetDouble(val));
                        break;
                case RBUS_STRING:
                {
                        int len = 0;
                        const char *s = rbusValue_GetString(val, &len);
                        cJSON_AddStringToObject(leaf, "value", s != NULL ? s : "");
                        break;
                }
                default:
                {
                        char buf[512] = {'\0'};
                        rbusValue_ToString(val, buf, sizeof(buf));
                        cJSON_AddStringToObject(leaf, "value", buf);
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
        int i = 0;
        rbusValue_t value = NULL;
        rbusValueType_t type = RBUS_NONE;
        char *str_value = NULL;
        if(obj == NULL || jsonOut == NULL)
        {
                WalError("rbusObjectToJson: invalid input (obj=%p, jsonOut=%p)\n", obj, jsonOut);
                return -1;
        }

        WalInfo("rbusObjectToJson: start converting RBUS object to JSON\n");
        rbusProperty_t prop = rbusObject_GetProperties(obj);

        if(prop == NULL)
        {
                WalInfo("rbusObjectToJson: RBUS object has no properties\n");
        }

        while(prop != NULL)
        {
                // const char *name = rbusProperty_GetName(prop);
                // rbusValue_t val = rbusProperty_GetValue(prop);

                // if(name != NULL && val != NULL)
                // {
                //         WalInfo("rbusObjectToJson: converting property '%s'\n", name);
                //         cJSON *leaf = rbusValueToJson(val);
                //         if(leaf == NULL)
                //         {
                //                 WalError("rbusObjectToJson: failed converting property '%s'\n", name);
                //                 return -1;
                //         }
                //         cJSON_AddItemToObject(jsonOut, name, leaf);
                // }
                // else
                // {
                //         WalInfo("rbusObjectToJson: skipping property with missing name/value (name=%s, value=%p)\n",
                //                 name != NULL ? name : "<null>", val);
                // }
                value = rbusProperty_GetValue(prop);
                if(value)
                {
                        type = rbusValue_GetType(value);
                        str_value = rbusValue_ToString(value,NULL,0);

                        if(str_value)
                        {
                                WalInfo ("Parameter %2d:\r\n", ++i);
                                WalInfo ("              Name  : %s\r\n", rbusProperty_GetName(prop));
                                //WalInfo ("              Type  : %s\r\n", getDataType_toString(type));
                                WalInfo ("              Value : %s\r\n", str_value);
                                cJSON *leaf = rbusValueToJson(value);
                                if(leaf == NULL)
                                {
                                        WalError("rbusObjectToJson: failed converting property '%s'\n", rbusProperty_GetName(prop));
                                        return -1;
                                }
                                cJSON_AddStringToObject(jsonOut, rbusProperty_GetName(prop), leaf);
                                free(str_value);
                        }
                }
                else
                {
                        WalInfo("rbusObjectToJson: skipping property with missing value (value=%p)\n", value);
                }                                
                prop = rbusProperty_GetNext(prop);
        }
        WalInfo("rbusObjectToJson: conversion completed successfully\n");
        return 0;
}
