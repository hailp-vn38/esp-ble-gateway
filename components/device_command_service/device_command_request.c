#include "device_command_service_internal.h"

#include <string.h>

#include "device_schema.h"

void dcs_build_wire_message(const device_command_request_t *request,
                            uint32_t request_id, gw_message_t *message)
{
    memset(message, 0, sizeof(*message));
    message->protocol_version = GW_PROTOCOL_VERSION;
    strlcpy(message->type, "device_command", sizeof(message->type));
    strlcpy(message->device_id, request->device_id, sizeof(message->device_id));
    strlcpy(message->command, request->command, sizeof(message->command));
    message->has_device_id = 1;
    message->request_id = request_id;
    message->has_request_id = 1;
    if (request->has_bool_value) {
        message->bool_value = request->bool_value ? 1 : 0;
        message->has_bool_value = 1;
    }
    if (request->has_int_value) {
        message->int_value = request->int_value;
        message->has_int_value = 1;
    }
    if (request->has_feature_id) {
        strlcpy(message->feature_id, request->feature_id, sizeof(message->feature_id));
        message->has_feature_id = 1;
    }
    if (request->has_property_id) {
        message->property_id = request->property_id;
        message->has_property_id = 1;
    }
}

device_command_status_t dcs_validate_request(const device_command_request_t *request)
{
    if (request->device_id[0] == '\0') {
        return DEVICE_CMD_STATUS_INVALID_ARGUMENT;
    }
    switch (request->origin) {
    case DEVICE_CMD_ORIGIN_CONTROL: {
        if (request->command[0] == '\0') {
            return DEVICE_CMD_STATUS_INVALID_ARGUMENT;
        }
        gw_message_t message;
        dcs_build_wire_message(request, 0, &message);
        message.has_request_id = 0;
        device_schema_validation_t validation =
            device_schema_validate_command(&message, NULL);
        if (validation == DEVICE_SCHEMA_VALID_UNKNOWN) {
            return DEVICE_CMD_STATUS_SCHEMA_NOT_READY;
        }
        if (validation == DEVICE_SCHEMA_VALID_UNSUPPORTED_COMMAND) {
            return DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND;
        }
        if (validation == DEVICE_SCHEMA_VALID_TYPE_MISMATCH) {
            return DEVICE_CMD_STATUS_TYPE_MISMATCH;
        }
        if (validation == DEVICE_SCHEMA_VALID_RANGE_ERROR) {
            return DEVICE_CMD_STATUS_RANGE_ERROR;
        }
        if (validation == DEVICE_SCHEMA_VALID_ARGUMENT) {
            return DEVICE_CMD_STATUS_INVALID_ARGUMENT;
        }
        if (validation != DEVICE_SCHEMA_VALID) {
            return DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND;
        }
        break;
    }
    case DEVICE_CMD_ORIGIN_SCHEMA_DISCOVERY:
        if (strcmp(request->command, "describe_capabilities") != 0) {
            return DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND;
        }
        break;
    case DEVICE_CMD_ORIGIN_STATE_READ:
        if (strcmp(request->command, "read_feature_state") != 0) {
            return DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND;
        }
        if (!request->has_feature_id || request->feature_id[0] == '\0' ||
            !request->has_property_id) {
            return DEVICE_CMD_STATUS_INVALID_ARGUMENT;
        }
        break;
    default:
        return DEVICE_CMD_STATUS_INVALID_ARGUMENT;
    }
    return DEVICE_CMD_STATUS_OK;
}
