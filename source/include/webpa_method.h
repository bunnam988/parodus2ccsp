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

/**
 * @brief isMethodInvokeRequest detects whether a SET request should be routed
 *        to method-invocation handling.
 *
 * The current check is strict about shape and identifier only: the request
 * must contain exactly one parameter and that parameter name must be
 * RDK.Operate.
 *
 * @param[in] setReq parsed SET request
 * @return true when setReq has exactly one parameter named RDK.Operate
 */
bool isMethodInvokeRequest(set_req_t *setReq);

/**
 * @brief handleMethodInvoke decodes the RDK.Operate operate payload, invokes
 *        the target RBUS method synchronously and fills the WDMP response
 *        structure consumed by wdmp_form_response.
 *
 * @param[in]  setReq parsed SET request carrying the RDK.Operate parameter
 * @param[out] resObj response structure to be populated
 */
void handleMethodInvoke(set_req_t *setReq, res_struct *resObj);

#endif
