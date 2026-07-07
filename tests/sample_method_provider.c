/**
 * @file sample_method_provider.c
 *
 * @description Sample RBUS method provider used to validate the WebPA
 *              RDK.Operate method-invocation path end-to-end on a device.
 *
 *              It registers a synchronous test method
 *              "Device.WebpaTest.Echo()" which echoes back the input params
 *              under an "echo" object and adds a "status" string, and a method
 *              "Device.WebpaTest.Fail()" that rejects its input so the
 *              -32602 mapping can be exercised.
 *
 *              Manual on-device validation:
 *                1. Ensure rtrouted is running.
 *                2. Run this provider:  ./sample_method_provider
 *                3. From the cloud (or a WRP test tool) send a WebPA PATCH/SET
 *                   with a single parameter:
 *                       name     = "RDK.Operate"
 *                       dataType = 5   (WDMP_BASE64)
 *                       value    = base64( {"method":"Device.WebpaTest.Echo()",
 *                                           "params":{"arg1":{"value":"hello",
 *                                                             "dataType":0}}} )
 *                4. WebPA returns a method response whose parameters[0].message
 *                   is base64( {"result":{ ... }} ) with statusCode 200.
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
#include <signal.h>
#include <unistd.h>
#include <rbus.h>

static int running = 1;

static void handleSignal(int sig)
{
        (void) sig;
        running = 0;
}

static rbusError_t methodHandler(rbusHandle_t handle, char const *methodName,
        rbusObject_t inParams, rbusObject_t outParams, rbusMethodAsyncHandle_t asyncHandle)
{
        rbusValue_t value = NULL;

        (void) handle;
        (void) asyncHandle;

        printf("sample provider: methodHandler called for %s\n", methodName);

        if(strcmp(methodName, "Device.WebpaTest.Echo()") == 0)
        {
                rbusValue_Init(&value);
                rbusValue_SetString(value, "ok");
                rbusObject_SetValue(outParams, "status", value);
                rbusValue_Release(value);

                if(inParams != NULL)
                {
                        rbusValue_t echo = NULL;
                        rbusValue_Init(&echo);
                        rbusValue_SetObject(echo, inParams);
                        rbusObject_SetValue(outParams, "echo", echo);
                        rbusValue_Release(echo);
                }
                return RBUS_ERROR_SUCCESS;
        }
        else if(strcmp(methodName, "Device.WebpaTest.Fail()") == 0)
        {
                /* Reject the input so WebPA maps this to error code -32602. */
                return RBUS_ERROR_INVALID_INPUT;
        }

        return RBUS_ERROR_BUS_ERROR;
}

int main(int argc, char *argv[])
{
        rbusHandle_t handle;
        int rc = RBUS_ERROR_SUCCESS;
        rbusDataElement_t dataElements[2] = {
                {"Device.WebpaTest.Echo()", RBUS_ELEMENT_TYPE_METHOD, {NULL, NULL, NULL, NULL, NULL, methodHandler}},
                {"Device.WebpaTest.Fail()", RBUS_ELEMENT_TYPE_METHOD, {NULL, NULL, NULL, NULL, NULL, methodHandler}}
        };

        (void) argc;
        (void) argv;

        signal(SIGINT, handleSignal);
        signal(SIGTERM, handleSignal);

        rc = rbus_open(&handle, "SampleWebpaMethodProvider");
        if(rc != RBUS_ERROR_SUCCESS)
        {
                printf("sample provider: rbus_open failed: %d\n", rc);
                return rc;
        }

        rc = rbus_regDataElements(handle, 2, dataElements);
        if(rc != RBUS_ERROR_SUCCESS)
        {
                printf("sample provider: rbus_regDataElements failed: %d\n", rc);
                rbus_close(handle);
                return rc;
        }

        printf("sample provider: registered Device.WebpaTest.Echo()/Fail(). Waiting...\n");
        while(running)
        {
                sleep(1);
        }

        rbus_unregDataElements(handle, 2, dataElements);
        rbus_close(handle);
        printf("sample provider: exit\n");
        return rc;
}
