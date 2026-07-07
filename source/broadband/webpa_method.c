/**
 * @file webpa_method.c
 *
 * @description Implements the WebPA cloud-to-CPE RPC method invocation path.
 *              A method request arrives as an ordinary WebPA PATCH/SET carrying
 *              a single reserved parameter named RDK.Operate (dataType 5,
 *              WDMP_BASE64) whose value is a Base64-encoded UTF-8 JSON operate
 *              payload: { "method": "...", "params": { ... } }. The target RDK
 *              method is invoked synchronously via rbusMethod_Invoke and the
 *              result (or error) is returned to the cloud using the method
 *              response shape.
 *
 * Copyright (c) 2015  Comcast
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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
static cJSON *buildErrorObject(int code, const char *data);
static char *buildMethodResponse(const char *name, int statusCode, cJSON *messageObj);
static rbusValueType_t wdmpToRbusType(int wdmpType);
static int rbusToWdmpType(rbusValueType_t rt);
static int jsonScalarToRbusValue(cJSON *val, rbusValue_t *out);
static int jsonLeafToRbusValue(cJSON *val, int wdmpType, rbusValue_t *out);
static int jsonObjectToRbus(cJSON *jsonObj, rbusObject_t rbusObj);
static cJSON *rbusValueToJson(rbusValue_t val);
static int rbusObjectToJson(rbusObject_t obj, cJSON *jsonOut);

/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/

bool isMethodInvokeRequest(set_req_t *setReq)
{
        size_t i = 0;

        if(setReq == NULL || setReq->param == NULL)
        {
                return false;
        }
        for(i = 0; i < setReq->paramCnt; i++)
        {
                if(setReq->param[i].name != NULL &&
                        strcmp(setReq->param[i].name, RDK_OPERATE_PARAM) == 0)
                {
                        return true;
                }
        }
        return false;
}

void handleMethodInvoke(set_req_t *setReq, char **resPayload)
{
        const char *responseName = RDK_OPERATE_PARAM;
        char *decoded = NULL;
        size_t decodedLen = 0;
        cJSON *operateJson = NULL;
        cJSON *methodItem = NULL;
        cJSON *paramsItem = NULL;
        cJSON *resultObj = NULL;
        cJSON *messageObj = NULL;
        rbusObject_t inParams = NULL;
        rbusObject_t outParams = NULL;
        int statusCode = METHOD_STATUS_FAILURE;
        rbusError_t rc = RBUS_ERROR_SUCCESS;
        param_t *p = NULL;

        WalInfo("************** handleMethodInvoke *****************\n");

        /* A method request carries exactly one RDK.Operate parameter. */
        if(setReq == NULL || setReq->paramCnt != 1 || setReq->param == NULL)
        {
                WalError("RDK.Operate method request must carry exactly one parameter\n");
                messageObj = buildErrorObject(METHOD_ERR_INVALID_REQUEST,
                        "RDK.Operate method request must carry exactly one parameter");
                goto respond;
        }

        p = &setReq->param[0];
        if(p->type != WDMP_BASE64)
        {
                WalError("RDK.Operate parameter must be Base64 (dataType 5)\n");
                messageObj = buildErrorObject(METHOD_ERR_INVALID_REQUEST,
                        "RDK.Operate parameter must be Base64 (dataType 5)");
                goto respond;
        }
        if(p->value == NULL)
        {
                WalError("RDK.Operate parameter value is missing\n");
                messageObj = buildErrorObject(METHOD_ERR_INVALID_REQUEST,
                        "RDK.Operate parameter value is missing");
                goto respond;
        }

        /* Base64-decode the operate payload. */
        decoded = base64Decode(p->value, &decodedLen);
        if(decoded == NULL)
        {
                WalError("Failed to Base64-decode operate payload\n");
                messageObj = buildErrorObject(METHOD_ERR_PARSE,
                        "Failed to Base64-decode operate payload");
                goto respond;
        }

        /* Parse the decoded operate payload JSON. */
        operateJson = cJSON_Parse(decoded);
        if(operateJson == NULL)
        {
                WalError("Operate payload is not valid JSON\n");
                messageObj = buildErrorObject(METHOD_ERR_PARSE,
                        "Operate payload is not valid JSON");
                goto respond;
        }

        /* Extract the mandatory method name. */
        methodItem = cJSON_GetObjectItem(operateJson, "method");
        if(methodItem == NULL || !cJSON_IsString(methodItem) ||
                methodItem->valuestring == NULL || methodItem->valuestring[0] == '\0')
        {
                WalError("Operate payload is missing the method name\n");
                messageObj = buildErrorObject(METHOD_ERR_INVALID_REQUEST,
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
                        messageObj = buildErrorObject(METHOD_ERR_INTERNAL,
                                "Failed to convert params to RBUS input object");
                        goto respond;
                }
        }

        /* Phase 1 supports synchronous (blocking) invocation only, which
         * applies when rspDestination is absent. A present rspDestination
         * indicates an asynchronous request, which is not supported in Phase 1
         * and is rejected as an invalid request (-32600). */
        if(setReq->rspDestination != NULL && setReq->rspDestination[0] != '\0')
        {
                WalError("received async request, and is not supported (rspDestination '%s')\n",
                        setReq->rspDestination);
                messageObj = buildErrorObject(METHOD_ERR_INVALID_REQUEST,
                        "The request is not valid / properly formatted");
                goto respond;
        }

        /* Invoke the method synchronously (blocking) and return the result
         * directly in the response payload carried by the HTTP response. */
        rc = webpaRbusMethodInvoke(responseName, inParams, &outParams);
        if(rc != RBUS_ERROR_SUCCESS)
        {
                char detail[256] = {'\0'};
                int code = METHOD_ERR_INTERNAL;

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
                messageObj = buildErrorObject(code, detail);
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
                        messageObj = buildErrorObject(METHOD_ERR_INTERNAL,
                                "Failed to convert RBUS result object to JSON");
                        goto respond;
                }
        }
        messageObj = cJSON_CreateObject();
        cJSON_AddItemToObject(messageObj, "result", resultObj);
        resultObj = NULL;
        statusCode = METHOD_STATUS_SUCCESS;
        WalInfo("Method %s invoked successfully\n", responseName);

respond:
        *resPayload = buildMethodResponse(responseName, statusCode, messageObj);

        if(messageObj != NULL)
        {
                cJSON_Delete(messageObj);
        }
        if(operateJson != NULL)
        {
                cJSON_Delete(operateJson);
        }
        if(decoded != NULL)
        {
                free(decoded);
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
static cJSON *buildErrorObject(int code, const char *data)
{
        cJSON *root = cJSON_CreateObject();
        cJSON *err = cJSON_CreateObject();

        cJSON_AddNumberToObject(err, "code", code);
        if(data != NULL)
        {
                cJSON_AddStringToObject(err, "data", data);
        }
        cJSON_AddItemToObject(root, "error", err);
        return root;
}

/**
 * @brief buildMethodResponse builds the method response payload
 *        { "statusCode", "parameters": [ { "name", "message" } ] } where
 *        message is the Base64-encoded result/error object.
 *
 * @param[in] name       invoked method name (or RDK.Operate if unknown)
 * @param[in] statusCode 200 on success, 520 on failure
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

        if(messageObj != NULL)
        {
                messageStr = cJSON_PrintUnformatted(messageObj);
        }
        if(messageStr != NULL)
        {
                b64message = base64Encode(messageStr);
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
        rbusProperty_t prop = rbusObject_GetProperties(obj);

        while(prop != NULL)
        {
                const char *name = rbusProperty_GetName(prop);
                rbusValue_t val = rbusProperty_GetValue(prop);

                if(name != NULL && val != NULL)
                {
                        cJSON *leaf = rbusValueToJson(val);
                        if(leaf == NULL)
                        {
                                return -1;
                        }
                        cJSON_AddItemToObject(jsonOut, name, leaf);
                }
                prop = rbusProperty_GetNext(prop);
        }
        return 0;
}
