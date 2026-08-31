#include "device_schema.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "cbor_codec.h"

bool schema_valid_command_name(const char *command)
{
    size_t length = strnlen(command, GW_MSG_COMMAND_LEN);
    if (length == 0 || length >= GW_MSG_COMMAND_LEN) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)command[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}

bool schema_valid_tool(const device_schema_tool_t *tool)
{
    if (!schema_valid_command_name(tool->command) ||
        tool->value_type > 2 /* NONE, BOOL, INT */) {
        return false;
    }
    if (tool->value_type == 2 /* INT */) {
        if (tool->min_value > tool->max_value || tool->step == 0) {
            return false;
        }
    }
    return true;
}

bool schema_tool_equal(const device_schema_tool_t *a,
                       const device_schema_tool_t *b)
{
    return a->value_type == b->value_type &&
           a->flags == b->flags &&
           a->min_value == b->min_value &&
           a->max_value == b->max_value &&
           a->step == b->step &&
           strcmp(a->command, b->command) == 0 &&
           strcmp(a->label, b->label) == 0 &&
           strcmp(a->unit, b->unit) == 0;
}

bool schema_valid_feature_id(const char *feature_id)
{
    size_t length = strnlen(feature_id, GW_FEATURE_ID_LEN);
    return length > 0 && length < GW_FEATURE_ID_LEN;
}

int8_t schema_resolve_writable_tool(const device_schema_tool_t *tools,
                                     size_t tool_count,
                                     const char *feature_tool)
{
    if (feature_tool == NULL || feature_tool[0] == '\0') return -1;
    for (size_t i = 0; i < tool_count; i++) {
        if (strcmp(tools[i].command, feature_tool) == 0) {
            return (int8_t)i;
        }
    }
    return -1;
}
