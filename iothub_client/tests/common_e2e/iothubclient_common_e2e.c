// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#ifdef __cplusplus
#include <cstdlib>
#include <cstddef>
#include <cstring>
#else
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "umock_c/umock_c.h"
#endif

#ifdef AZIOT_LINUX
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

#include "testrunnerswitcher.h"

#include "iothub.h"
#include "iothub_device_client.h"
#include "iothub_module_client.h"
#include "iothub_device_client_ll.h"
#include "iothub_module_client_ll.h"
#include "iothub_client_options.h"
#include "iothub_message.h"
#include "iothub_messaging.h"

#include "iothub_account.h"
#include "iothubtest.h"

#include "azure_c_shared_utility/buffer_.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/shared_util_options.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/lock.h"
#include "azure_c_shared_utility/gb_rand.h"

#ifdef SET_TRUSTED_CERT_IN_SAMPLES
#include "certs.h"
#endif // SET_TRUSTED_CERT_IN_SAMPLES

#include "iothubclient_common_e2e.h"

static bool g_callbackRecv = false;
static TEST_PROTOCOL_TYPE test_protocol_type;

const char* TEST_EVENT_DATA_FMT = "{\"data\":\"%.24s\",\"id\":\"%d\"}";
const char* TEST_EVENT_DATA_FMT_SPECIAL_CHAR = "{\"#data\":\"$%.24s\",\";id\":\"*%d\"}";
const char* TEST_EVENT_DATA_FMT_LONG = "{\"data\":\"*.s\",\"id\":\"%d\"}";

#define MSG_UNIQUE_ID_STAMP_LEN 5
const char MSG_ID[] = "MessageIdForE2E";
const char MSG_ID_SPECIAL[] = "MessageIdForE2E*";
const char MSG_CORRELATION_ID[] = "MessageCorrelationIdForE2E";
const char MSG_CORRELATION_ID_SPECIAL[] = "MessageCorrelationIdFor#E2E";
const char MSG_CONTENT[] = "Message content for E2E";
const char MSG_CONTENT_SPECIAL[] = "!*'();:@&=+$,/?#[]";

#define MSG_PROP_COUNT 3
const char* MSG_PROP_KEYS[MSG_PROP_COUNT] = { "Key1", "Key2", "Key3" };
const char* MSG_PROP_VALS[MSG_PROP_COUNT] = { "Val1", "Val2", "Val3" };

const char* MSG_PROP_KEYS_SPECIAL[MSG_PROP_COUNT] = { "&ey1", "K/y2", "Ke?3"};
const char* MSG_PROP_VALS_SPECIAL[MSG_PROP_COUNT] = { "=al1", "V@l2", "Va%3" };

// Due to the way some test messages are composed, please do not set the initial
// value of g_iotHubTestId to zero (due to potential issues with strlen).
static size_t g_iotHubTestId = 1;
IOTHUB_ACCOUNT_INFO_HANDLE g_iothubAcctInfo = NULL;
E2E_TEST_OPTIONS g_e2e_test_options;

static IOTHUB_DEVICE_CLIENT_HANDLE iothub_deviceclient_handle = NULL;
static IOTHUB_MODULE_CLIENT_HANDLE iothub_moduleclient_handle = NULL;
static IOTHUB_DEVICE_CLIENT_LL_HANDLE iothub_deviceclient_ll_handle = NULL;
static IOTHUB_MODULE_CLIENT_LL_HANDLE iothub_moduleclient_ll_handle = NULL;

#define RETRY_COUNT                  4
#define RETRY_DELAY_SECONDS          60
#define IOTHUB_COUNTER_MAX           10
#define MAX_CLOUD_TRAVEL_TIME        120.0
// Wait for 5 mins (arbitrary value since 2 mins didn't work for fault injection tests)
#define MAX_CLOUD_TRAVEL_TIME_FAULT_INJECTION 300.0
// Wait for 60 seconds for the service to tell us that an event was received.
#define MAX_SERVICE_EVENT_WAIT_TIME_SECONDS 60
// When waiting for events, start listening for events that happened up to 60 seconds in the past.
#define SERVICE_EVENT_WAIT_TIME_DELTA_SECONDS 60

#define MAX_SECURITY_DEVICE_WAIT_TIME   30

// A default message size of 0 causes client_create_and_send_d2c_messages to use 
// `test_message_creation` for creating the telemetry message.
#define DEFAULT_MESSAGE_SIZE    0
#define LARGE_MESSAGE_SIZE      64536
#define LARGE_MESSAGE_COUNT     5

TEST_DEFINE_ENUM_TYPE(IOTHUB_TEST_CLIENT_RESULT, IOTHUB_TEST_CLIENT_RESULT_VALUES);
TEST_DEFINE_ENUM_TYPE(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_RESULT_VALUES);
TEST_DEFINE_ENUM_TYPE(IOTHUB_MESSAGE_RESULT, IOTHUB_MESSAGE_RESULT_VALUES);
TEST_DEFINE_ENUM_TYPE(MAP_RESULT, MAP_RESULT_VALUES);
TEST_DEFINE_ENUM_TYPE(LOCK_RESULT, LOCK_RESULT_VALUES);

typedef struct EXPECTED_SEND_DATA_TAG
{
    const char* expectedString;
    bool wasFound;
    bool dataWasRecv;
    IOTHUB_CLIENT_CONFIRMATION_RESULT result;
    LOCK_HANDLE lock;
    IOTHUB_MESSAGE_HANDLE msgHandle;
} EXPECTED_SEND_DATA;

#define MAX_SEND_DATA_BUNDLE_ITEM_COUNT 5

typedef struct SEND_DATA_BUNDLE_TAG
{
    int count;
    EXPECTED_SEND_DATA* items[MAX_SEND_DATA_BUNDLE_ITEM_COUNT];
} SEND_DATA_BUNDLE;

typedef struct EXPECTED_RECEIVE_DATA_TAG
{
    bool wasFound;
    LOCK_HANDLE lock; /*needed to protect this structure*/
    IOTHUB_MESSAGE_HANDLE msgHandle;
    char correlation_id[sizeof(MSG_CORRELATION_ID_SPECIAL) + MSG_UNIQUE_ID_STAMP_LEN];
    char message_id[sizeof(MSG_ID_SPECIAL) + MSG_UNIQUE_ID_STAMP_LEN];
} EXPECTED_RECEIVE_DATA;

typedef struct CONNECTION_STATUS_DATA_TAG
{
    bool connFaultHappened;
    bool connRestored;
    LOCK_HANDLE lock;
    IOTHUB_CLIENT_CONNECTION_STATUS currentStatus;
    IOTHUB_CLIENT_CONNECTION_STATUS_REASON currentStatusReason;
} CONNECTION_STATUS_INFO;

static CONNECTION_STATUS_INFO g_connection_status_info;

static void openCompleteCallback(void* context)
{
    LogInfo("Open completed, context: %s", (char*)context);
}

static void sendCompleteCallback(void* context, IOTHUB_MESSAGING_RESULT messagingResult)
{  
    time_t t = get_time(NULL);
    char* timeString = get_ctime(&t);

    (void) context;
    if (messagingResult == IOTHUB_MESSAGING_OK)
    {
        LogInfo("%.24s C2D Message has been sent successfully", timeString);
    }
    else
    {
        LogError("%.24s C2D send failed! messagingResult=%s", timeString, MU_ENUM_TO_STRING(IOTHUB_MESSAGING_RESULT, messagingResult));
    }
}

static int IoTHubCallback(void* context, const char* data, size_t size)
{
    int messagesReceived = 0;
    SEND_DATA_BUNDLE* d2cMessages = (SEND_DATA_BUNDLE*)context;

    for (int i = 0; i < d2cMessages->count; i++)
    {
        EXPECTED_SEND_DATA* expectedData = d2cMessages->items[i];
        if (expectedData != NULL)
        {
            if (Lock(expectedData->lock) != LOCK_OK)
            {
                ASSERT_FAIL("unable to lock");
            }
            else
            {
                if (
                    (strlen(expectedData->expectedString) == size) &&
                    (memcmp(expectedData->expectedString, data, size) == 0)
                    )
                {
                    expectedData->wasFound = true;
                }

                messagesReceived += (expectedData->wasFound ? 1 : 0);

                (void)Unlock(expectedData->lock);
            }
        }
    }

    // Returning 0 means "keep processing"
    return (messagesReceived == d2cMessages->count ? 1 : 0);
}

// Invoked when a connection status changes.  Tests poll the status in the connection_status_info to make sure expected transitions occur.
static void connection_status_callback(IOTHUB_CLIENT_CONNECTION_STATUS status, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* userContextCallback)
{
    LogInfo("connection_status_callback: status=<%d>, reason=<%s>", status, MU_ENUM_TO_STRING(IOTHUB_CLIENT_CONNECTION_STATUS_REASON, reason));

    CONNECTION_STATUS_INFO* connection_status_info = (CONNECTION_STATUS_INFO*)userContextCallback;
    ASSERT_ARE_EQUAL(LOCK_RESULT, LOCK_OK, Lock(connection_status_info->lock));
    
    if ((connection_status_info->currentStatus == IOTHUB_CLIENT_CONNECTION_AUTHENTICATED) &&
        (status == IOTHUB_CLIENT_CONNECTION_UNAUTHENTICATED))
    {
        connection_status_info->connFaultHappened = true;
    }
    if ((connection_status_info->currentStatus == IOTHUB_CLIENT_CONNECTION_UNAUTHENTICATED) &&
        (status == IOTHUB_CLIENT_CONNECTION_AUTHENTICATED))
    {
        connection_status_info->connRestored = true;
    }
    connection_status_info->currentStatus = status;
    connection_status_info->currentStatusReason = reason;

    (void)Unlock(connection_status_info->lock);
}

static void ReceiveConfirmationCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* userContextCallback)
{
    LogInfo("ReceiveConfirmationCallback invoked, result=<%s>, userContextCallback=<%p>", MU_ENUM_TO_STRING(IOTHUB_CLIENT_CONFIRMATION_RESULT, result), userContextCallback);

    EXPECTED_SEND_DATA* expectedData = (EXPECTED_SEND_DATA*)userContextCallback;
    if (expectedData != NULL)
    {
        ASSERT_ARE_EQUAL(LOCK_RESULT, LOCK_OK, Lock(expectedData->lock));
        expectedData->dataWasRecv = true;
        expectedData->result = result;
        (void)Unlock(expectedData->lock);
    }
}

static IOTHUBMESSAGE_DISPOSITION_RESULT ReceiveMessageCallback(IOTHUB_MESSAGE_HANDLE messageHandle, void* userContextCallback)
{
    EXPECTED_RECEIVE_DATA* receiveUserContext = (EXPECTED_RECEIVE_DATA*)userContextCallback;
    if (receiveUserContext == NULL)
    {
        ASSERT_FAIL("User context is NULL");
    }
    else
    {
        ASSERT_ARE_EQUAL(LOCK_RESULT, LOCK_OK, Lock(receiveUserContext->lock));

        const char* messageId;
        const char* correlationId;
        const unsigned char* content;
        size_t contentSize;

        messageId = IoTHubMessage_GetMessageId(messageHandle);
        ASSERT_IS_NOT_NULL(messageId, "messageId not received");

        if (g_e2e_test_options.use_special_chars && strncmp(messageId, MSG_ID_SPECIAL, sizeof(MSG_ID_SPECIAL) - 1) != 0)
        {
            ASSERT_FAIL("Message ID mismatch.");
        }
        else if (!g_e2e_test_options.use_special_chars && strncmp(messageId, MSG_ID, sizeof(MSG_ID) - 1) != 0)
        {
            ASSERT_FAIL("Message ID mismatch.");
        }

        correlationId = IoTHubMessage_GetCorrelationId(messageHandle);
        ASSERT_IS_NOT_NULL(correlationId, "correlationId not received");

        if (g_e2e_test_options.use_special_chars && strncmp(correlationId, MSG_CORRELATION_ID_SPECIAL, sizeof(MSG_CORRELATION_ID_SPECIAL) - 1) != 0)
        {
            ASSERT_FAIL("Message correlation ID mismatch.");
        }
        else if (!g_e2e_test_options.use_special_chars && strncmp(correlationId, MSG_CORRELATION_ID, sizeof(MSG_CORRELATION_ID) - 1) != 0)
        {
            ASSERT_FAIL("Message correlation ID mismatch.");
        }

        IOTHUBMESSAGE_CONTENT_TYPE contentType = IoTHubMessage_GetContentType(messageHandle);
        ASSERT_ARE_EQUAL(int, (int)IOTHUBMESSAGE_BYTEARRAY, (int)contentType, "Content types don't match");
        ASSERT_ARE_EQUAL(IOTHUB_MESSAGE_RESULT, IOTHUB_MESSAGE_OK, IoTHubMessage_GetByteArray(messageHandle, &content, &contentSize));

        LogInfo("Received new message from IoT Hub :\nMessage-id: %s\nCorrelation-id: %s", messageId, correlationId);

        receiveUserContext->wasFound = true;
        MAP_HANDLE mapHandle = IoTHubMessage_Properties(messageHandle);
        ASSERT_IS_NOT_NULL(mapHandle, "No message properties available");

        const char*const* keys;
        const char*const* values;
        size_t propertyCount = 0;

        ASSERT_ARE_EQUAL(MAP_RESULT, MAP_OK, Map_GetInternals(mapHandle, &keys, &values, &propertyCount));
        ASSERT_ARE_EQUAL(int, MSG_PROP_COUNT, propertyCount);

        LogInfo("Message Properties:");
        const char** msg_prop_keys;
        const char** msg_prop_vals;
        if (g_e2e_test_options.use_special_chars)
        {
            msg_prop_keys = MSG_PROP_KEYS_SPECIAL;
            msg_prop_vals = MSG_PROP_VALS_SPECIAL;
        }
        else
        {
            msg_prop_keys = MSG_PROP_KEYS;
            msg_prop_vals = MSG_PROP_VALS;
        }

        for (size_t index = 0; index < propertyCount; index++)
        {
            LogInfo("\tKey: %s Value: %s", keys[index], values[index]);
            ASSERT_ARE_EQUAL(char_ptr, keys[index], msg_prop_keys[index], "Expected keys mismatch at index %lu", (unsigned long)index);
            ASSERT_ARE_EQUAL(char_ptr, values[index], msg_prop_vals[index], "Expected values mismatch at index %lu", (unsigned long)index);
        }

        receiveUserContext->wasFound = true;           
        Unlock(receiveUserContext->lock);
    }
    return IOTHUBMESSAGE_ACCEPTED;
}

static void GenerateUniqueId(const char* prefix, size_t prefix_len, char* buffer, size_t buffer_len)
{
    if (prefix_len <= buffer_len)
    {
        memcpy(buffer, prefix, prefix_len);
        buffer_len -= prefix_len;
        if ((MSG_UNIQUE_ID_STAMP_LEN) <= buffer_len)
        {
            buffer[prefix_len - 1] = '.';
            for (int i = 0; i < (MSG_UNIQUE_ID_STAMP_LEN - 1); i++)
            {
                buffer[prefix_len + i] = (unsigned char)((gb_rand() % 10) + '0');
            }
            buffer[prefix_len + MSG_UNIQUE_ID_STAMP_LEN - 1] = '\0';
        }
    }
    else
    {
        buffer[0] = '\0';
    }
}

static EXPECTED_RECEIVE_DATA* ReceiveUserContext_Create(void)
{
    EXPECTED_RECEIVE_DATA* result = (EXPECTED_RECEIVE_DATA*)calloc(sizeof(EXPECTED_RECEIVE_DATA), 1);
    if (result != NULL)
    {
        if ((result->lock = Lock_Init()) == NULL)
        {
            free(result);
            result = NULL;
        }
    }
    return result;
}

static void ReceiveUserContext_Destroy(EXPECTED_RECEIVE_DATA* data)
{
    if (data != NULL)
    {
        (void)Lock_Deinit(data->lock);

        if (data->msgHandle != NULL)
        {
            IoTHubMessage_Destroy(data->msgHandle);
        }

        free(data);
    }
}

static EXPECTED_SEND_DATA* EventData_Create_With_String(char* string)
{
    EXPECTED_SEND_DATA* result = (EXPECTED_SEND_DATA*)malloc(sizeof(EXPECTED_SEND_DATA));

    if (result == NULL)
    {
        LogError("Failed allocating EXPECTED_SEND_DATA");
    }
    else
    {
        memset(result, 0, sizeof(*result));

        if ((result->lock = Lock_Init()) == NULL)
        {
            ASSERT_FAIL("unable to Lock_Init");
            free(result);
            result = NULL;
        }
        else
        {
            result->expectedString = string;
            result->wasFound = false;
            result->dataWasRecv = false;
            result->result = IOTHUB_CLIENT_CONFIRMATION_ERROR;
        }
    }

    return result;
}

static EXPECTED_SEND_DATA* EventData_Create(void)
{
    EXPECTED_SEND_DATA* result;
    char temp[1000];
    char* tempString;
    time_t t = time(NULL);
    size_t string_length;
    const char* data_fmt;

    if (g_e2e_test_options.use_special_chars)
    {
        data_fmt = TEST_EVENT_DATA_FMT_SPECIAL_CHAR;
    }
    else
    {
        data_fmt = TEST_EVENT_DATA_FMT;
    }

    string_length = sprintf(temp, data_fmt, ctime(&t), g_iotHubTestId++);
    if ((tempString = (char*)malloc(string_length + 1)) == NULL)
    {
        result = NULL;
    }
    else
    {
        (void)strcpy(tempString, temp);
        result = EventData_Create_With_String(tempString);

        if (result == NULL)
        {
            free(tempString);
        }
    }

    return result;
}

static EXPECTED_SEND_DATA* EventData_Create_With_Custom_Size(size_t messageSize)
{
    EXPECTED_SEND_DATA* result;
    char* message = calloc(messageSize, sizeof(char));

    ASSERT_IS_NOT_NULL(message, "Failed to allocate EventData message");

    size_t idStringLength = sprintf(message, "%d", (int)g_iotHubTestId++);

    (void)memset(message + idStringLength, 'a', messageSize - idStringLength - 1);

    result = EventData_Create_With_String(message);

    ASSERT_IS_NOT_NULL(result, "Failed allocating EventData");

    return result;
}

static void EventData_Destroy(EXPECTED_SEND_DATA* data)
{
    if (data != NULL)
    {
        (void)Lock_Deinit(data->lock);
        if (data->expectedString != NULL)
        {
            free((void*)data->expectedString);
        }
        if (data->msgHandle != NULL)
        {
            IoTHubMessage_Destroy(data->msgHandle);
        }
        free(data);
    }
}

#ifdef AZIOT_LINUX
static char* get_target_mac_address()
{
    char* result;
    int s;

    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        LogError("Failure: socket create failure %d.", s);
        result = NULL;
    }
    else
    {
        struct ifreq ifr;
        struct ifconf ifc;
        char buf[1024];

        ifc.ifc_len = sizeof(buf);
        ifc.ifc_buf = buf;

        if (ioctl(s, SIOCGIFCONF, &ifc) == -1)
        {
            LogError("ioctl failed querying socket (SIOCGIFCONF)");
            result = NULL;
        }
        else
        {
            struct ifreq* it = ifc.ifc_req;
            const struct ifreq* const end = it + (ifc.ifc_len / sizeof(struct ifreq));

            result = NULL;

            for (; it != end; ++it)
            {
                strcpy(ifr.ifr_name, it->ifr_name);

                if (ioctl(s, SIOCGIFFLAGS, &ifr) != 0)
                {
                    LogError("ioctl failed querying socket (SIOCGIFFLAGS)");
                    break;
                }
                else if (ioctl(s, SIOCGIFHWADDR, &ifr) != 0)
                {
                    LogError("ioctl failed querying socket (SIOCGIFHWADDR)");
                    break;
                }
                else if (ioctl(s, SIOCGIFADDR, &ifr) != 0)
                {
                    LogError("ioctl failed querying socket (SIOCGIFADDR)");
                    break;
                }
                else if (strcmp(ifr.ifr_name, "eth0") == 0)
                {
                    unsigned char* mac = (unsigned char*)ifr.ifr_hwaddr.sa_data;

                    if ((result = (char*)malloc(sizeof(char) * 18)) == NULL)
                    {
                        LogError("failed formatting mac address (malloc failed)");
                    }
                    else if (sprintf(result, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]) <= 0)
                    {
                        LogError("failed formatting mac address (sprintf failed)");
                        free(result);
                        result = NULL;
                    }

                    break;
                }
            }
        }

        close(s);
    }

    return result;
}
#endif //AZIOT_LINUX


void e2e_init(TEST_PROTOCOL_TYPE protocol_type, bool testing_modules)
{
    int result = IoTHub_Init();
    ASSERT_ARE_EQUAL(int, 0, result, "Iothub init failed");
    g_iothubAcctInfo = IoTHubAccount_Init(testing_modules);
    ASSERT_IS_NOT_NULL(g_iothubAcctInfo, "Could not initialize IoTHubAccount");
    (void)IoTHub_Init();

    memset(&g_e2e_test_options, 0, sizeof(E2E_TEST_OPTIONS));
    g_e2e_test_options.set_mac_address = false;
    g_e2e_test_options.use_special_chars = false;

    g_connection_status_info.lock = Lock_Init();
    test_protocol_type = protocol_type;
}

void e2e_deinit(void)
{
    IoTHubAccount_deinit(g_iothubAcctInfo);
    // Need a double deinit
    IoTHub_Deinit();
    IoTHub_Deinit();

    Lock_Deinit(g_connection_status_info.lock);
}

static void setoption_on_device_or_module(const char * optionName, const void * optionData, const char * errorMessage)
{
    IOTHUB_CLIENT_RESULT result;

    if (iothub_moduleclient_handle != NULL)
    {
        result = IoTHubModuleClient_SetOption(iothub_moduleclient_handle, optionName, optionData);
    }
    else if (iothub_deviceclient_handle != NULL)
    {
        result = IoTHubDeviceClient_SetOption(iothub_deviceclient_handle, optionName, optionData);
    }
    else if (iothub_moduleclient_ll_handle != NULL)
    {
        result = IoTHubModuleClient_LL_SetOption(iothub_moduleclient_ll_handle, optionName, optionData);
    }
    else
    {
        result = IoTHubDeviceClient_LL_SetOption(iothub_deviceclient_ll_handle, optionName, optionData);
    }

    ASSERT_ARE_EQUAL(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, errorMessage);
}

static void setconnectionstatuscallback_on_device_or_module()
{
    IOTHUB_CLIENT_RESULT result;

    if (iothub_moduleclient_handle != NULL)
    {
        result = IoTHubModuleClient_SetConnectionStatusCallback(iothub_moduleclient_handle, connection_status_callback, &g_connection_status_info);
    }
    else if (iothub_deviceclient_handle != NULL)
    {
        result = IoTHubDeviceClient_SetConnectionStatusCallback(iothub_deviceclient_handle, connection_status_callback, &g_connection_status_info);
    }
    else if(iothub_moduleclient_ll_handle != NULL)
    {
        result = IoTHubModuleClient_LL_SetConnectionStatusCallback(iothub_moduleclient_ll_handle, connection_status_callback, &g_connection_status_info);
    }
    else
    {
        result = IoTHubDeviceClient_LL_SetConnectionStatusCallback(iothub_deviceclient_ll_handle, connection_status_callback, &g_connection_status_info);
    }

    ASSERT_ARE_EQUAL(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "Could not set connection Status Callback");
}

static void sendeventasync_on_device_or_module(IOTHUB_MESSAGE_HANDLE msgHandle, EXPECTED_SEND_DATA* sendData)
{
    IOTHUB_CLIENT_RESULT result;

    if (iothub_moduleclient_handle != NULL)
    {
        result = IoTHubModuleClient_SendEventAsync(iothub_moduleclient_handle, msgHandle, ReceiveConfirmationCallback, sendData);
    }
    else if (iothub_deviceclient_handle != NULL)
    {
        result = IoTHubDeviceClient_SendEventAsync(iothub_deviceclient_handle, msgHandle, ReceiveConfirmationCallback, sendData);
    }
    else if (iothub_moduleclient_ll_handle != NULL)
    {
        result = IoTHubModuleClient_LL_SendEventAsync(iothub_moduleclient_ll_handle, msgHandle, ReceiveConfirmationCallback, sendData);
    }
    else
    {
        result = IoTHubDeviceClient_LL_SendEventAsync(iothub_deviceclient_ll_handle, msgHandle, ReceiveConfirmationCallback, sendData);
    }

    ASSERT_ARE_EQUAL(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "SendEventAsync failed");
}

static void setmessagecallback_on_device_or_module(EXPECTED_RECEIVE_DATA* receiveUserContext)
{
    IOTHUB_CLIENT_RESULT result;

    if (iothub_moduleclient_handle != NULL)
    {
        result = IoTHubModuleClient_SetMessageCallback(iothub_moduleclient_handle, ReceiveMessageCallback, receiveUserContext);
    }
    else if (iothub_deviceclient_handle != NULL)
    {
        result = IoTHubDeviceClient_SetMessageCallback(iothub_deviceclient_handle, ReceiveMessageCallback, receiveUserContext);
    }
    else if (iothub_moduleclient_ll_handle != NULL)
    {
        result = IoTHubModuleClient_LL_SetMessageCallback(iothub_moduleclient_ll_handle, ReceiveMessageCallback, receiveUserContext);
    }
    else
    {
        result = IoTHubDeviceClient_LL_SetMessageCallback(iothub_deviceclient_ll_handle, ReceiveMessageCallback, receiveUserContext);
    }

    ASSERT_ARE_EQUAL(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "IoTHubDeviceClient_SetMessageCallback failed");
}


static void destroy_on_device_or_module()
{
    if (iothub_deviceclient_handle != NULL)
    {
        IoTHubDeviceClient_Destroy(iothub_deviceclient_handle);
        iothub_deviceclient_handle = NULL;
    }

    if (iothub_moduleclient_handle != NULL)
    {
        IoTHubModuleClient_Destroy(iothub_moduleclient_handle);
        iothub_moduleclient_handle = NULL;
    }

    if (iothub_deviceclient_ll_handle != NULL)
    {
        IoTHubDeviceClient_LL_Destroy(iothub_deviceclient_ll_handle);
        iothub_deviceclient_ll_handle = NULL;
    }

    if (iothub_moduleclient_ll_handle != NULL)
    {
        IoTHubModuleClient_LL_Destroy(iothub_moduleclient_ll_handle);
        iothub_moduleclient_ll_handle = NULL;
    }
}

static void run_client_do_work()
{
    if (iothub_deviceclient_ll_handle != NULL)
    {
        IoTHubDeviceClient_LL_DoWork(iothub_deviceclient_ll_handle);
    }
    else if (iothub_moduleclient_ll_handle != NULL)
    {
        IoTHubModuleClient_LL_DoWork(iothub_moduleclient_ll_handle);
    }
}

static void setup_iothub_client(IOTHUB_PROVISIONED_DEVICE* deviceToUse)
{
#ifdef SET_TRUSTED_CERT_IN_SAMPLES
    setoption_on_device_or_module(OPTION_TRUSTED_CERT, certificates, "Cannot enable trusted cert");
#endif // SET_TRUSTED_CERT_IN_SAMPLES

    // Set connection status change callback
    setconnectionstatuscallback_on_device_or_module();

    if (deviceToUse->howToCreate == IOTHUB_ACCOUNT_AUTH_X509)
    {
        setoption_on_device_or_module(OPTION_X509_CERT, deviceToUse->certificate, "Could not set the device x509 certificate");
        setoption_on_device_or_module(OPTION_X509_PRIVATE_KEY, deviceToUse->primaryAuthentication, "Could not set the device x509 privateKey");
    }

    bool trace = true;
    setoption_on_device_or_module(OPTION_LOG_TRACE, &trace, "Cannot enable tracing");

    setoption_on_device_or_module(OPTION_PRODUCT_INFO, "MQTT_E2E/1.1.12", "Cannot set product info");

    //Turn on URL encoding/decoding (MQTT)
    if (g_e2e_test_options.use_special_chars)
    {
        bool encodeDecode = true;
        setoption_on_device_or_module(OPTION_AUTO_URL_ENCODE_DECODE, &encodeDecode, "Cannot set auto_url_encode/decode");
    }

    if (test_protocol_type == TEST_AMQP || test_protocol_type == TEST_AMQP_WEBSOCKETS)
    {
        size_t svc2cl_keep_alive_timeout_secs = 120; // service will send pings at 120 x 7/8 = 105 seconds. Higher the value, lesser the frequency of service side pings.
        setoption_on_device_or_module(OPTION_SERVICE_SIDE_KEEP_ALIVE_FREQ_SECS, &svc2cl_keep_alive_timeout_secs, "Cannot set OPTION_SERVICE_SIDE_KEEP_ALIVE_FREQ_SECS");

        // Set keep alive for remote idle is optional. If it is not set the default ratio of 1/2 will be used. For default value of 4 minutes, it will be 2 minutes (120 seconds)
        double cl2svc_keep_alive_send_ratio = 1.0 / 2.0; // Set it to 120 seconds (240 x 1/2 = 120 seconds) for 4 minutes remote idle.

        // client will send pings to service at 210 second interval for 4 minutes remote idle. For 25 minutes remote idle, it will be set to 21 minutes.
        setoption_on_device_or_module(OPTION_REMOTE_IDLE_TIMEOUT_RATIO, &cl2svc_keep_alive_send_ratio, "Cannot set OPTION_REMOTE_IDLE_TIMEOUT_RATIO");
    }
#ifdef AZIOT_LINUX
    if (g_e2e_test_options.set_mac_address)
    {
        char* mac_address = get_target_mac_address();
        ASSERT_IS_NOT_NULL(mac_address, "failed getting the target MAC ADDRESS");

        setoption_on_device_or_module(OPTION_NET_INT_MAC_ADDRESS, mac_address, "Cannot setoption MAC ADDRESS");

        LogInfo("Target MAC ADDRESS: %s", mac_address);
        free(mac_address);
    }
#endif //AZIOT_LINUX
}

static void client_connect_to_hub(IOTHUB_PROVISIONED_DEVICE* deviceToUse, IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    ASSERT_IS_NULL(iothub_deviceclient_handle, "iothub_deviceclient_handle is non-NULL on test initialization");
    ASSERT_IS_NULL(iothub_moduleclient_handle, "iothub_moduleclient_handle is non-NULL on test initialization");

    if (deviceToUse->moduleConnectionString != NULL)
    {
        iothub_moduleclient_handle = IoTHubModuleClient_CreateFromConnectionString(deviceToUse->moduleConnectionString, protocol);
        ASSERT_IS_NOT_NULL(iothub_moduleclient_handle, "Could not invoke IoTHubModuleClient_CreateFromConnectionString");
    }
    else
    {
        iothub_deviceclient_handle = IoTHubDeviceClient_CreateFromConnectionString(deviceToUse->connectionString, protocol);
        ASSERT_IS_NOT_NULL(iothub_deviceclient_handle, "Could not invoke IoTHubDeviceClient_CreateFromConnectionString");
    }

    setup_iothub_client(deviceToUse);
}

static void client_ll_connect_to_hub(IOTHUB_PROVISIONED_DEVICE* deviceToUse, IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    size_t client_conn_wait_time = 4000;

    ASSERT_IS_NULL(iothub_deviceclient_ll_handle, "iothub_deviceclient_ll_handle is non-NULL on test initialization");
    ASSERT_IS_NULL(iothub_moduleclient_ll_handle, "iothub_moduleclient_ll_handle is non-NULL on test initialization");

    if (deviceToUse->moduleConnectionString != NULL)
    {
        iothub_moduleclient_ll_handle = IoTHubModuleClient_LL_CreateFromConnectionString(deviceToUse->moduleConnectionString, protocol);
        ASSERT_IS_NOT_NULL(iothub_moduleclient_ll_handle, "Could not invoke IoTHubModuleClient_LL_CreateFromConnectionString");
    }
    else
    {
        iothub_deviceclient_ll_handle = IoTHubDeviceClient_LL_CreateFromConnectionString(deviceToUse->connectionString, protocol);
        ASSERT_IS_NOT_NULL(iothub_deviceclient_ll_handle, "Could not invoke IoTHubDeviceClient_LL_CreateFromConnectionString");
    }

    setup_iothub_client(deviceToUse);

    // LL clients require _DoWork to be explicitly called to do network I/O.
    // Calling the function below guarantees DoWork is called and the client gets authenticated.
    ASSERT_IS_TRUE(wait_for_client_authenticated(client_conn_wait_time));
}

void client_create_and_send_d2c_messages(TEST_MESSAGE_CREATION_MECHANISM test_message_creation, int count, size_t messageSize, SEND_DATA_BUNDLE* d2cMessages)
{
    for (int i = 0; i < count; i++)
    {
        IOTHUB_MESSAGE_HANDLE msgHandle;
        EXPECTED_SEND_DATA* sendData = (messageSize == 0 ? EventData_Create() : EventData_Create_With_Custom_Size(messageSize));
        ASSERT_IS_NOT_NULL(sendData, "Could not create the EventData associated with the event to be sent");

        if (test_message_creation == TEST_MESSAGE_CREATE_BYTE_ARRAY)
        {
            msgHandle = IoTHubMessage_CreateFromByteArray((const unsigned char*)sendData->expectedString, strlen(sendData->expectedString));
        }
        else if (test_message_creation == TEST_MESSAGE_CREATE_STRING)
        {
            msgHandle = IoTHubMessage_CreateFromString(sendData->expectedString);
        }
        else
        {
            msgHandle = NULL;
            ASSERT_FAIL("Unknown test message creation mechanism specified");
        }
        ASSERT_IS_NOT_NULL(msgHandle, "Could not create the D2C message to be sent");

        for (size_t j = 0; j < MSG_PROP_COUNT; j++)
        {
            if (g_e2e_test_options.use_special_chars)
            {
                ASSERT_ARE_EQUAL(IOTHUB_MESSAGE_RESULT, IOTHUB_MESSAGE_OK, IoTHubMessage_SetProperty(msgHandle, MSG_PROP_KEYS_SPECIAL[j], MSG_PROP_VALS_SPECIAL[j]), "Map_AddOrUpdate failed for property %zu!", j);
            }
            else
            {
                ASSERT_ARE_EQUAL(IOTHUB_MESSAGE_RESULT, IOTHUB_MESSAGE_OK, IoTHubMessage_SetProperty(msgHandle, MSG_PROP_KEYS[j], MSG_PROP_VALS[j]), "Map_AddOrUpdate failed for property %zu!", j);
            }
        }

        sendData->msgHandle = msgHandle;
        d2cMessages->items[i] = sendData;
        d2cMessages->count++;

        // act
        sendeventasync_on_device_or_module(msgHandle, sendData);
    }
}

D2C_MESSAGE_HANDLE client_create_and_send_d2c(TEST_MESSAGE_CREATION_MECHANISM test_message_creation)
{
    SEND_DATA_BUNDLE d2cMessages;
    client_create_and_send_d2c_messages(test_message_creation, 1, DEFAULT_MESSAGE_SIZE, &d2cMessages);
    return d2cMessages.items[0];
}

bool client_wait_for_d2c_confirmations(SEND_DATA_BUNDLE* d2cMessages, IOTHUB_CLIENT_CONFIRMATION_RESULT expectedClientResult)
{
    time_t beginOperation, nowTime;
    bool allMessagesDelivered = false;

    for (int i = 0; i < d2cMessages->count; i++)
    {
        LogInfo("Begin wait for d2c confirmation.  d2cMessage=<%p>", d2cMessages->items[i]);
    }

    beginOperation = time(NULL);
    while (
        (nowTime = time(NULL)),
        !allMessagesDelivered && (difftime(nowTime, beginOperation) < MAX_CLOUD_TRAVEL_TIME) // time box
        )
    {
        allMessagesDelivered = true;

        for (int i = 0; i < d2cMessages->count; i++)
        {
            if (!client_received_confirmation(d2cMessages->items[i], expectedClientResult))
            {
                allMessagesDelivered = false;
                break;
            }
        }

        run_client_do_work();
        ThreadAPI_Sleep(100);
    }

    for (int i = 0; i < d2cMessages->count; i++)
    {
        bool messageSent = client_received_confirmation(d2cMessages->items[i], expectedClientResult);
        LogInfo("Completed wait for d2c confirmation. d2cMessage=<%p>, ret=<%d>", d2cMessages->items[i], messageSent);
    }

    return allMessagesDelivered;
}

bool client_wait_for_d2c_confirmation(D2C_MESSAGE_HANDLE d2cMessage, IOTHUB_CLIENT_CONFIRMATION_RESULT expectedClientResult)
{
    SEND_DATA_BUNDLE d2cMessages;
    d2cMessages.count = 1;
    d2cMessages.items[0] = (EXPECTED_SEND_DATA*)d2cMessage;
    return client_wait_for_d2c_confirmations(&d2cMessages, expectedClientResult);
}

bool client_received_confirmation(D2C_MESSAGE_HANDLE d2cMessage, IOTHUB_CLIENT_CONFIRMATION_RESULT expectedClientResult)
{
    bool result = false;
    EXPECTED_SEND_DATA* sendData = (EXPECTED_SEND_DATA*)d2cMessage;

    ASSERT_ARE_EQUAL(LOCK_RESULT, LOCK_OK, Lock(sendData->lock));
    result = sendData->dataWasRecv;
    if (sendData->dataWasRecv == true)
    {
        ASSERT_ARE_EQUAL(int, expectedClientResult, sendData->result, "Result from callback does not match expected");
    }
    (void)Unlock(sendData->lock);

    return result;
}

// White-listed IoT Hub's (ONLY!) have special logic that looks at message properties such as 'AzIoTHub_FaultOperationType' and if so, will
// cause the specified error to occur.  This allows end-to-end testing to simulate across a wide range of errors.
D2C_MESSAGE_HANDLE send_error_injection_message(const char* faultOperationType, const char* faultOperationCloseReason, const char* faultOperationDelayInSecs)
{
    IOTHUB_MESSAGE_HANDLE msgHandle;

    EXPECTED_SEND_DATA* sendData = EventData_Create();
    ASSERT_IS_NOT_NULL(sendData, "Could not create the EventData associated with the event to be sent");

    if (sendData != NULL)
    {
        msgHandle = IoTHubMessage_CreateFromByteArray((const unsigned char*)sendData->expectedString, strlen(sendData->expectedString));
        ASSERT_IS_NOT_NULL(msgHandle, "Could not create the D2C message to be sent");

        ASSERT_ARE_EQUAL(IOTHUB_MESSAGE_RESULT, IOTHUB_MESSAGE_OK, IoTHubMessage_SetProperty(msgHandle, "AzIoTHub_FaultOperationType", faultOperationType));
        ASSERT_ARE_EQUAL(IOTHUB_MESSAGE_RESULT, IOTHUB_MESSAGE_OK, IoTHubMessage_SetProperty(msgHandle, "AzIoTHub_FaultOperationCloseReason", faultOperationCloseReason));
        ASSERT_ARE_EQUAL(IOTHUB_MESSAGE_RESULT, IOTHUB_MESSAGE_OK, IoTHubMessage_SetProperty(msgHandle, "AzIoTHub_FaultOperationDelayInSecs", faultOperationDelayInSecs));
        ASSERT_ARE_EQUAL(IOTHUB_MESSAGE_RESULT, IOTHUB_MESSAGE_OK, IoTHubMessage_SetProperty(msgHandle, "AzIoTHub_FaultOperationDurationInSecs", "20"));

        sendData->msgHandle = msgHandle;

        // act
        sendeventasync_on_device_or_module(msgHandle, sendData);
    }

    return (D2C_MESSAGE_HANDLE)sendData;
}

bool client_wait_for_connection_fault()
{
    time_t beginOperation, nowTime;

    beginOperation = time(NULL);
    while (
        (nowTime = time(NULL)),
        (difftime(nowTime, beginOperation) < MAX_CLOUD_TRAVEL_TIME_FAULT_INJECTION) // time box
        )
    {
        if (client_status_fault_happened())
        {
            break;
        }
        ThreadAPI_Sleep(100);
    }
    return (client_status_fault_happened());
}

bool client_status_fault_happened()
{
    bool result = false;

    ASSERT_ARE_EQUAL(LOCK_RESULT, LOCK_OK, Lock(g_connection_status_info.lock));
    result = g_connection_status_info.connFaultHappened;
    (void)Unlock(g_connection_status_info.lock);

    return result;
}

bool wait_for_client_authenticated(size_t wait_time)
{
    bool result = false;
    time_t beginOperation, nowTime;

    beginOperation = time(NULL);
    while (
        (nowTime = time(NULL)),
        (difftime(nowTime, beginOperation) < wait_time) // time box
        )
    {
        ASSERT_ARE_EQUAL(LOCK_RESULT, LOCK_OK, Lock(g_connection_status_info.lock));

        result = (g_connection_status_info.currentStatus == IOTHUB_CLIENT_CONNECTION_AUTHENTICATED);
        (void)Unlock(g_connection_status_info.lock);

        if (result)
        {
            break;
        }

        run_client_do_work();
        ThreadAPI_Sleep(100);
    }
    return result;
}

bool client_wait_for_connection_restored()
{
    time_t beginOperation, nowTime;

    beginOperation = time(NULL);
    while (
        (nowTime = time(NULL)),
        (difftime(nowTime, beginOperation) < MAX_CLOUD_TRAVEL_TIME) // time box
        )
    {
        if (client_status_restored())
        {
            break;
        }
        ThreadAPI_Sleep(100);
    }
    return (client_status_restored());
}

bool client_status_restored()
{
    bool result = false;

    ASSERT_ARE_EQUAL(LOCK_RESULT, LOCK_OK, Lock(g_connection_status_info.lock));
    result = g_connection_status_info.connRestored;
    (void)Unlock(g_connection_status_info.lock);

    return result;
}

// Resets global connection status at beginning of tests.
void clear_connection_status_info_flags()
{
    ASSERT_ARE_EQUAL(LOCK_RESULT, LOCK_OK, Lock(g_connection_status_info.lock));
    g_connection_status_info.connFaultHappened = false;
    g_connection_status_info.connRestored = false;
    g_connection_status_info.currentStatus = IOTHUB_CLIENT_CONNECTION_UNAUTHENTICATED;
    g_connection_status_info.currentStatusReason = IOTHUB_CLIENT_CONNECTION_NO_NETWORK;
    (void)Unlock(g_connection_status_info.lock);
}

static void service_wait_for_security_d2c_event_arrival(IOTHUB_PROVISIONED_DEVICE* deviceToUse, D2C_MESSAGE_HANDLE d2cMessage, double max_wait_time)
{
    EXPECTED_SEND_DATA* sendData = (EXPECTED_SEND_DATA*)d2cMessage;

    SEND_DATA_BUNDLE d2cMessages;
    d2cMessages.count = 1;
    d2cMessages.items[0] = sendData;

    IOTHUB_TEST_HANDLE iotHubTestHandle = IoTHubTest_Initialize(IoTHubAccount_GetEventHubConnectionString(g_iothubAcctInfo), IoTHubAccount_GetIoTHubConnString(g_iothubAcctInfo), deviceToUse->deviceId, IoTHubAccount_GetEventhubListenName(g_iothubAcctInfo), IoTHubAccount_GetEventhubAccessKey(g_iothubAcctInfo), IoTHubAccount_GetSharedAccessSignature(g_iothubAcctInfo), IoTHubAccount_GetEventhubConsumerGroup(g_iothubAcctInfo));
    ASSERT_IS_NOT_NULL(iotHubTestHandle, "Could not initialize IoTHubTest in order to listen for events");

    LogInfo("Beginning to listen for d2c event arrival.  Waiting up to %f seconds...", max_wait_time);
    IOTHUB_TEST_CLIENT_RESULT result = IoTHubTest_ListenForEvent(iotHubTestHandle, IoTHubCallback, IoTHubAccount_GetIoTHubPartitionCount(g_iothubAcctInfo), &d2cMessages, time(NULL)-SERVICE_EVENT_WAIT_TIME_DELTA_SECONDS, max_wait_time);
    ASSERT_ARE_EQUAL(IOTHUB_TEST_CLIENT_RESULT, IOTHUB_TEST_CLIENT_ERROR, result, "Listening for the event failed");

    ASSERT_IS_FALSE(sendData->wasFound, "Failure event was not routed correctly when sending security event"); // was found is written by the callback...

    IoTHubTest_Deinit(iotHubTestHandle);

    LogInfo("Completed listening for security event arrival.");
}

void service_wait_for_d2c_events_arrival(IOTHUB_PROVISIONED_DEVICE* deviceToUse, SEND_DATA_BUNDLE* d2cMessages, double max_wait_time)
{
    bool allMessagesArrived = true;

    IOTHUB_TEST_HANDLE iotHubTestHandle = IoTHubTest_Initialize(IoTHubAccount_GetEventHubConnectionString(g_iothubAcctInfo), IoTHubAccount_GetIoTHubConnString(g_iothubAcctInfo), deviceToUse->deviceId, IoTHubAccount_GetEventhubListenName(g_iothubAcctInfo), IoTHubAccount_GetEventhubAccessKey(g_iothubAcctInfo), IoTHubAccount_GetSharedAccessSignature(g_iothubAcctInfo), IoTHubAccount_GetEventhubConsumerGroup(g_iothubAcctInfo));
    ASSERT_IS_NOT_NULL(iotHubTestHandle, "Could not initialize IoTHubTest in order to listen for events");

    LogInfo("Beginning to listen for d2c event arrival.  Waiting up to %f seconds...", max_wait_time);
    IOTHUB_TEST_CLIENT_RESULT result = IoTHubTest_ListenForEvent(iotHubTestHandle, IoTHubCallback, IoTHubAccount_GetIoTHubPartitionCount(g_iothubAcctInfo), d2cMessages, time(NULL) - SERVICE_EVENT_WAIT_TIME_DELTA_SECONDS, max_wait_time);
    ASSERT_ARE_EQUAL(IOTHUB_TEST_CLIENT_RESULT, IOTHUB_TEST_CLIENT_OK, result, "Listening for the event failed");

    for (int i = 0; i < d2cMessages->count; i++)
    {
        allMessagesArrived &= d2cMessages->items[i]->wasFound;
        LogInfo("d2c message %p received by eventhub: %d", d2cMessages->items[i], d2cMessages->items[i]->wasFound);
    }
    ASSERT_IS_TRUE(allMessagesArrived, "Failure retrieving all data that was sent to eventhub"); // was found is written by the callback...

    IoTHubTest_Deinit(iotHubTestHandle);

    LogInfo("Completed listening for d2c event arrival.");
}

void service_wait_for_d2c_event_arrival(IOTHUB_PROVISIONED_DEVICE* deviceToUse, D2C_MESSAGE_HANDLE d2cMessage, double max_wait_time)
{
    SEND_DATA_BUNDLE d2cMessages;
    d2cMessages.count = 1;
    d2cMessages.items[0] = (EXPECTED_SEND_DATA*)d2cMessage;
    service_wait_for_d2c_events_arrival(deviceToUse, &d2cMessages, max_wait_time);
}

bool service_received_the_message(D2C_MESSAGE_HANDLE d2cMessage)
{
    return ((EXPECTED_SEND_DATA*)d2cMessage)->wasFound;
}

void destroy_d2c_message_handle(D2C_MESSAGE_HANDLE d2cMessage)
{
    LogInfo("Destroying message %p", d2cMessage);
    EventData_Destroy((EXPECTED_SEND_DATA*)d2cMessage);
}

void destroy_d2c_messages(SEND_DATA_BUNDLE* d2cMessages)
{
    for (int i = 0; i < d2cMessages->count; i++)
    {
        destroy_d2c_message_handle(d2cMessages->items[i]);
    }
}

static void send_event_test(IOTHUB_PROVISIONED_DEVICE* deviceToUse, IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    TEST_MESSAGE_CREATION_MECHANISM test_message_creation[] = { TEST_MESSAGE_CREATE_BYTE_ARRAY, TEST_MESSAGE_CREATE_STRING };

    int i;
    for (i = 0; i < sizeof(test_message_creation) / sizeof(test_message_creation[0]); i++)
    {
        // arrange
        D2C_MESSAGE_HANDLE d2cMessage;

        // Create the IoT Hub Data
        client_connect_to_hub(deviceToUse, protocol);

        // Send the Event from the client
        d2cMessage = client_create_and_send_d2c(test_message_creation[i]);

        // Wait for confirmation that the event was recevied
        bool dataWasRecv = client_wait_for_d2c_confirmation(d2cMessage, IOTHUB_CLIENT_CONFIRMATION_OK);
        ASSERT_IS_TRUE(dataWasRecv, "Failure sending data to IotHub"); // was received by the callback...

        // close the client connection
        destroy_on_device_or_module();

        // Wait for the message to arrive
        service_wait_for_d2c_event_arrival(deviceToUse, d2cMessage, MAX_SERVICE_EVENT_WAIT_TIME_SECONDS);

        // cleanup
        destroy_d2c_message_handle(d2cMessage);
    }
}

/**
 * This is a test function that can send several telemetry messages in one shot,
 * aiming to trigger the telemetry batching capability of the Azure IoT C SDK (AMQP). 
 * We need to use the Azure IoT C SDK LL API to force the batching to occur, since we lose
 * control on convenience layer (which has a dedicated thread running *DoWork, and could consume
 * messages on the `waitingToSend` list too fast and prevent telemetry batching to kick in). 
 */
static void send_batch_event_test(IOTHUB_PROVISIONED_DEVICE* deviceToUse, IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    TEST_MESSAGE_CREATION_MECHANISM test_message_creation[] = { TEST_MESSAGE_CREATE_BYTE_ARRAY, TEST_MESSAGE_CREATE_STRING };

    int i;
    for (i = 0; i < sizeof(test_message_creation) / sizeof(test_message_creation[0]); i++)
    {
        // arrange
        SEND_DATA_BUNDLE d2cMessages = { 0 };

        clear_connection_status_info_flags();

        // Create the IoT Hub Data
        client_ll_connect_to_hub(deviceToUse, protocol);

        // Send the Events from the client
        client_create_and_send_d2c_messages(test_message_creation[i], LARGE_MESSAGE_COUNT, LARGE_MESSAGE_SIZE, &d2cMessages);

        // Wait for confirmation that the event was recevied
        bool dataWasRecv = client_wait_for_d2c_confirmations(&d2cMessages, IOTHUB_CLIENT_CONFIRMATION_OK);
        ASSERT_IS_TRUE(dataWasRecv, "Failure sending data to IotHub"); // was received by the callback...

        // close the client connection
        destroy_on_device_or_module();

        // Wait for the message to arrive
        service_wait_for_d2c_events_arrival(deviceToUse, &d2cMessages, MAX_SERVICE_EVENT_WAIT_TIME_SECONDS);

        // cleanup
        destroy_d2c_messages(&d2cMessages);
    }
}

void e2e_send_event_test_sas(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    send_event_test(IoTHubAccount_GetSASDevice(g_iothubAcctInfo), protocol);
}

void e2e_send_batch_event_test_sas(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    send_batch_event_test(IoTHubAccount_GetSASDevice(g_iothubAcctInfo), protocol);
}

void e2e_send_event_test_x509(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    send_event_test(IoTHubAccount_GetX509Device(g_iothubAcctInfo), protocol);
}

void e2e_send_security_event_test_sas(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    //IOTHUB_MESSAGE_HANDLE msgHandle;

    //char* test_telemetry_message;
    const char* TEST_ASC_SECURITY_MESSAGE = "{ \
        \"AgentVersion\": \"0.0.1\", \
        \"AgentId\" : \"{A3B5D80C-06AA-4D84-BA2D-5470ADAE33A3}\", \
        \"MessageSchemaVersion\" : \"1.0\", \
        \"Events\" : \
        { \
            \"EventType\": \"Security\", \
            \"Category\" : \"Periodic\", \
            \"Name\" : \"ListeningPorts\", \
            \"IsEmpty\" : true, \
            \"PayloadSchemaVersion\" : \"1.0\", \
            \"Id\" : \"12432\", \
            \"TimestampLocal\" : \"2012-04-23T18:25:43.511Z\", \
            \"TimestampUTC\" : \"2012-04-23T18:25:43.511Z\" }, \
            \"Payload\": { \"data\": \"test\" } \
        } \
    }";

    // Create the IoT Hub Data
    IOTHUB_PROVISIONED_DEVICE* deviceToUse;
    deviceToUse = IoTHubAccount_GetSASDevice(g_iothubAcctInfo);
    client_connect_to_hub(deviceToUse, protocol);

    EXPECTED_SEND_DATA* send_data = (EXPECTED_SEND_DATA*)client_create_and_send_d2c(TEST_MESSAGE_CREATE_STRING);
    bool dataWasRecv = client_wait_for_d2c_confirmation((D2C_MESSAGE_HANDLE)send_data, IOTHUB_CLIENT_CONFIRMATION_OK);
    ASSERT_IS_TRUE(dataWasRecv, "Failure sending data to IotHub"); // was received by the callback...

    // Free the message
    IoTHubMessage_Destroy(send_data->msgHandle);

    // Send the messages to the ASC Event hub
    // Create an ASC Security Message
    LogInfo("Sending ASC message to endpoint");

    const char* temp_string_val = send_data->expectedString;

    send_data->msgHandle = IoTHubMessage_CreateFromString(TEST_ASC_SECURITY_MESSAGE);
    ASSERT_IS_NOT_NULL(send_data->msgHandle, "Could not create the ASC Security message to be sent");
    send_data->wasFound = false;
    send_data->dataWasRecv = false;
    send_data->result = IOTHUB_CLIENT_CONFIRMATION_ERROR;
    send_data->expectedString = TEST_ASC_SECURITY_MESSAGE;

    // Send the messages to the ASC Event hub
    ASSERT_ARE_EQUAL(IOTHUB_MESSAGE_RESULT, IOTHUB_MESSAGE_OK, IoTHubMessage_SetAsSecurityMessage(send_data->msgHandle), "Failure setting message as a security message");

    // Send ASC message, it should not arrive in the IoTHub EventHub
    sendeventasync_on_device_or_module(send_data->msgHandle, send_data);

    // close the client connection
    destroy_on_device_or_module();

    // Wait for the message to not arrive since it's going to an ASC eventhub
    service_wait_for_security_d2c_event_arrival(deviceToUse, send_data, MAX_SECURITY_DEVICE_WAIT_TIME);

    // Done with ASC test, restore string
    send_data->expectedString = temp_string_val;

    // cleanup
    EventData_Destroy(send_data);
}

// Simulates a fault occurring in end-to-end testing (with special opcodes forcing service failure on certain white-listed Hubs) and
// ability to recover after error.
void e2e_d2c_with_svc_fault_ctrl(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol, const char* faultOperationType, const char* faultOperationCloseReason, const char* faultOperationDelayInSecs)
{
    // arrange
    D2C_MESSAGE_HANDLE d2cMessageInitial = NULL;
    D2C_MESSAGE_HANDLE d2cMessageFaultInjection = NULL;
    D2C_MESSAGE_HANDLE d2cMessageDuringRetry = NULL;

    IOTHUB_PROVISIONED_DEVICE* deviceToUse = IoTHubAccount_GetSASDevice(g_iothubAcctInfo);

    clear_connection_status_info_flags();

    // Create the IoT Hub Data
    client_connect_to_hub(deviceToUse, protocol);

    // Send the Event from the client
    LogInfo("Creating and sending message...");
    d2cMessageInitial = client_create_and_send_d2c(TEST_MESSAGE_CREATE_STRING);

    // Wait for confirmation that the event was recevied
    LogInfo("Waiting for initial message %p", d2cMessageInitial);
    bool dataWasRecv = client_wait_for_d2c_confirmation(d2cMessageInitial, IOTHUB_CLIENT_CONFIRMATION_OK);
    ASSERT_IS_TRUE(dataWasRecv, "Failure sending data to IotHub"); // was received by the callback...

    LogInfo("Send server fault control message...");
    d2cMessageFaultInjection = send_error_injection_message(faultOperationType, faultOperationCloseReason, faultOperationDelayInSecs);
    LogInfo("FaultInject message handle is %p", d2cMessageFaultInjection);

    LogInfo("Sleeping after sending fault injection...");
    ThreadAPI_Sleep(5000);

    // Wait for the server fault message to be timed out
    LogInfo("Waiting for server fault message to timeout");
    dataWasRecv = client_wait_for_d2c_confirmation(d2cMessageFaultInjection, IOTHUB_CLIENT_CONFIRMATION_MESSAGE_TIMEOUT);
    ASSERT_IS_TRUE(dataWasRecv, "Failure recieving server fault message timeout"); // was received by the callback...

    // Send the Event fromsend_data. the client
    LogInfo("Send message after the server fault and then sleeping...");
    d2cMessageDuringRetry = client_create_and_send_d2c(TEST_MESSAGE_CREATE_STRING);
    ThreadAPI_Sleep(8000);

    // Wait for confirmation that the event was recevied
    LogInfo("Waiting for message after server fault %p", d2cMessageDuringRetry);
    dataWasRecv = client_wait_for_d2c_confirmation(d2cMessageDuringRetry, IOTHUB_CLIENT_CONFIRMATION_OK);

    ASSERT_IS_TRUE(dataWasRecv, "Don't recover after the fault...");

    // close the client connection
    destroy_on_device_or_module();

    // Wait for the message to arrive
    LogInfo("waiting for d2c arrive...");
    service_wait_for_d2c_event_arrival(deviceToUse, d2cMessageDuringRetry, MAX_SERVICE_EVENT_WAIT_TIME_SECONDS);

    // cleanup
    destroy_d2c_message_handle(d2cMessageDuringRetry);
    destroy_d2c_message_handle(d2cMessageFaultInjection);
    destroy_d2c_message_handle(d2cMessageInitial);
}

// Simulates a fault occurring in end-to-end testing (with special opcodes forcing service failure on certain white-listed Hubs) and
// ability to recover after error.  Further simulates connection status events being fired as expected.
// Note that not all classes of failures result in connection status being reflected.
void e2e_d2c_with_svc_fault_ctrl_with_transport_status(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol, const char* faultOperationType, const char* faultOperationCloseReason, const char* faultOperationDelayInSecs)
{
    IOTHUB_PROVISIONED_DEVICE* deviceToUse = IoTHubAccount_GetSASDevice(g_iothubAcctInfo);

    // arrange
    D2C_MESSAGE_HANDLE d2cMessageInitial = NULL;
    D2C_MESSAGE_HANDLE d2cMessageFaultInjection = NULL;
    D2C_MESSAGE_HANDLE d2cMessageDuringRetry = NULL;
    EXPECTED_RECEIVE_DATA* receiveUserContext = NULL;

    clear_connection_status_info_flags();

    // Create the IoT Hub Data
    client_connect_to_hub(deviceToUse, protocol);

    if ((0 == strcmp(faultOperationType, "KillAmqpCBSLinkReq")) || (0 == strcmp(faultOperationType, "KillAmqpCBSLinkResp")))
    {
        // We will only detect errors in CBS link when we attempt to refresh the token, which usually is quite long (see OPTION_SAS_TOKEN_LIFETIME).
        // We make the refresh time only 10 seconds so that the error is detected more quickly.
        size_t refresh_time = 10;
        setoption_on_device_or_module(OPTION_SAS_TOKEN_LIFETIME, (const void*)&refresh_time, "Failed setting OPTION_SAS_TOKEN_LIFETIME");
    }

    LogInfo("Sleeping 3 seconds to let SetMessageCallback() register with server.");
    ThreadAPI_Sleep(3000);
    LogInfo("Continue with service client message.");

    // Send the Event from the client
    LogInfo("Send message and wait for confirmation...");
    d2cMessageInitial = client_create_and_send_d2c(TEST_MESSAGE_CREATE_BYTE_ARRAY);

    // Wait for confirmation that the event was recevied
    bool dataWasRecv = client_wait_for_d2c_confirmation(d2cMessageInitial, IOTHUB_CLIENT_CONFIRMATION_OK);
    ASSERT_IS_TRUE(dataWasRecv, "Failure sending data to IotHub"); // was received by the callback...

    // Set callback.  This is required to create the D2C link (it's not created otherwise) that we'll get DETATCH message on.
    receiveUserContext = ReceiveUserContext_Create();
    setmessagecallback_on_device_or_module(receiveUserContext);

    // Send the Fault Control Event from the client
    LogInfo("Send server fault control message...");
    d2cMessageFaultInjection = send_error_injection_message(faultOperationType, faultOperationCloseReason, faultOperationDelayInSecs);

    LogInfo("Sleeping after sending fault injection...");
    ThreadAPI_Sleep(3000);
    LogInfo("Woke up after sending fault injection...");

    // Wait for connection status change (restored)
    LogInfo("wait for restore...");
    bool connStatus = client_wait_for_connection_restored();
    ASSERT_IS_TRUE(connStatus, "Fault injection failed - connection has not been restored");

    // Wait for connection status change (fault)
    LogInfo("wait for fault...");
    connStatus = client_wait_for_connection_fault();
    ASSERT_IS_TRUE(connStatus, "Fault injection failed - no fault happened");

    // Wait for connection status change (restored)
    LogInfo("wait for restore...");
    connStatus = client_wait_for_connection_restored();
    ASSERT_IS_TRUE(connStatus, "Fault injection failed - connection has not been restored");

    // Send the Event from the client
    LogInfo("Send message after the server fault and wait for confirmation...");
    d2cMessageDuringRetry = client_create_and_send_d2c(TEST_MESSAGE_CREATE_BYTE_ARRAY);
    // Wait for confirmation that the event was recevied
    LogInfo("wait for d2c confirm...");
    dataWasRecv = client_wait_for_d2c_confirmation(d2cMessageDuringRetry, IOTHUB_CLIENT_CONFIRMATION_OK);
    ASSERT_IS_TRUE(dataWasRecv, "Failure sending data to IotHub"); // was received by the callback...

    // close the client connection
    destroy_on_device_or_module();

    // Wait for the message to arrive
    LogInfo("waiting for d2c arrive...");
    service_wait_for_d2c_event_arrival(deviceToUse, d2cMessageDuringRetry, MAX_SERVICE_EVENT_WAIT_TIME_SECONDS);

    // cleanup
    destroy_d2c_message_handle(d2cMessageInitial);
    destroy_d2c_message_handle(d2cMessageFaultInjection);
    destroy_d2c_message_handle(d2cMessageDuringRetry);
    ReceiveUserContext_Destroy(receiveUserContext);
}

// Simulates a fault occurring in end-to-end testing (with special opcodes forcing service failure on certain white-listed Hubs) and
// ability to recover after error.
void e2e_d2c_with_svc_fault_ctrl_error_message_callback(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol, const char* faultOperationType, const char* faultOperationCloseReason, const char* faultOperationDelayInSecs, bool setTimeoutOption, bool isMqtt)
{
    // Note we pass 'isMqtt' instead of (protocol==MQTT_Transport) because this takes an explicit dependency on MQTT libraries, which will not be present
    // for someone building AMQP only.
    IOTHUB_PROVISIONED_DEVICE* deviceToUse = IoTHubAccount_GetSASDevice(g_iothubAcctInfo);

    // arrange
    D2C_MESSAGE_HANDLE d2cMessageInitial = NULL;
    D2C_MESSAGE_HANDLE d2cMessageFaultInjection = NULL;
    D2C_MESSAGE_HANDLE d2cMessageDuringRetry = NULL;
    bool connStatus;

    clear_connection_status_info_flags();

    // Create the IoT Hub Data
    client_connect_to_hub(deviceToUse, protocol);

    if (setTimeoutOption)
    {
        setoption_on_device_or_module("event_send_timeout_secs", "30", "Failure setting transport timeout");
    }

    // Send the Event from the client
    LogInfo("Send message and wait for confirmation...");
    d2cMessageInitial = client_create_and_send_d2c(TEST_MESSAGE_CREATE_STRING);

    // Wait for confirmation that the event was recevied
    bool dataWasRecv = client_wait_for_d2c_confirmation(d2cMessageInitial, IOTHUB_CLIENT_CONFIRMATION_OK);
    ASSERT_IS_TRUE(dataWasRecv, "Failure sending data to IotHub"); // was received by the callback...

    // Send the Fault Control Event from the client
    LogInfo("Send server fault control message...");
    d2cMessageFaultInjection = send_error_injection_message(faultOperationType, faultOperationCloseReason, faultOperationDelayInSecs);

    LogInfo("Sleeping after sending fault injection...");
    ThreadAPI_Sleep(3000);

    LogInfo("Sending message and expect no confirmation...");
    d2cMessageDuringRetry = client_create_and_send_d2c(TEST_MESSAGE_CREATE_STRING);

    if (isMqtt)
    {
        // MQTT gets disconnects (not error messages), though it'll auto-reconnect.  Make sure that happens.
        LogInfo("wait for fault...");
        connStatus = client_wait_for_connection_fault();
        ASSERT_IS_TRUE(connStatus, "Fault injection failed - no fault happened");

        // Wait for connection status change (restored)
        LogInfo("wait for restore...");
        connStatus = client_wait_for_connection_restored();
        ASSERT_IS_TRUE(connStatus, "Fault injection failed - connection has not been restored");
    }

    // AMQP fault injection tests persist the fact an error occurred on server and mean the test gets this back.  MQTT fault injection on server is more stateless; we'll
    // reconnect automatically after error but server will have us succeed.
    dataWasRecv = client_wait_for_d2c_confirmation(d2cMessageDuringRetry, isMqtt ? IOTHUB_CLIENT_CONFIRMATION_OK : IOTHUB_CLIENT_CONFIRMATION_ERROR);
    ASSERT_IS_TRUE(dataWasRecv, "Failure getting response from IoT Hub..."); // was received by the callback...

    // close the client connection
    destroy_on_device_or_module();

    // cleanup
    destroy_d2c_message_handle(d2cMessageInitial);
    destroy_d2c_message_handle(d2cMessageFaultInjection);
    destroy_d2c_message_handle(d2cMessageDuringRetry);
}

EXPECTED_RECEIVE_DATA *service_create_c2d(const char *content)
{
    IOTHUB_MESSAGE_RESULT iotHubMessageResult;
    IOTHUB_MESSAGE_HANDLE messageHandle;

    EXPECTED_RECEIVE_DATA *receiveUserContext = ReceiveUserContext_Create();
    ASSERT_IS_NOT_NULL(receiveUserContext, "Could not create receive user context");

    if (receiveUserContext != NULL)
    {
        messageHandle = IoTHubMessage_CreateFromString(content);
        ASSERT_IS_NOT_NULL(messageHandle, "Could not create IoTHubMessage to send C2D messages to the device");

        if (g_e2e_test_options.use_special_chars)
        {
            GenerateUniqueId(MSG_ID_SPECIAL, sizeof(MSG_ID_SPECIAL), receiveUserContext->message_id, sizeof(receiveUserContext->message_id));
            iotHubMessageResult = IoTHubMessage_SetMessageId(messageHandle, receiveUserContext->message_id);
        }
        else
        {
            GenerateUniqueId(MSG_ID, sizeof(MSG_ID), receiveUserContext->message_id, sizeof(receiveUserContext->message_id));
            iotHubMessageResult = IoTHubMessage_SetMessageId(messageHandle, receiveUserContext->message_id);

        }
        ASSERT_ARE_EQUAL(int, IOTHUB_MESSAGING_OK, iotHubMessageResult);
        if (g_e2e_test_options.use_special_chars)
        {
            GenerateUniqueId(MSG_CORRELATION_ID_SPECIAL, sizeof(MSG_CORRELATION_ID_SPECIAL), receiveUserContext->correlation_id, sizeof(receiveUserContext->correlation_id));
            iotHubMessageResult = IoTHubMessage_SetCorrelationId(messageHandle, receiveUserContext->correlation_id);
        }
        else
        {
            GenerateUniqueId(MSG_CORRELATION_ID, sizeof(MSG_CORRELATION_ID), receiveUserContext->correlation_id, sizeof(receiveUserContext->correlation_id));
            iotHubMessageResult = IoTHubMessage_SetCorrelationId(messageHandle, receiveUserContext->correlation_id);
        }
        ASSERT_ARE_EQUAL(int, IOTHUB_MESSAGING_OK, iotHubMessageResult);

        MAP_HANDLE mapHandle = IoTHubMessage_Properties(messageHandle);
        for (size_t i = 0; i < MSG_PROP_COUNT; i++)
        {
            if (g_e2e_test_options.use_special_chars)
            {
                ASSERT_ARE_EQUAL(MAP_RESULT, MAP_OK, Map_AddOrUpdate(mapHandle, MSG_PROP_KEYS_SPECIAL[i], MSG_PROP_VALS_SPECIAL[i]));
            }
            else
            {
                ASSERT_ARE_EQUAL(MAP_RESULT, MAP_OK, Map_AddOrUpdate(mapHandle, MSG_PROP_KEYS[i], MSG_PROP_VALS[i]));
            }
        }

        receiveUserContext->msgHandle = messageHandle;
    }
    return receiveUserContext;
}

static void service_send_c2d(IOTHUB_MESSAGING_CLIENT_HANDLE iotHubMessagingHandle, EXPECTED_RECEIVE_DATA* receiveUserContext, IOTHUB_PROVISIONED_DEVICE* deviceToUse)
{
    IOTHUB_MESSAGING_RESULT iotHubMessagingResult;

    if (deviceToUse->moduleId)
    {
        iotHubMessagingResult = IOTHUB_MESSAGING_ERROR;
        ASSERT_FAIL("modules are not supported for sending messages");
    }
    else
    {
        iotHubMessagingResult = IoTHubMessaging_SendAsync(iotHubMessagingHandle, deviceToUse->deviceId, receiveUserContext->msgHandle, sendCompleteCallback, receiveUserContext);
    }

    ASSERT_ARE_EQUAL(int, IOTHUB_MESSAGING_OK, iotHubMessagingResult, "IoTHubMessaging_SendAsync failed, could not send C2D message to the device");
}

void client_wait_for_c2d_event_arrival(EXPECTED_RECEIVE_DATA* receiveUserContext)
{
    time_t beginOperation, nowTime;

    beginOperation = time(NULL);
    while (
        (nowTime = time(NULL)), (difftime(nowTime, beginOperation) < MAX_CLOUD_TRAVEL_TIME) //time box
        )
    {
        ASSERT_ARE_EQUAL(LOCK_RESULT, LOCK_OK, Lock(receiveUserContext->lock));
        if (receiveUserContext->wasFound)
        {
            (void)Unlock(receiveUserContext->lock);
            break;
        }
        (void)Unlock(receiveUserContext->lock);
        ThreadAPI_Sleep(100);
    }

}

static IOTHUB_MESSAGING_RESULT IoTHubMessaging_Open_with_retry(IOTHUB_MESSAGING_CLIENT_HANDLE messagingClientHandle, IOTHUB_OPEN_COMPLETE_CALLBACK openCompleteCallback, void* userContextCallback)
{
    IOTHUB_MESSAGING_RESULT result = IOTHUB_MESSAGING_ERROR;
    for (int i = 0; i < RETRY_COUNT; i++)
    {
        result = IoTHubMessaging_Open(messagingClientHandle, openCompleteCallback, userContextCallback);
        if (result == IOTHUB_MESSAGING_OK)
        {
            break;
        }
        ThreadAPI_Sleep(1000 * RETRY_DELAY_SECONDS);
    }
    return result;
}

static void recv_message_test(IOTHUB_PROVISIONED_DEVICE* deviceToUse, IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    // arrange
    IOTHUB_SERVICE_CLIENT_AUTH_HANDLE iotHubServiceClientHandle;
    IOTHUB_MESSAGING_CLIENT_HANDLE iotHubMessagingHandle;

    IOTHUB_MESSAGING_RESULT iotHubMessagingResult;
    size_t client_conn_wait_time = 4000;

    EXPECTED_RECEIVE_DATA* receiveUserContext;

    clear_connection_status_info_flags();

    // Create device client
    client_connect_to_hub(deviceToUse, protocol);

    // Make sure we have a connection
    ASSERT_IS_TRUE(client_wait_for_connection_restored(), "Connection Callback has not been called");

    // Create receive context
    const char* msg_content;
    if (g_e2e_test_options.use_special_chars)
    {
        msg_content = MSG_CONTENT;
    }
    else
    {
        msg_content = MSG_CONTENT_SPECIAL;
    }

    receiveUserContext = service_create_c2d(msg_content);

    // Set callback
    setmessagecallback_on_device_or_module(receiveUserContext);

    if (test_protocol_type == TEST_HTTP)
    {
        // Http protocol does not have a connection callback, so we just need to wait here
        ThreadAPI_Sleep(2000);
    }
    else
    {
        // Let the iothub client establish the connection
        if (!wait_for_client_authenticated(client_conn_wait_time))
        {
            // In some situations this will pass due to the device already being connected
            // Or being amqp.  Make sure we flag this as a possible situation and continue
            LogInfo("Did not recieve the client connection callback within the alloted time <%lu> seconds", (unsigned long)client_conn_wait_time);
        }
        // Make sure we subscribe to all the events
        ThreadAPI_Sleep(3000);
    }

    // Create Service Client
    iotHubServiceClientHandle = IoTHubServiceClientAuth_CreateFromConnectionString(IoTHubAccount_GetIoTHubConnString(g_iothubAcctInfo));
    ASSERT_IS_NOT_NULL(iotHubServiceClientHandle, "Could not initialize IoTHubServiceClient to send C2D messages to the device");

    iotHubMessagingHandle = IoTHubMessaging_Create(iotHubServiceClientHandle);
    ASSERT_IS_NOT_NULL(iotHubMessagingHandle, "Could not initialize IoTHubMessaging to send C2D messages to the device");

#ifdef SET_TRUSTED_CERT_IN_SAMPLES
    ASSERT_ARE_EQUAL(int, IOTHUB_MESSAGING_OK, IoTHubMessaging_SetTrustedCert(iotHubMessagingHandle, certificates));
#endif // SET_TRUSTED_CERT_IN_SAMPLES

    iotHubMessagingResult = IoTHubMessaging_Open_with_retry(iotHubMessagingHandle, openCompleteCallback, (void*)"Context string for open");
    ASSERT_ARE_EQUAL(int, IOTHUB_MESSAGING_OK, iotHubMessagingResult);

    // Send message
    service_send_c2d(iotHubMessagingHandle, receiveUserContext, deviceToUse);

    // wait for message to arrive on client
    client_wait_for_c2d_event_arrival(receiveUserContext);
    if (receiveUserContext->wasFound == false)
    {
        LogError("C2D message not received. message_id: %s, correlation_id: %s", receiveUserContext->message_id, receiveUserContext->correlation_id);
    }

    // assert
    ASSERT_IS_TRUE(receiveUserContext->wasFound, "Failure retrieving data from C2D"); // was found is written by the callback...

    // cleanup
    IoTHubMessaging_Close(iotHubMessagingHandle);
    IoTHubMessaging_Destroy(iotHubMessagingHandle);
    IoTHubServiceClientAuth_Destroy(iotHubServiceClientHandle);

    destroy_on_device_or_module();
    ReceiveUserContext_Destroy(receiveUserContext);
}

void e2e_recv_message_test_sas(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    recv_message_test(IoTHubAccount_GetSASDevice(g_iothubAcctInfo), protocol);
}

void e2e_recv_message_test_x509(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    recv_message_test(IoTHubAccount_GetX509Device(g_iothubAcctInfo), protocol);
}

void e2e_c2d_with_svc_fault_ctrl(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol, const char* faultOperationType, const char* faultOperationCloseReason, const char* faultOperationDelayInSecs)
{
    // arrange
    IOTHUB_SERVICE_CLIENT_AUTH_HANDLE iotHubServiceClientHandle;
    IOTHUB_MESSAGING_CLIENT_HANDLE iotHubMessagingHandle;

    IOTHUB_MESSAGING_RESULT iotHubMessagingResult;

    EXPECTED_RECEIVE_DATA* receiveUserContext;

    D2C_MESSAGE_HANDLE d2cMessage;
    IOTHUB_PROVISIONED_DEVICE* deviceToUse = IoTHubAccount_GetSASDevice(g_iothubAcctInfo);

    clear_connection_status_info_flags();

    // Create device client
    client_connect_to_hub(deviceToUse, protocol);

    // Make sure we have a connection
    ASSERT_IS_TRUE(client_wait_for_connection_restored(), "Connection Callback has not been called");

    // Create receive context
    receiveUserContext = service_create_c2d(MSG_CONTENT);

    // Set callback
    setmessagecallback_on_device_or_module(receiveUserContext);

    LogInfo("Sleeping 3 seconds to let SetMessageCallback() register with server.");
    ThreadAPI_Sleep(3000);
    LogInfo("Continue with service client message.");

    // Create Service Client
    iotHubServiceClientHandle = IoTHubServiceClientAuth_CreateFromConnectionString(IoTHubAccount_GetIoTHubConnString(g_iothubAcctInfo));
    ASSERT_IS_NOT_NULL(iotHubServiceClientHandle, "Could not initialize IoTHubServiceClient to send C2D messages to the device");

    iotHubMessagingHandle = IoTHubMessaging_Create(iotHubServiceClientHandle);
    ASSERT_IS_NOT_NULL(iotHubMessagingHandle, "Could not initialize IoTHubMessaging to send C2D messages to the device");

    iotHubMessagingResult = IoTHubMessaging_Open_with_retry(iotHubMessagingHandle, openCompleteCallback, (void*)"Context string for open");
    ASSERT_ARE_EQUAL(int, IOTHUB_MESSAGING_OK, iotHubMessagingResult);

    // Send message
    service_send_c2d(iotHubMessagingHandle, receiveUserContext, deviceToUse);

    // wait for message to arrive on client
    client_wait_for_c2d_event_arrival(receiveUserContext);

    // assert
    ASSERT_ARE_EQUAL(LOCK_RESULT, LOCK_OK, Lock(receiveUserContext->lock));
    ASSERT_IS_TRUE(receiveUserContext->wasFound, "Failure retrieving data from C2D"); // was found is written by the callback...
    Unlock(receiveUserContext->lock);

    LogInfo("Send server fault control message...");
    d2cMessage = send_error_injection_message(faultOperationType, faultOperationCloseReason, faultOperationDelayInSecs);

    LogInfo("Sleeping after sending fault injection...");
    ThreadAPI_Sleep(3000);

    // Send message

    ASSERT_ARE_EQUAL(LOCK_RESULT, LOCK_OK, Lock(receiveUserContext->lock));
    receiveUserContext->wasFound = false;
    Unlock(receiveUserContext->lock);

    service_send_c2d(iotHubMessagingHandle, receiveUserContext, deviceToUse);
    // wait for message to arrive on client
    client_wait_for_c2d_event_arrival(receiveUserContext);
    // assert
    ASSERT_ARE_EQUAL(LOCK_RESULT, LOCK_OK, Lock(receiveUserContext->lock));
    ASSERT_IS_TRUE(receiveUserContext->wasFound, "Failure retrieving data from C2D"); // was found is written by the callback...
    Unlock(receiveUserContext->lock);

    // cleanup
    IoTHubMessaging_Close(iotHubMessagingHandle);
    IoTHubMessaging_Destroy(iotHubMessagingHandle);
    IoTHubServiceClientAuth_Destroy(iotHubServiceClientHandle);

    destroy_on_device_or_module();
    ReceiveUserContext_Destroy(receiveUserContext);

    destroy_d2c_message_handle(d2cMessage);
}


//***********************************************************
// D2C
//***********************************************************
void e2e_d2c_svc_fault_ctrl_kill_TCP_connection(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_d2c_with_svc_fault_ctrl(protocol, "KillTcp", "boom", "1");
}

void e2e_d2c_svc_fault_ctrl_kill_TCP_connection_with_transport_status_check(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_d2c_with_svc_fault_ctrl_with_transport_status(protocol, "KillTcp", "boom", "1");
}

void e2e_d2c_svc_fault_ctrl_AMQP_kill_connection(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_d2c_with_svc_fault_ctrl_with_transport_status(protocol, "KillAmqpConnection", "Connection fault", "1");
}

void e2e_d2c_svc_fault_ctrl_AMQP_kill_session(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_d2c_with_svc_fault_ctrl_with_transport_status(protocol, "KillAmqpSession", "Session fault", "1");
}

void e2e_d2c_svc_fault_ctrl_AMQP_kill_CBS_request_link(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_d2c_with_svc_fault_ctrl_with_transport_status(protocol, "KillAmqpCBSLinkReq", "CBS request link fault", "1");
}

void e2e_d2c_svc_fault_ctrl_AMQP_kill_CBS_response_link(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_d2c_with_svc_fault_ctrl_with_transport_status(protocol, "KillAmqpCBSLinkResp", "CBS response link fault", "1");
}

void e2e_d2c_svc_fault_ctrl_AMQP_kill_D2C_link(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_d2c_with_svc_fault_ctrl_with_transport_status(protocol, "KillAmqpD2CLink", "D2C link fault", "1");
}

void e2e_d2c_svc_fault_ctrl_AMQP_kill_C2D_link(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_d2c_with_svc_fault_ctrl_with_transport_status(protocol, "KillAmqpC2DLink", "C2D link fault", "1");
}

void e2e_d2c_svc_fault_ctrl_AMQP_throttling_reconnect(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_d2c_with_svc_fault_ctrl_error_message_callback(protocol, "InvokeThrottling", "20", "1", true, false);
}

void e2e_d2c_svc_fault_ctrl_AMQP_message_quota_exceeded(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_d2c_with_svc_fault_ctrl_error_message_callback(protocol, "InvokeMaxMessageQuota", "20", "1", true, false);
}

void e2e_d2c_svc_fault_ctrl_AMQP_auth_error(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_d2c_with_svc_fault_ctrl_error_message_callback(protocol, "InvokeAuthError", "20", "1", true, false);
}

void e2e_d2c_svc_fault_ctrl_AMQP_shut_down(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_d2c_with_svc_fault_ctrl_with_transport_status(protocol, "ShutDownAmqp", "byebye", "1");
}

void e2e_d2c_svc_fault_ctrl_MQTT_throttling_reconnect(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_d2c_with_svc_fault_ctrl_error_message_callback(protocol, "InvokeThrottling", "20", "1", false, true);
}

void e2e_d2c_svc_fault_ctrl_MQTT_message_quota_exceeded(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_d2c_with_svc_fault_ctrl_error_message_callback(protocol, "InvokeMaxMessageQuota", "20", "1", false, true);
}

void e2e_d2c_svc_fault_ctrl_MQTT_auth_error(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_d2c_with_svc_fault_ctrl_error_message_callback(protocol, "InvokeAuthError", "20", "1", false, true);
}

void e2e_d2c_svc_fault_ctrl_MQTT_shut_down(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_d2c_with_svc_fault_ctrl_with_transport_status(protocol, "ShutDownMqtt", "byebye", "1");
}

//***********************************************************
// C2D
//***********************************************************
void e2e_c2d_svc_fault_ctrl_kill_TCP_connection(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_c2d_with_svc_fault_ctrl(protocol, "KillTcp", "boom", "1");
}

void e2e_c2d_svc_fault_ctrl_AMQP_kill_connection(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_c2d_with_svc_fault_ctrl(protocol, "KillAmqpConnection", "Connection fault", "1");
}

void e2e_c2d_svc_fault_ctrl_AMQP_kill_session(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_c2d_with_svc_fault_ctrl(protocol, "KillAmqpSession", "Session fault", "1");
}

void e2e_c2d_svc_fault_ctrl_AMQP_kill_CBS_request_link(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_c2d_with_svc_fault_ctrl(protocol, "KillAmqpCBSLinkReq", "CBS request link fault", "1");
}

void e2e_c2d_svc_fault_ctrl_AMQP_kill_CBS_response_link(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_c2d_with_svc_fault_ctrl(protocol, "KillAmqpCBSLinkResp", "CBS response link fault", "1");
}

void e2e_c2d_svc_fault_ctrl_AMQP_kill_D2C_link(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_c2d_with_svc_fault_ctrl(protocol, "KillAmqpc2dLink", "c2d link fault", "1");
}

void e2e_c2d_svc_fault_ctrl_AMQP_kill_C2D_link(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_c2d_with_svc_fault_ctrl(protocol, "KillAmqpC2DLink", "C2D link fault", "1");
}

void e2e_c2d_svc_fault_ctrl_AMQP_shut_down(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_c2d_with_svc_fault_ctrl(protocol, "ShutDownAmqp", "byebye", "1");
}

void e2e_c2d_svc_fault_ctrl_throttling_reconnect(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_c2d_with_svc_fault_ctrl(protocol, "InvokeThrottling", "20", "1");
}

void e2e_c2d_svc_fault_ctrl_message_quota_exceeded(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_c2d_with_svc_fault_ctrl(protocol, "InvokeMaxMessageQuota", "20", "1");
}

void e2e_c2d_svc_fault_ctrl_auth_error(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_c2d_with_svc_fault_ctrl(protocol, "InvokeAuthError", "20", "1");
}

void e2e_c2d_svc_fault_ctrl_MQTT_shut_down(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    e2e_c2d_with_svc_fault_ctrl(protocol, "ShutDownMqtt", "byebye", "1");
}
