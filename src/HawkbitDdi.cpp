/**

   @file HawkbitDdi.cpp
   @date 06.04.2019
   @author Sven Ebenfeld

   Copyright (c) 2019 Sven Ebenfeld. All rights reserved.
   This file is part of the ESP32 Hawkbit Updater Arduino Library.

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

#include "HawkbitDdi.h"
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Update.h>

// Allocate JsonBuffer for biggest possible JSON document in DDI API
// Use arduinojson.org/assistant to compute the capacity.
const size_t capacity = JSON_ARRAY_SIZE(1) + 3 * JSON_ARRAY_SIZE(2) + JSON_ARRAY_SIZE(3) + JSON_ARRAY_SIZE(5) +
                        24 * JSON_OBJECT_SIZE(1) + 8 * JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(3) +
                        15 * JSON_OBJECT_SIZE(4) + JSON_OBJECT_SIZE(5) + 4970;
static DynamicJsonDocument jsonBuffer(capacity);
#define HEADERSIZE 1024
static char headers[HEADERSIZE];

typedef struct str_href {
  char href_server[64];
  int16_t href_port;
  char href_url[512];
} t_href;

t_href href_param;

const char *HawkbitDdi::securityTypeString[HB_SEC_MAX] = {
  [HB_SEC_CLIENTCERTIFICATE] = NULL,
  [HB_SEC_GATEWAYTOKEN] = "GatewayToken",
  [HB_SEC_TARGETTOKEN] = "TargetToken",
  [HB_SEC_NONE] = NULL
};

const char *HawkbitDdi::executionStatusString[HB_EX_MAX] = {
  [HB_EX_CANCELED] = "canceled", // If cancellation has been requested and was successful
  [HB_EX_REJECTED] = "rejected", // If update will not be installed at this time
  [HB_EX_CLOSED] = "closed", // If update has been finished in success or failure state
  [HB_EX_PROCEEDING] = "proceeding", // During download/check/installation/verification
  [HB_EX_SCHEDULED] = "scheduled", // If update will be scheduled
  [HB_EX_RESUMED] = "resumed" // If update has been resumed after scheduling
};

const char *HawkbitDdi::executionResultString[HB_RES_MAX] = {
  [HB_RES_NONE] = "none", // If action is not in closed state, yet
  [HB_RES_SUCCESS] = "success", // If action was completed successfully
  [HB_RES_FAILURE] = "failure" // If action was completed with error
};

const char *HawkbitDdi::configDataModeString[HB_CONFIGDATA_MAX] = {
  [HB_CONFIGDATA_MERGE] = "merge", // If action is not in closed state, yet
  [HB_CONFIGDATA_REPLACE] = "replace", // If action was completed successfully
  [HB_CONFIGDATA_REMOVE] = "remove" // If action was completed with error
};

const char *HawkbitDdi::deploymentModeString[HB_DEPLOYMENT_MAX] = {
  [HB_DEPLOYMENT_NONE] = NULL, // Unknown deployment mode
  [HB_DEPLOYMENT_SKIP] = "skip", // do not update, yet
  [HB_DEPLOYMENT_ATTEMPT] = "attempt", // server asks to update
  [HB_DEPLOYMENT_FORCE] = "forced" // server requests immediate update
};

/* Static definitions for GET requests to use in printf functions */
const char *HawkbitDdi::_getRequest = "GET %s HTTP/1.1\r\n";
const char *HawkbitDdi::_getRootController = "GET /%s/controller/v1/%s HTTP/1.1\r\n";
const char *HawkbitDdi::_putConfigData = "PUT /%s/controller/v1/%s/configData HTTP/1.1\r\n";
const char *HawkbitDdi::_postDeploymentBaseFeedback = "POST /%s/controller/v1/%s/deploymentBase/%d/feedback HTTP/1.1\r\n";

static void splitHref(char *href_string);

HawkbitDdi::HawkbitDdi() {
  
}

HawkbitDdi::HawkbitDdi(String serverName, uint16_t serverPort, String tenantId, String controllerId, String securityToken, HB_SECURITY_TYPE securityType) {
  this->_serverName = serverName;
  this->_serverPort = serverPort;
  this->_tenantId = tenantId;
  this->_controllerId = controllerId;
  this->_securityToken = securityToken;
  this->_securityType = securityType;
}

HawkbitDdi::~HawkbitDdi(void) {
}

void HawkbitDdi::begin(WiFiClientSecure client) {
  this->_client = client;
  this->_currentExecutionStatus = HB_EX_CLOSED;
  this->_currentExecutionResult = HB_RES_NONE;
  this->pollController();
  this->putConfigData(HB_CONFIGDATA_REPLACE);
  this->work();
}

int HawkbitDdi::work() {
  int retStatus = -1;
  if (millis() > this->_nextPoll) {
    this->pollController();
    if (strnlen(this->_putConfigDataHref, sizeof(this->_putConfigDataHref)) > 0) {
      Serial.println("Need to put config data");
      this->putConfigData();
    }
    if (strnlen(this->_getDeploymentBaseHref, sizeof(this->_getDeploymentBaseHref)) > 0 && this->_currentActionId <= 0) {
      Serial.println("Need to get Deployment Base");
      this->getDeploymentBase();
    }
    if (strnlen(this->_getCancelActionHref, sizeof(this->_getCancelActionHref)) > 0) {
      Serial.println("Need to get Cancel Action Information");
      this->getCancelAction();
    }
  }
  if (this->_currentActionId > 0) {
    switch (this->_currentExecutionStatus) {
      case HB_EX_PROCEEDING:
        this->getAndInstallUpdateImage();
        //this->_currentExecutionStatus = HB_EX_CLOSED;
        //this->_currentExecutionResult = HB_RES_SUCCESS;
        //this->_jobFeedbackChanged = true;
        break;
      case HB_EX_SCHEDULED:
        if (millis() > this->_jobSchedule) {
          this->_currentExecutionStatus = HB_EX_PROCEEDING;
          this->_currentExecutionResult = HB_RES_NONE;
          this->_jobFeedbackChanged = true;
        }
        break;
      case HB_EX_CANCELED:
      case HB_EX_CLOSED:
        break;
      default:
        this->_currentExecutionStatus = HB_EX_PROCEEDING;
        this->_currentExecutionResult = HB_RES_NONE;
        this->_jobFeedbackChanged = true;
        break;
    }
    if (this->_jobFeedbackChanged) {
      if (this->_currentExecutionStatus == HB_EX_CANCELED) {
        this->postCancelFeedback();
      } else {
        this->postDeploymentBaseFeedback();
      }
      this->_jobFeedbackChanged = false;
    }
    if (this->_currentExecutionStatus == HB_EX_CLOSED) {
      this->_currentActionId = 0;
      ESP.restart();
    }
  }
  return retStatus;
}

char * HawkbitDdi::createHeaders() {
  return this->createHeaders(this->_serverName.c_str());
}

char * HawkbitDdi::createHeaders(const char *serverName) {
  return this->createHeaders(serverName, "application/hal+json");
}

char * HawkbitDdi::createHeaders(const char *serverName, const char *acceptType) {
  size_t strsize = 0;
  snprintf(headers, HEADERSIZE, "Host: %s\r\n", serverName);
  strsize = strnlen(headers, HEADERSIZE);
  switch (this->_securityType) {
    case HB_SEC_GATEWAYTOKEN:
    case HB_SEC_TARGETTOKEN:
      /* Add Authorization Header */
      snprintf(headers + strsize, HEADERSIZE - strsize, "Authorization: %s %s\r\n", HawkbitDdi::securityTypeString[this->_securityType], this->_securityToken.c_str());
      strsize = strnlen(headers, HEADERSIZE);
      break;
    default:
      /* No Authorization Header needed */
      break;
  }
  if (acceptType != NULL && strlen(acceptType) > 0) {
    snprintf(headers + strsize, HEADERSIZE - strsize, "Accept: %s\r\n", acceptType);
    strsize = strnlen(headers, HEADERSIZE);
  }
  snprintf(headers + strsize, HEADERSIZE - strsize, "Connection: close\r\n");
  strsize = strnlen(headers, HEADERSIZE);
  return headers;
}

static void splitHref(char *href_string) {
  Serial.println("Split Href");
  uint8_t partNo = 0;
  char *endPtr;
  size_t curLen;
  memset(&href_param, 0, sizeof(href_param));
  // Read each time pair
  char* component = strtok(href_string, "/");
  while (component != NULL)
  {
    Serial.println(component);
    switch (partNo) {
      case 0:
      default:
        // Find the next command in input string
        component = strtok(NULL, ":/");
        break;
      case 1:
        strncpy(href_param.href_server, component, sizeof(href_param.href_server));
        // Find the next command in input string
        component = strtok(NULL, "/");
        break;
      case 2:
        href_param.href_port = strtol(component, &endPtr, 10);
        if (endPtr != NULL && *endPtr != '\0') {
          href_param.href_port = 443;
          href_param.href_url[0] = '/';
          strncpy(&href_param.href_url[1], component, sizeof(href_param.href_url) - 1);
        }
        // Find the next command in input string
        component = strtok(NULL, "");
        break;
      case 3:
        curLen = strnlen(href_param.href_url, sizeof(href_param.href_url));
        href_param.href_url[curLen] = '/';
        strncpy(&href_param.href_url[curLen + 1], component, sizeof(href_param.href_url) - curLen - 1);
        // Find the next command in input string
        component = strtok(NULL, "");
        break;
    }
    partNo++;
  }
}

HB_DEPLOYMENT_MODE HawkbitDdi::parseDeploymentMode(const char *deploymentmode) {
  HB_DEPLOYMENT_MODE returnMode = HB_DEPLOYMENT_NONE;
  if (deploymentmode == NULL) {
    return returnMode;
  }
  for (int i = 1; i < HB_DEPLOYMENT_MAX; i++) {
    if (strncmp(HawkbitDdi::deploymentModeString[i], deploymentmode, 8) == 0) {
      returnMode = (HB_DEPLOYMENT_MODE)i;
      break;
    }
  }
  return returnMode;
}

void HawkbitDdi::getAndInstallUpdateImage() {
  splitHref(this->_getSoftwareModuleHref);
  this->_getSoftwareModuleHref[0] = '\0';
  Serial.printf("Server: %s:%d, GET %s\r\n", href_param.href_server, href_param.href_port, href_param.href_url);
  Serial.println("\nStarting connection to server...");
  if (!_client.connect(href_param.href_server, href_param.href_port)) {
    Serial.println("Connection failed!");
  } else {
    Serial.println("Connected to server!");
    // Make a HTTP request:
    _client.printf(HawkbitDdi::_getRequest, href_param.href_url);
    _client.print(this->createHeaders(href_param.href_server, "application/octet-stream"));
    // Close Headers field
    _client.println();

    while (_client.connected()) {
      String line = _client.readStringUntil('\n');
      Serial.println(line);
      if (line == "\r") {
        Serial.println("headers received");
        break;
      }
    }
    Update.begin(this->_updateSize, U_FLASH);
    Serial.printf("%d Bytes written\r\n", Update.writeStream(_client));
    if (Update.end()) {
      Serial.println("OTA done!");
      if (Update.isFinished()) {
        this->_currentExecutionStatus = HB_EX_CLOSED;
        this->_currentExecutionResult = HB_RES_SUCCESS;
        this->_jobFeedbackChanged = true;
        Serial.println("Update successfully completed. Rebooting.");
      }
      else {
        this->_currentExecutionStatus = HB_EX_CLOSED;
        this->_currentExecutionResult = HB_RES_FAILURE;
        this->_jobFeedbackChanged = true;
        Serial.println("Update not finished? Something went wrong!");
      }
    }
    else {
      this->_currentExecutionStatus = HB_EX_CLOSED;
      this->_currentExecutionResult = HB_RES_FAILURE;
      this->_jobFeedbackChanged = true;
      Serial.println("Error Occurred. Error #: " + String(Update.getError()));
    }
    _client.stop();
  }
}

void HawkbitDdi::getDeploymentBase() {
  splitHref(this->_getDeploymentBaseHref);
  this->_getDeploymentBaseHref[0] = '\0';
  Serial.printf("Server: %s:%d, GET %s\r\n", href_param.href_server, href_param.href_port, href_param.href_url);
  Serial.println("\nStarting connection to server...");
  if (!_client.connect(href_param.href_server, href_param.href_port)) {
    Serial.println("Connection failed!");
  } else {
    Serial.println("Connected to server!");
    // Make a HTTP request:
    _client.printf(HawkbitDdi::_getRequest, href_param.href_url);
    _client.print(this->createHeaders(href_param.href_server));
    // Close Headers field
    _client.println();

    while (_client.connected()) {
      String line = _client.readStringUntil('\n');
      Serial.println(line);
      if (line == "\r") {
        Serial.println("headers received");
        break;
      }
    }
    auto error = deserializeJson(jsonBuffer, _client);
    if (error) {
      Serial.print(F("deserializeJson() failed with code "));
      Serial.println(error.c_str());
      _client.stop();
      return;
    }

    // Extract values
    Serial.println(F("Response:"));
    serializeJsonPretty(jsonBuffer, Serial);
    Serial.println();
    _client.stop();
    Serial.println("Storing Values");
    this->_currentActionId = atoi(jsonBuffer["id"].as<char *>());
    Serial.printf("Current Action ID: %d\r\n", this->_currentActionId);
    /* Only look for the deployment update mode as we don't want to split download and update on ESP32 */
    this->_currentDeploymentMode = HawkbitDdi::parseDeploymentMode(jsonBuffer["deployment"]["update"].as<char *>());
    Serial.printf("Deployment Mode: %s", HawkbitDdi::deploymentModeString[this->_currentDeploymentMode]);
    if (this->_currentExecutionStatus == HB_EX_CLOSED) {
      if (this->_currentDeploymentMode == HB_DEPLOYMENT_FORCE) {
        /* Immediately start downloading and updating */
        this->_currentExecutionStatus = HB_EX_PROCEEDING;
        this->_currentExecutionResult = HB_RES_NONE;
        this->_jobFeedbackChanged = true;
        this->_jobSchedule = millis();
      } else if (this->_currentDeploymentMode == HB_DEPLOYMENT_ATTEMPT) {
        /* Schedule downloading and updating in 10 minute */
        this->_currentExecutionStatus = HB_EX_SCHEDULED;
        this->_currentExecutionResult = HB_RES_NONE;
        this->_jobSchedule = millis() + 15000UL;
        this->_jobFeedbackChanged = true;
      }
    }
    Serial.println("Storing Chunks");
    /* We only support one chunk with one artifact for now. */
    JsonArray chunks = jsonBuffer["deployment"]["chunks"].as<JsonArray>();
    if (!chunks.isNull() && chunks.size() >= 1) {
      Serial.println("Storing Artifacts");
      JsonArray artifacts = chunks[0]["artifacts"].as<JsonArray>();
      if (!artifacts.isNull() && artifacts.size() >= 1) {
        this->_updateSize = artifacts[0]["size"].as<unsigned long>();
        strncpy(this->_getSoftwareModuleHref, artifacts[0]["_links"]["download"]["href"].as<char *>(), sizeof(this->_getSoftwareModuleHref));
        Serial.println(this->_getSoftwareModuleHref);
      }
    }
  }
  Serial.println("Deployment Base finished");
}

void HawkbitDdi::pollController() {
  char timeString[16];
  int i = 0;
  Serial.println("\nStarting connection to server...");
  if (!_client.connect(this->_serverName.c_str(), this->_serverPort))
    Serial.println("Connection failed!");
  else {
    Serial.println("Connected to server!");
    // Make a HTTP request:
    _client.printf(HawkbitDdi::_getRootController, this->_tenantId.c_str(), this->_controllerId.c_str());
    _client.print(this->createHeaders());
    // Close Headers field
    _client.println();

    while (_client.connected()) {
      String line = _client.readStringUntil('\n');
      Serial.println(line);
      if (line == "\r") {
        Serial.println("headers received");
        break;
      }
    }
    auto error = deserializeJson(jsonBuffer, _client);
    if (error) {
      Serial.print(F("deserializeJson() failed with code "));
      Serial.println(error.c_str());
      _client.stop();
      return;
    }

    // Extract values
    Serial.println(F("Response:"));
    serializeJsonPretty(jsonBuffer, Serial);
    Serial.println();
    strncpy(timeString, jsonBuffer["config"]["polling"]["sleep"].as<char*>(), sizeof(timeString));
    //  Serial.println(timeString);
    this->_pollInterval = HawkbitDdi::convertTime(timeString);
    Serial.printf("Poll Interval: %d\r\n", this->_pollInterval);
    this->_nextPoll = millis() + (this->_pollInterval > 0 ? this->_pollInterval : 300000UL);
    //this->_nextPoll = millis() + 10000UL;
    Serial.printf("Next Poll: %d\r\n", this->_nextPoll);
    Serial.println(jsonBuffer["_links"].as<char*>());
    if (jsonBuffer["_links"].isNull()) {
      this->_putConfigDataHref[0] = '\0';
      this->_getDeploymentBaseHref[0] = '\0';
    } else {
      if (!jsonBuffer["_links"]["deploymentBase"]["href"].isNull()) {
        strncpy(this->_getDeploymentBaseHref, jsonBuffer["_links"]["deploymentBase"]["href"].as<char*>(), sizeof(this->_getDeploymentBaseHref));
      } else {
        this->_getDeploymentBaseHref[0] = '\0';
      }
      if (!jsonBuffer["_links"]["configData"]["href"].isNull()) {
        strncpy(this->_putConfigDataHref, jsonBuffer["_links"]["configData"]["href"].as<char*>(), sizeof(this->_putConfigDataHref));
      } else {
        this->_putConfigDataHref[0] = '\0';
      }
      if (!jsonBuffer["_links"]["cancelAction"]["href"].isNull()) {
        strncpy(this->_getCancelActionHref, jsonBuffer["_links"]["cancelAction"]["href"].as<char*>(), sizeof(this->_getCancelActionHref));
      } else {
        this->_getCancelActionHref[0] = '\0';
      }
    }

    _client.stop();
  }
}

void HawkbitDdi::postDeploymentBaseFeedback() {
  char timeString[16];
  jsonBuffer.clear();
  jsonBuffer["id"] = String(this->_currentActionId);
  jsonBuffer["time"] = "20190511T121314";
  jsonBuffer["status"]["execution"] = HawkbitDdi::executionStatusString[this->_currentExecutionStatus];
  jsonBuffer["status"]["result"]["finished"] = HawkbitDdi::executionResultString[this->_currentExecutionResult];
  Serial.println("\nStarting connection to server...");
  if (!_client.connect(this->_serverName.c_str(), this->_serverPort))
    Serial.println("Connection failed!");
  else {
    Serial.println("Connected to server!");
    // Make a HTTP request:
    _client.printf(HawkbitDdi::_postDeploymentBaseFeedback, this->_tenantId.c_str(), this->_controllerId.c_str(), this->_currentActionId);
    _client.print(this->createHeaders());
    _client.println("Content-Type: application/json");
    _client.printf("Content-Length: %d\r\n", measureJson(jsonBuffer));
    // Close Headers field
    _client.println();
    serializeJson(jsonBuffer, _client);

    while (_client.connected()) {
      String line = _client.readStringUntil('\n');
      Serial.println(line);
      if (line == "\r") {
        Serial.println("headers received");
        break;
      }
    }
    _client.stop();
  }
}

void HawkbitDdi::getCancelAction() {
  int actionId;
  splitHref(this->_getCancelActionHref);
  this->_getCancelActionHref[0] = '\0';
  Serial.printf("Server: %s:%d, GET %s\r\n", href_param.href_server, href_param.href_port, href_param.href_url);
  Serial.println("\nStarting connection to server...");
  if (!_client.connect(href_param.href_server, href_param.href_port)) {
    Serial.println("Connection failed!");
  } else {
    Serial.println("Connected to server!");
    // Make a HTTP request:
    _client.printf(HawkbitDdi::_getRequest, href_param.href_url);
    _client.print(this->createHeaders(href_param.href_server));
    // Close Headers field
    _client.println();

    while (_client.connected()) {
      String line = _client.readStringUntil('\n');
      Serial.println(line);
      if (line == "\r") {
        Serial.println("headers received");
        break;
      }
    }
    auto error = deserializeJson(jsonBuffer, _client);
    if (error) {
      Serial.print(F("deserializeJson() failed with code "));
      Serial.println(error.c_str());
      _client.stop();
      return;
    }

    // Extract values
    Serial.println(F("Response:"));
    serializeJsonPretty(jsonBuffer, Serial);
    Serial.println();
    _client.stop();
    Serial.println("Storing Values");
    actionId = atoi(jsonBuffer["cancelAction"]["stopId"].as<char *>());
    if (this->_currentActionId == actionId) {
      Serial.printf("Canceled Action ID: %d\r\n", this->_currentActionId);
      /* Immediately start downloading and updating */
      this->_currentExecutionStatus = HB_EX_CANCELED;
      this->_currentExecutionResult = HB_RES_SUCCESS;
      this->_jobFeedbackChanged = true;
    } else {
      this->_currentActionId = actionId;
      Serial.printf("Canceled Action ID: %d\r\n", this->_currentActionId);
      /* Immediately start downloading and updating */
      this->_currentExecutionStatus = HB_EX_CANCELED;
      this->_currentExecutionResult = HB_RES_FAILURE;
      this->_jobFeedbackChanged = true;
    }
  }
  Serial.println("CancelAction finished");
}

void HawkbitDdi::postCancelFeedback() {
  char timeString[16];
  if (this->_currentExecutionStatus == HB_EX_CANCELED) {
    this->_currentExecutionStatus = HB_EX_CLOSED;
    this->_currentExecutionResult = HB_RES_SUCCESS;
  } else {
    this->_currentExecutionStatus = HB_EX_CLOSED;
    this->_currentExecutionResult = HB_RES_FAILURE;
  }
  jsonBuffer.clear();
  jsonBuffer["id"] = String(this->_currentActionId);
  jsonBuffer["time"] = "20190511T121314";
  jsonBuffer["status"]["execution"] = HawkbitDdi::executionStatusString[this->_currentExecutionStatus];
  jsonBuffer["status"]["result"]["finished"] = HawkbitDdi::executionResultString[this->_currentExecutionResult];
  Serial.println("\nStarting connection to server...");
  if (!_client.connect(this->_serverName.c_str(), this->_serverPort))
    Serial.println("Connection failed!");
  else {
    Serial.println("Connected to server!");
    // Make a HTTP request:
    _client.printf(HawkbitDdi::_postDeploymentBaseFeedback, this->_tenantId.c_str(), this->_controllerId.c_str(), this->_currentActionId);
    _client.print(this->createHeaders());
    _client.println("Content-Type: application/json");
    _client.printf("Content-Length: %d\r\n", measureJson(jsonBuffer));
    // Close Headers field
    _client.println();
    serializeJson(jsonBuffer, _client);

    while (_client.connected()) {
      String line = _client.readStringUntil('\n');
      Serial.println(line);
      if (line == "\r") {
        Serial.println("headers received");
        break;
      }
    }
    // if there are incoming bytes available
    // from the server, read them and print them:
    while (_client.available()) {
      char c = _client.read();
      Serial.write(c);
    }
    _client.stop();
  }
  this->_currentActionId = 0;
}

void HawkbitDdi::putConfigData() {
  this->putConfigData(HB_CONFIGDATA_MERGE);
}

void HawkbitDdi::putConfigData(HB_CONFIGDATA_MODE cf_mode) {
  char timeString[16];
  jsonBuffer.clear();
  jsonBuffer["id"] = String(this->_currentActionId);
  jsonBuffer["time"] = "20190511T121314";
  jsonBuffer["status"]["execution"] = HawkbitDdi::executionStatusString[this->_currentExecutionStatus];
  jsonBuffer["status"]["result"]["finished"] = HawkbitDdi::executionResultString[this->_currentExecutionResult];
  jsonBuffer["data"] = serialized(this->_configData);
  jsonBuffer["mode"] = HawkbitDdi::configDataModeString[cf_mode];
  Serial.println("\nStarting connection to server...");
  if (!_client.connect(this->_serverName.c_str(), this->_serverPort))
    Serial.println("Connection failed!");
  else {
    Serial.println("Connected to server!");
    // Make a HTTP request:
    _client.printf(HawkbitDdi::_putConfigData, this->_tenantId.c_str(), this->_controllerId.c_str());
    _client.print(this->createHeaders());
    _client.println("Content-Type: application/json");
    _client.printf("Content-Length: %d\r\n", measureJson(jsonBuffer));
    // Close Headers field
    _client.println();
    serializeJson(jsonBuffer, _client);
    serializeJsonPretty(jsonBuffer, Serial);

    while (_client.connected()) {
      String line = _client.readStringUntil('\n');
      Serial.println(line);
      if (line == "\r") {
        Serial.println("headers received");
        break;
      }
    }
    _client.stop();
  }
}

unsigned long HawkbitDdi::convertTime(char *timeString) {
  uint8_t partNo = 0;
  unsigned long milliseconds = 0;
  // Read each time pair
  char* number = strtok(timeString, ":");
  while (number != NULL)
  {
    int timeNumber = atoi(number);
    switch (partNo) {
      case 0:
        // Hours
        milliseconds += (unsigned long)timeNumber * 60UL * 60UL * 1000UL;
        break;
      case 1:
        // Minutes
        milliseconds += (unsigned long)timeNumber * 60UL * 1000UL;
        break;
      case 2:
        // Seconds
        milliseconds += (unsigned long)timeNumber * 1000UL;
        break;
      default:
        return 0UL;
    }
    // Find the next command in input string
    number = strtok(0, ":");
    partNo++;
  }
  return milliseconds;
}

unsigned long HawkbitDdi::convertTime(String timeString) {
  return HawkbitDdi::convertTime(timeString.c_str());
}
