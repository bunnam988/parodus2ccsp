#ifndef _WEBPA_RBUS_H_
#define _WEBPA_RBUS_H_

#include <stdio.h>
#include <rbus/rbus.h>
#include <rbus/rbus_object.h>
#include <rbus/rbus_property.h>
#include <rbus/rbus_value.h>

#include "webpa_adapter.h"
#include <wdmp-c.h>
#include <cimplog.h>


bool isRbusEnabled();
bool isRbusInitialized();
WDMP_STATUS webpaRbusInit(const char *pComponentName);
void webpaRbus_Uninit();
rbusError_t setTraceContext(char* traceContext[]);
rbusError_t getTraceContext(char* traceContext[]);
rbusError_t clearTraceContext();

//rbusError_t webpaRbusMethodInvoke(const char *methodName, rbusObject_t inParams, rbusObject_t *outParams);
/**
 * @brief Gets the rbus_handle for lmLite.
 *
 * @return rbusHandle_t value
 */
rbusHandle_t get_rbus_handle(void);
#endif
