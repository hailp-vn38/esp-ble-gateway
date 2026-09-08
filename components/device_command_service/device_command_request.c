#include "device_command_service_internal.h"

#include <string.h>

#include "device_schema.h"
#include "gw_settings_view.h"

static bool settings_string_is_terminated(const char *value, size_t capacity)
{
    return strnlen(value, capacity) < capacity;
}

static bool settings_value_is_valid(const device_command_settings_payload_t *settings)
{
    if (!settings->has_setting_value) {
        return false;
    }
    switch (settings->setting_type) {
    case GW_SETTING_TYPE_BOOL:
    case GW_SETTING_TYPE_INT:
    case GW_SETTING_TYPE_ENUM:
        return true;
    case GW_SETTING_TYPE_STRING:
        return settings_string_is_terminated(settings->value.string_value,
                                             sizeof(settings->value.string_value));
    default:
        return false;
    }
}

static bool settings_id_is_valid(const device_command_settings_payload_t *settings)
{
    return settings->has_setting_id && settings->setting_id[0] != '\0' &&
           settings_string_is_terminated(settings->setting_id,
                                         sizeof(settings->setting_id));
}

static bool is_settings_command(const char *command)
{
    return strcmp(command, GW_SETTINGS_CMD_DESCRIBE_SETTINGS) == 0 ||
           strcmp(command, GW_SETTINGS_CMD_READ_SETTINGS) == 0 ||
           strcmp(command, GW_SETTINGS_CMD_TX_BEGIN) == 0 ||
           strcmp(command, GW_SETTINGS_CMD_TX_SET) == 0 ||
           strcmp(command, GW_SETTINGS_CMD_TX_COMMIT) == 0 ||
           strcmp(command, GW_SETTINGS_CMD_TX_ABORT) == 0 ||
           strcmp(command, GW_SETTINGS_CMD_COMMIT_CONFIRM) == 0;
}

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
    const device_command_settings_payload_t *settings = &request->settings;
    if (settings->has_transaction_id) {
        message->settings_transaction_id = settings->transaction_id;
        message->has_settings_transaction_id = 1;
    }
    if (settings->has_expected_revision) {
        message->settings_expected_revision = settings->expected_revision;
        message->has_settings_expected_revision = 1;
    }
    if (settings->has_new_revision) {
        message->settings_new_revision = settings->new_revision;
        message->has_settings_new_revision = 1;
    }
    if (settings->has_setting_id) {
        strlcpy(message->setting_id, settings->setting_id,
                sizeof(message->setting_id));
        message->has_setting_id = 1;
    }
    if (settings->has_setting_value) {
        message->setting_type = settings->setting_type;
        message->has_setting_type = 1;
        message->has_setting_value = 1;
        switch (settings->setting_type) {
        case GW_SETTING_TYPE_BOOL:
            message->setting_value.setting_value_bool = settings->value.bool_value;
            break;
        case GW_SETTING_TYPE_INT:
            message->setting_value.setting_value_int = settings->value.int_value;
            break;
        case GW_SETTING_TYPE_ENUM:
            message->setting_value.setting_value_enum = settings->value.enum_value;
            break;
        case GW_SETTING_TYPE_STRING:
            strlcpy(message->setting_value.setting_value_string,
                    settings->value.string_value,
                    sizeof(message->setting_value.setting_value_string));
            break;
        default:
            break;
        }
    }
}

device_command_status_t dcs_validate_request(
    const device_command_request_t *request, const gw_message_t *wire_message)
{
    if (request == NULL || wire_message == NULL ||
        request->device_id[0] == '\0') {
        return DEVICE_CMD_STATUS_INVALID_ARGUMENT;
    }
    switch (request->origin) {
    case DEVICE_CMD_ORIGIN_CONTROL: {
        if (request->command[0] == '\0') {
            return DEVICE_CMD_STATUS_INVALID_ARGUMENT;
        }
        device_schema_validation_t validation =
            device_schema_validate_command(wire_message, NULL);
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
    case DEVICE_CMD_ORIGIN_SETTINGS:
        if (!is_settings_command(request->command)) {
            return DEVICE_CMD_STATUS_UNSUPPORTED_COMMAND;
        }
        if (request->has_bool_value || request->has_int_value ||
            request->has_feature_id || request->has_property_id) {
            return DEVICE_CMD_STATUS_INVALID_ARGUMENT;
        }
        if (strcmp(request->command, GW_SETTINGS_CMD_TX_BEGIN) == 0) {
            if (!request->settings.has_transaction_id ||
                !request->settings.has_expected_revision) {
                return DEVICE_CMD_STATUS_INVALID_ARGUMENT;
            }
        } else if (strcmp(request->command, GW_SETTINGS_CMD_TX_SET) == 0) {
            if (!request->settings.has_transaction_id ||
                !settings_id_is_valid(&request->settings) ||
                !settings_value_is_valid(&request->settings)) {
                return DEVICE_CMD_STATUS_INVALID_ARGUMENT;
            }
        } else if (strcmp(request->command, GW_SETTINGS_CMD_TX_COMMIT) == 0 ||
                   strcmp(request->command, GW_SETTINGS_CMD_TX_ABORT) == 0) {
            if (!request->settings.has_transaction_id) {
                return DEVICE_CMD_STATUS_INVALID_ARGUMENT;
            }
        } else if (strcmp(request->command, GW_SETTINGS_CMD_COMMIT_CONFIRM) == 0) {
            if (!request->settings.has_transaction_id ||
                !request->settings.has_new_revision) {
                return DEVICE_CMD_STATUS_INVALID_ARGUMENT;
            }
        }
        break;
    default:
        return DEVICE_CMD_STATUS_INVALID_ARGUMENT;
    }
    return DEVICE_CMD_STATUS_OK;
}
