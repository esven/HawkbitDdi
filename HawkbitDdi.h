/**

   @file HawkbitDdi.h
   @date 06.04.2019
   @author Sven Ebenfeld

   Copyright (c) 2019 Sven Ebenfeld. All rights reserved.
   This file is part of the ESP32 Hawkbit Updater.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/

#ifndef ___HAWKBIT_DDI_H___
#define ___HAWKBIT_DDI_H___

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <Update.h>

enum HB_SECURITY_TYPE {
  HB_SEC_CLIENTCERTIFICATE,
  HB_SEC_GATEWAYTOKEN,
  HB_SEC_TARGETTOKEN,
  HB_SEC_NONE,
  HB_SEC_MAX
};

enum HB_EXECUTION_STATUS {
  HB_EX_CANCELED,
  HB_EX_REJECTED,
  HB_EX_CLOSED,
  HB_EX_PROCEEDING,
  HB_EX_SCHEDULED,
  HB_EX_RESUMED,
  HB_EX_MAX
};

enum HB_EXECUTION_RESULT {
  HB_RES_NONE,
  HB_RES_SUCCESS,
  HB_RES_FAILURE,
  HB_RES_MAX
};

enum HB_CONFIGDATA_MODE {
  HB_CONFIGDATA_MERGE,
  HB_CONFIGDATA_REPLACE,
  HB_CONFIGDATA_REMOVE,
  HB_CONFIGDATA_MAX
};

enum HB_DEPLOYMENT_MODE {
  HB_DEPLOYMENT_NONE,
  HB_DEPLOYMENT_SKIP,
  HB_DEPLOYMENT_ATTEMPT,
  HB_DEPLOYMENT_FORCE,
  HB_DEPLOYMENT_MAX
};

class HawkbitDdi
{
  public:
    /* De-/Constructors */
    HawkbitDdi(String serverName, uint16_t serverPort, String tenantId, String controllerId, String securityToken, HB_SECURITY_TYPE securityType = HB_SEC_NONE);
    ~HawkbitDdi(void);

    /* Public member methods */
    void begin(WiFiClientSecure client);

    int work();

    void setConfigData(char *jsonString) {
      strncpy(this->_configData, jsonString, sizeof(this->_configData));
    }

  protected:

  private:
    /* private static member attributes */
    static const char *securityTypeString[];
    static const char *executionStatusString[];
    static const char *executionResultString[];
    static const char *configDataModeString[];
    static const char *deploymentModeString[];
    static const char *_getRequest;
    static const char *_getRootController;
    static const char *_putConfigData;
    static const char *_postDeploymentBaseFeedback;
    /* private static member methods */
    static unsigned long convertTime(char *timeString);
    static unsigned long convertTime(String timeString);

    /* private member attributes */
    char _putConfigDataHref[512];
    char _getDeploymentBaseHref[512];
    char _getCancelActionHref[512];
    char _getSoftwareModuleHref[1024];
    char _configData[512];

    unsigned long _nextPoll = 0;
    /* Default the pollInterval to 5 Minutes */
    unsigned long _pollInterval = 300000UL;
    unsigned long _jobSchedule;
    bool _jobFeedbackChanged = false;
    int _currentActionId = -1;
    unsigned long _updateSize;
    WiFiClientSecure _client;
    uint16_t _serverPort;
    String _serverName;
    String _tenantId;
    String _controllerId;
    String _securityToken;
    HB_SECURITY_TYPE _securityType;
    HB_EXECUTION_STATUS _currentExecutionStatus;
    HB_EXECUTION_RESULT _currentExecutionResult;
    HB_DEPLOYMENT_MODE _currentDeploymentMode;

    /* private member methods */
    void pollController();
    void putConfigData();
    void putConfigData(HB_CONFIGDATA_MODE cf_mode);
    void getDeploymentBase();
    void postDeploymentBaseFeedback();
    void getCancelAction();
    void postCancelFeedback();
    void getAndInstallUpdateImage();
    static HB_DEPLOYMENT_MODE parseDeploymentMode(const char *deploymentmode);
    char * createHeaders();
    char * createHeaders(const char *serverName);
    char * createHeaders(const char *serverName, const char *acceptType);

};

#if !defined(NO_GLOBAL_INSTANCES) && !defined(NO_GLOBAL_HTTPUPDATE)
extern HawkbitDdi hawkbitDdi;
#endif

#endif /* ___HAWKBIT_DDI_H___ */
