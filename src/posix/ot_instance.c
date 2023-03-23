/*
 *  Copyright (c) 2018, The OpenThread Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

#include "openthread-posix-config.h"

#include <openthread/config.h>

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include <openthread/cli.h>
#include <openthread/diag.h>
#include <openthread/logging.h>
#include <openthread/tasklet.h>
#include <openthread/thread.h>
#include <openthread/platform/radio.h>

#include <common/code_utils.hpp>
#include <lib/platform/exit_code.h>
#include <openthread/openthread-system.h>
#include <openthread/platform/misc.h>

#define MULTIPLE_INSTANCE_MAX 10
#define MAX_TTY_LEN 100
#define MAX_RADIO_URL_LEN 200
#define MAX_INTERFACE_LEN 100

static otInstance *gInstance = NULL;
static pthread_t   gThreadId;
pthread_mutex_t    gLock;
bool               gTerminate = 0;

typedef struct Param
{
    char     comPort[MAX_TTY_LEN];
    uint16_t debugLevel;
} Param;

typedef struct PosixConfig
{
    otPlatformConfig mPlatformConfig;    ///< Platform configuration.
    otLogLevel       mLogLevel;          ///< Debug level of logging.
    bool             mPrintRadioVersion; ///< Whether to print radio firmware version.
    bool             mIsVerbose;         ///< Whether to print log to stderr.
} PosixConfig;

static otInstance *InitInstance(PosixConfig *aConfig)
{
    otInstance *instance = NULL;

    syslog(LOG_INFO, "Running %s", otGetVersionString());
    syslog(LOG_INFO, "Thread version: %hu", otThreadGetVersion());
    IgnoreError(otLoggingSetLevel(aConfig->mLogLevel));

    instance = otSysInit(&aConfig->mPlatformConfig);
    VerifyOrDie(instance != NULL, OT_EXIT_FAILURE);
    syslog(LOG_INFO, "Thread interface: %s", otSysGetThreadNetifName());

    if (aConfig->mPrintRadioVersion)
    {
        printf("%s\n", otPlatRadioGetVersionString(instance));
    }
    else
    {
        syslog(LOG_INFO, "RCP version: %s", otPlatRadioGetVersionString(instance));
    }

    if (aConfig->mPlatformConfig.mDryRun)
    {
        exit(OT_EXIT_SUCCESS);
    }

    return instance;
}

void otPlatReset(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);

    otSysDeinit();

    assert(false);
}

static bool getRadioURL(char *comPort)
{
    struct stat st;
    char        radioDevice[MAX_RADIO_URL_LEN] = {0};

    /* For multiple devices and intefaces,
     * we will align the index of them.
     */
    sprintf(radioDevice, "/dev/%s", comPort);
    if (stat(radioDevice, &st) == 0)
    {
        syslog(LOG_INFO, "radio device found [%s]", radioDevice);
        return true;
    }
    return false;
}

static int getInterface(void)
{
    struct stat st;
    int         i                        = 0;
    char        iface[MAX_INTERFACE_LEN] = {0};

    for (i = 0; i < MULTIPLE_INSTANCE_MAX; i++)
    {
        sprintf(iface, "/sys/class/net/wpan%d", i);
        if (stat(iface, &st) == 0)
        {
            syslog(LOG_INFO, "Interface is already used [%s]", iface);
            continue;
        }
        else
        {
            syslog(LOG_INFO, "find empty interface [%s]", iface);
            break;
        }
    }
    if (i == MULTIPLE_INSTANCE_MAX)
    {
        syslog(LOG_CRIT, "Interface reached max count...Not able to create new inteface");
        return -1;
    }

    return i;
}

void otCreateInstance(void *arg)
{
    pthread_mutex_lock(&gLock);
    PosixConfig config;
    int         interfaceIdx                = 0;
    char        radioUrl[MAX_RADIO_URL_LEN] = {0};
    char        iface[MAX_INTERFACE_LEN]    = {0};
    Param      *initParam                   = (Param *)arg;

    memset(&config, 0, sizeof(config));

    syslog(LOG_INFO, "otCreateInstance");

    interfaceIdx = getInterface();
    sprintf(iface, "wpan%d", interfaceIdx);
    syslog(LOG_INFO, "interface found [%s]", iface);

    if (!getRadioURL(initParam->comPort))
    {
        syslog(LOG_CRIT, "radio device not found");
        return;
    }
    sprintf(radioUrl, "spinel+hdlc+uart:///dev/%s", initParam->comPort);
    syslog(LOG_INFO, "radio Url found [%s]", radioUrl);
    syslog(LOG_INFO, "debug level [%d]", initParam->debugLevel);

    config.mLogLevel                      = initParam->debugLevel;
    config.mIsVerbose                     = true;
    config.mPlatformConfig.mInterfaceName = iface;
    VerifyOrDie(config.mPlatformConfig.mRadioUrlNum < OT_ARRAY_LENGTH(config.mPlatformConfig.mRadioUrls),
                OT_EXIT_INVALID_ARGUMENTS);
    config.mPlatformConfig.mRadioUrls[config.mPlatformConfig.mRadioUrlNum++] = radioUrl;
#if defined(__linux)
    config.mPlatformConfig.mRealTimeSignal = 41;
#endif
    config.mPlatformConfig.mSpeedUpFactor = 1;

    free(initParam);
    gInstance = InitInstance(&config);
    otLogInfoPlat("ot instance create success!!!");

    pthread_mutex_unlock(&gLock);

    while (gTerminate == false)
    {
        otSysMainloopContext mainloop;

        otTaskletsProcess(gInstance);

        FD_ZERO(&mainloop.mReadFdSet);
        FD_ZERO(&mainloop.mWriteFdSet);
        FD_ZERO(&mainloop.mErrorFdSet);

        mainloop.mMaxFd           = -1;
        mainloop.mTimeout.tv_sec  = 10;
        mainloop.mTimeout.tv_usec = 0;

        otSysMainloopUpdate(gInstance, &mainloop);

        if (otSysMainloopPoll(&mainloop) >= 0)
        {
            pthread_mutex_lock(&gLock);

            otSysMainloopProcess(gInstance, &mainloop);

            pthread_mutex_unlock(&gLock);
        }
        else if (errno != EINTR)
        {
            perror("select");
            ExitNow();
        }
    }

exit:
    otSysDeinit();
    gThreadId  = 0;
    gInstance  = NULL;
    gTerminate = false;
    syslog(LOG_INFO, "terminate thread mainloop : exit");
    return;
}

void *otThreadMainLoop(void *arg)
{
    syslog(LOG_INFO, "Inside otThreadMainLoop");

    otCreateInstance(arg);

    return NULL;
}

void otGetInstance(otInstance **instance, pthread_t *instanceId, const char *comPort, uint16_t debug)
{
    Param *initParam = (Param *)malloc(sizeof(Param));
    strcpy(initParam->comPort, comPort);
    initParam->debugLevel = debug;

    syslog(LOG_INFO, "otGetInstance");

    if (gInstance)
    {
        syslog(LOG_INFO, "ot instance already initialised!!!");
        *instance  = gInstance;
        instanceId = &gThreadId;
        return;
    }

    if (pthread_mutex_init(&gLock, NULL) != 0)
    {
        syslog(LOG_INFO, "mutex init has failed");
        return;
    }

    syslog(LOG_INFO, "Before otThread");
    pthread_create(&gThreadId, NULL, otThreadMainLoop, initParam);

    syslog(LOG_INFO, "Wait for 1 Seconds to initialise openthread stack !!!");
    sleep(1);

    pthread_mutex_lock(&gLock);

    *instance   = gInstance;
    *instanceId = gThreadId;
#if defined(__linux)
    otLogInfoPlat("After otThread : gThreadId[%ld]", gThreadId);
#endif

    pthread_mutex_unlock(&gLock);

    return;
}

void otWait(void)
{
    otLogInfoPlat("otWait");
    pthread_join(gThreadId, NULL);
}

void otLock(void)
{
    otLogInfoPlat("otLock");
    pthread_mutex_lock(&gLock);
}

void otUnlock(void)
{
    otLogInfoPlat("otUnlock");
    pthread_mutex_unlock(&gLock);
}
void otDestroyInstance(void)
{
    otLogInfoPlat("otDestroyInstance");
    gTerminate = true;
    otWait();
    pthread_mutex_destroy(&gLock);
}
