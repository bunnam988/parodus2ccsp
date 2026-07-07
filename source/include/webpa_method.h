/**
 * @file webpa_method.h
 *
 * @description This header defines the WebPA cloud-to-CPE RPC method
 *              invocation path triggered by the reserved RDK.Operate
 *              PATCH/SET parameter.
 *
 * Copyright (c) 2015  Comcast
 */

#ifndef _WEBPA_METHOD_H_
#define _WEBPA_METHOD_H_

#include <stdbool.h>
#include <wdmp-c.h>

/**
 * @brief Reserved WebPA parameter name that signals a method invocation.
 */
#define RDK_OPERATE_PARAM                  "RDK.Operate"

/* JSON-RPC style error codes carried inside the Base64-encoded message. */
#define METHOD_ERR_PARSE                   (-32700)
#define METHOD_ERR_INVALID_REQUEST         (-32600)
#define METHOD_ERR_METHOD_NOT_FOUND        (-32601)
#define METHOD_ERR_INVALID_PARAMS          (-32602)
#define METHOD_ERR_INTERNAL                (-32603)

/* Method response statusCode values. */
#define METHOD_STATUS_SUCCESS              200
#define METHOD_STATUS_FAILURE              520

/**
 * @brief isMethodInvokeRequest detects whether a SET request is actually a
 *        cloud method-invocation request identified by the reserved
 *        RDK.Operate parameter name.
 *
 * @param[in] setReq parsed SET request
 * @return true if the request carries the RDK.Operate parameter
 */
bool isMethodInvokeRequest(set_req_t *setReq);

/**
 * @brief handleMethodInvoke decodes the RDK.Operate operate payload, invokes
 *        the target RBUS method synchronously and forms the method response
 *        payload returned to the cloud.
 *
 * @param[in]  setReq     parsed SET request carrying the RDK.Operate parameter
 * @param[out] resPayload newly-allocated response payload (caller frees)
 */
void handleMethodInvoke(set_req_t *setReq, char **resPayload);

#endif
