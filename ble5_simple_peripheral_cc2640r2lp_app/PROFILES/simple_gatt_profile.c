/******************************************************************************

 @file  simple_gatt_profile.c

 @brief This file contains the Simple GATT profile sample GATT service profile
 for use with the BLE sample application.

 Group: WCS, BTS
 Target Device: cc2640r2

 ******************************************************************************
 
 Copyright (c) 2010-2021, Texas Instruments Incorporated
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions
 are met:

 *  Redistributions of source code must retain the above copyright
 notice, this list of conditions and the following disclaimer.

 *  Redistributions in binary form must reproduce the above copyright
 notice, this list of conditions and the following disclaimer in the
 documentation and/or other materials provided with the distribution.

 *  Neither the name of Texas Instruments Incorporated nor the names of
 its contributors may be used to endorse or promote products derived
 from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 ******************************************************************************
 
 
 *****************************************************************************/

/*********************************************************************
 * INCLUDES
 */
#include <string.h>
#include <icall.h>
#include "util.h"
/* This Header file contains all BLE API and icall structure definition */
#include "icall_ble_api.h"

#include "audio.h"
#include "simple_peripheral.h"
#include "simple_gatt_profile.h"

/*********************************************************************
 * MACROS
 */

/*********************************************************************
 * CONSTANTS
 */

#define SERVAPP_NUM_ATTR_SUPPORTED        1 + 4 + 3   // TODO

/*********************************************************************
 * TYPEDEFS
 */

/*********************************************************************
 * GLOBAL VARIABLES
 */
// Simple GATT Profile Service UUID: 0x9500
CONST uint8 simpleProfileServUUID[ATT_UUID_SIZE] = {
    SIMPLEPROFILE_BASE_UUID_128(SIMPLEPROFILE_SERV_UUID) };

// Characteristic 1 UUID: 0x9501
CONST uint8 simpleProfileChar1UUID[ATT_UUID_SIZE] = {
    SIMPLEPROFILE_BASE_UUID_128(SIMPLEPROFILE_CHAR1_UUID) };

CONST uint8 simpleProfileChar2UUID[ATT_UUID_SIZE] = {
    SIMPLEPROFILE_BASE_UUID_128(SIMPLEPROFILE_CHAR2_UUID) };

/*********************************************************************
 * EXTERNAL VARIABLES
 */
extern ICall_EntityID selfEntity;

/*********************************************************************
 * EXTERNAL FUNCTIONS
 */

/*********************************************************************
 * LOCAL VARIABLES
 */

/*********************************************************************
 * Profile Attributes - variables
 */

// Simple Profile Service attribute
static CONST gattAttrType_t simpleProfileService = { ATT_UUID_SIZE,
                                                     simpleProfileServUUID };

// Simple Profile Characteristic 4 Properties
static uint8 simpleProfileChar1Props = GATT_PROP_WRITE | GATT_PROP_NOTIFY;
static uint8 simpleProfileChar2Props = GATT_PROP_READ | GATT_PROP_WRITE;

// Characteristic 4 Value
static uint8 simpleProfileChar1 = 0;
uint8_t simpleProfileChar2 = 5;

// Simple Profile Characteristic 4 Configuration Each client has its own
// instantiation of the Client Characteristic Configuration. Reads of the
// Client Characteristic Configuration only shows the configuration for
// that client and writes only affect the configuration of that client.
static gattCharCfg_t *simpleProfileChar1Config;

// Simple Profile Characteristic 4 User Description
static uint8 simpleProfileChar1UserDesp[6] = "audio";
static uint8 simpleProfileChar2UserDesp[9] = "duration";

/*********************************************************************
 * Profile Attributes - Table
 */

static gattAttribute_t simpleProfileAttrTbl[SERVAPP_NUM_ATTR_SUPPORTED] = {
// 0 Simple Profile Service
    { { ATT_BT_UUID_SIZE, primaryServiceUUID }, /* type */
      GATT_PERMIT_READ, /* permissions */
      0, /* handle */
      (uint8*) &simpleProfileService /* pValue */
    },

    // 1 Characteristic 1 Declaration
    { { ATT_BT_UUID_SIZE, characterUUID },
    GATT_PERMIT_READ,
      0, &simpleProfileChar1Props },

    // 2 Characteristic 1 Value
    { { ATT_UUID_SIZE, simpleProfileChar1UUID },
    GATT_PERMIT_WRITE,
      0, &simpleProfileChar1 },

    // 3 Characteristic 1 configuration
    { { ATT_BT_UUID_SIZE, clientCharCfgUUID },
    GATT_PERMIT_READ | GATT_PERMIT_WRITE,
      0, (uint8*) &simpleProfileChar1Config },

    // 4 Characteristic 1 User Description
    { { ATT_BT_UUID_SIZE, charUserDescUUID },
    GATT_PERMIT_READ,
      0, simpleProfileChar1UserDesp },

      // 5 Characteristic 2 Declaration
      { { ATT_BT_UUID_SIZE, characterUUID },
      GATT_PERMIT_READ,
        0, &simpleProfileChar2Props },

      // 6 Characteristic 2 Value
      { { ATT_UUID_SIZE, simpleProfileChar2UUID },
      GATT_PERMIT_READ | GATT_PERMIT_WRITE,
        0, &simpleProfileChar2 },

      // 7 Characteristic 2 User Description
      { { ATT_BT_UUID_SIZE, charUserDescUUID },
      GATT_PERMIT_READ,
        0, simpleProfileChar2UserDesp },
};

gattAttribute_t *simpleProfileChar1ValueAttrHandle = &simpleProfileAttrTbl[2];
gattAttribute_t *simpleProfileChar1ConfigAttrHandle = &simpleProfileAttrTbl[3];

/*********************************************************************
 * LOCAL FUNCTIONS
 */
static bStatus_t simpleProfile_ReadAttrCB(uint16_t connHandle,
                                          gattAttribute_t *pAttr,
                                          uint8_t *pValue, uint16_t *pLen,
                                          uint16_t offset, uint16_t maxLen,
                                          uint8_t method);
static bStatus_t simpleProfile_WriteAttrCB(uint16_t connHandle,
                                           gattAttribute_t *pAttr,
                                           uint8_t *pValue, uint16_t len,
                                           uint16_t offset, uint8_t method);

/*********************************************************************
 * PROFILE CALLBACKS
 */

// Simple Profile Service Callbacks
// Note: When an operation on a characteristic requires authorization and
// pfnAuthorizeAttrCB is not defined for that characteristic's service, the
// Stack will report a status of ATT_ERR_UNLIKELY to the client.  When an
// operation on a characteristic requires authorization the Stack will call
// pfnAuthorizeAttrCB to check a client's authorization prior to calling
// pfnReadAttrCB or pfnWriteAttrCB, so no checks for authorization need to be
// made within these functions.
CONST gattServiceCBs_t simpleProfileCBs = { simpleProfile_ReadAttrCB, // Read callback function pointer
    simpleProfile_WriteAttrCB, // Write callback function pointer
    NULL                       // Authorization callback function pointer
    };

/*********************************************************************
 * PUBLIC FUNCTIONS
 */

/*********************************************************************
 * @fn      SimpleProfile_AddService
 *
 * @brief   Initializes the Simple Profile service by registering
 *          GATT attributes with the GATT server.
 *
 * @param   services - services to add. This is a bit map and can
 *                     contain more than one service.
 *
 * @return  Success or Failure
 */
bStatus_t SimpleProfile_AddService(uint32 services)
{
  uint8 status;

  // Allocate Client Characteristic Configuration table
  simpleProfileChar1Config = (gattCharCfg_t*) ICall_malloc(
      sizeof(gattCharCfg_t) *
      MAX_NUM_BLE_CONNS);
  if (simpleProfileChar1Config == NULL)
  {
    return ( bleMemAllocError);
  }

  // Initialize Client Characteristic Configuration attributes
  GATTServApp_InitCharCfg( CONNHANDLE_INVALID, simpleProfileChar1Config);

  if (services & SIMPLEPROFILE_SERVICE)
  {
    // Register GATT attribute list and CBs with GATT Server App
    status = GATTServApp_RegisterService(simpleProfileAttrTbl,
                                         GATT_NUM_ATTRS( simpleProfileAttrTbl ),
                                         GATT_MAX_ENCRYPT_KEY_SIZE,
                                         &simpleProfileCBs);
  }
  else
  {
    status = SUCCESS;
  }

  return (status);
}

/*********************************************************************
 * @fn      SimpleProfile_SetParameter
 *
 * @brief   Set a Simple Profile parameter.
 *
 * @param   param - Profile parameter ID
 * @param   len - length of data to write
 * @param   value - pointer to data to write.  This is dependent on
 *          the parameter ID and WILL be cast to the appropriate
 *          data type (example: data type of uint16 will be cast to
 *          uint16 pointer).
 *
 * @return  bStatus_t
 */
bStatus_t SimpleProfile_SetParameter(uint8 param, uint8 len, void *value)
{
  bStatus_t ret = SUCCESS;
  switch (param)
  {
  default:
    ret = INVALIDPARAMETER;
    break;
  }

  return (ret);
}

/*********************************************************************
 * @fn      SimpleProfile_GetParameter
 *
 * @brief   Get a Simple Profile parameter.
 *
 * @param   param - Profile parameter ID
 * @param   value - pointer to data to put.  This is dependent on
 *          the parameter ID and WILL be cast to the appropriate
 *          data type (example: data type of uint16 will be cast to
 *          uint16 pointer).
 *
 * @return  bStatus_t
 */
bStatus_t SimpleProfile_GetParameter(uint8 param, void *value)
{
  bStatus_t ret = SUCCESS;
  switch (param)
  {
//    case SIMPLEPROFILE_CHAR1:
//      VOID memcpy( value, simpleProfileChar1, SIMPLEPROFILE_CHAR1_LEN );
//      break;

  default:
    ret = INVALIDPARAMETER;
    break;
  }

  return (ret);
}

/*********************************************************************
 * @fn          simpleProfile_ReadAttrCB
 *
 * @brief       Read an attribute.
 *
 * @param       connHandle - connection message was received on
 * @param       pAttr - pointer to attribute
 * @param       pValue - pointer to data to be read
 * @param       pLen - length of data to be read
 * @param       offset - offset of the first octet to be read
 * @param       maxLen - maximum length of data to be read
 * @param       method - type of read message
 *
 * @return      SUCCESS, blePending or Failure
 */
static bStatus_t simpleProfile_ReadAttrCB(uint16_t connHandle,
                                          gattAttribute_t *pAttr,
                                          uint8_t *pValue, uint16_t *pLen,
                                          uint16_t offset, uint16_t maxLen,
                                          uint8_t method)
{
  bStatus_t status = SUCCESS;

  if (pAttr->type.len == ATT_UUID_SIZE)
  {
    // Get 16-bit UUID from 128-bit UUID
    uint16 uuid = BUILD_UINT16(pAttr->type.uuid[12], pAttr->type.uuid[13]);
    switch (uuid)
    {
    case SIMPLEPROFILE_CHAR2_UUID:
      *pValue = simpleProfileChar2;
      *pLen = 1;
      break;

    default:
      // Should never get here! (characteristics 3 and 4 do not have read permissions)
      *pLen = 0;
      status = ATT_ERR_ATTR_NOT_FOUND;
      break;
    }
  }
  else
  {
    *pLen = 0;
    status = ATT_ERR_INVALID_HANDLE;
  }

  return (status);
}

bool commandIsValid(uint8_t* pValue, uint16_t len)
{
  if (len == 1)
  {
    return (pValue[0] <= IMT_START_READ);
  }
  else if (len == 5)
  {
    return (pValue[0] == IMT_START_READ);
  }
  else if (len == 9)
  {
    uint32_t s, e;
    memcpy(&s, &pValue[1], 4);
    memcpy(&e, &pValue[5], 4);
    return s < e;
  }
  else
  {
    return false;
  }
}

/*********************************************************************
 * @fn      simpleProfile_WriteAttrCB
 *
 * @brief   Validate attribute data prior to a write operation
 *
 * @param   connHandle - connection message was received on
 * @param   pAttr - pointer to attribute
 * @param   pValue - pointer to data to be written
 * @param   len - length of data
 * @param   offset - offset of the first octet to be written
 * @param   method - type of write message
 *
 * @return  SUCCESS, blePending or Failure
 */
static bStatus_t simpleProfile_WriteAttrCB(uint16_t connHandle,
                                           gattAttribute_t *pAttr,
                                           uint8_t *pValue, uint16_t len,
                                           uint16_t offset, uint8_t method)
{
  bStatus_t status = SUCCESS;

  if (pAttr->type.len == ATT_BT_UUID_SIZE)
  {
    // 16-bit UUID
    uint16 uuid = BUILD_UINT16(pAttr->type.uuid[0], pAttr->type.uuid[1]);
    switch (uuid)
    {
    case GATT_CLIENT_CHAR_CFG_UUID:
      status = GATTServApp_ProcessCCCWriteReq(connHandle, pAttr, pValue, len,
                                              offset, GATT_CLIENT_CFG_NOTIFY);

      if (SUCCESS == status && pAttr == &simpleProfileAttrTbl[3])
      {
        uint16_t *ccfg = (uint16_t*) pValue;

        if (*ccfg == GATT_CLIENT_CFG_NOTIFY)
        {
          SimplePeripheral_subscribe();
          Audio_subscribe();
        }
        else if (*ccfg == GATT_CFG_NO_OPERATION)
        {
          SimplePeripheral_unsubscribe();
          Audio_unsubscribe();
        }
      }
      break;

    default:
      // Should never get here! (characteristics 2 and 4 do not have write permissions)
      status = ATT_ERR_ATTR_NOT_FOUND;
      break;
    }
  }
  else if (pAttr->type.len == ATT_UUID_SIZE)
  {
    // 128-bit UUID
    uint16 uuid = BUILD_UINT16(pAttr->type.uuid[12], pAttr->type.uuid[13]);
    switch (uuid)
    {
    case SIMPLEPROFILE_CHAR1_UUID:
      // Make sure it's not a blob operation
      if (offset == 0)
      {
        if (len == 1 || len == 5 || len == 9)
        {
          if (commandIsValid(pValue, len))
          {
            IncomingMsg_t *msg = allocIncomingMsg();
            if (msg)
            {
              memset(msg, 0, sizeof(IncomingMsg_t));
              msg->type = pValue[0];

              if (len == 1)
              {
                msg->start = 0;
                msg->end = 0xffffffff;
              }
              else if (len == 5)
              {
                memcpy(&msg->start, &pValue[1], 4);
                msg->end = 0xffffffff;
              }
              else if (len == 9)
              {
                memcpy(&msg->start, &pValue[1], 4);
                memcpy(&msg->end, &pValue[5], 4);
              }

              recvIncomingMsg(msg);
            }
            else
            {
              status = ATT_ERR_INSUFFICIENT_RESOURCES;
            }
          }
          else
          {
            status = ATT_ERR_INVALID_VALUE;
          }
        }
        else
        {
          status = ATT_ERR_INVALID_VALUE_SIZE;
        }
      }
      else
      {
        status = ATT_ERR_ATTR_NOT_LONG;
      }
      break;

    case SIMPLEPROFILE_CHAR2_UUID:
      // Make sure it's not a blob operation
      if (offset == 0)
      {
        if (len == 1)
        {
          if (*pValue == 5)
          {
            Audio_updateDuration(5);
            simpleProfileChar2 = 5;
          }
          else if (*pValue == 10)
          {
            Audio_updateDuration(10);
            simpleProfileChar2 = 10;
          }
          else if (*pValue == 15)
          {
            Audio_updateDuration(15);
            simpleProfileChar2 = 15;
          }
          else
          {
            status = ATT_ERR_INVALID_VALUE;
          }
        }
        else
        {
          status = ATT_ERR_INVALID_VALUE_SIZE;
        }
      }
      else
      {
        status = ATT_ERR_ATTR_NOT_LONG;
      }
      break;

    default:
      // Should never get here! (characteristics 2 and 4 do not have write permissions)
      status = ATT_ERR_ATTR_NOT_FOUND;
      break;
    }
  }
  else
  {
    // 128-bit UUID
    status = ATT_ERR_INVALID_HANDLE;
  }

  return (status);
}

/*********************************************************************
 *********************************************************************/
