# Phase G0 — Cross-Repository Golden Vectors

**Document set:** ESP32 BLE Gateway ↔ Device Settings v2  
**Version:** v2.1 phased documents  
**Date:** 2026-09-07  
**Gateway repo:** `hailp-vn38/esp-ble-gateway`  
**Gateway branch:** `dev-device-settings`  
**Device repo:** `hailp-vn38/esp-ble-device`  
**Device branch:** `main`  
**Protocol:** ESP-GATT Protocol v4  

---

## Protocol constants used by this phase

Canonical Settings keys:

```c
GW_KEY_SETTINGS_SUPPORTED          = 32,
GW_KEY_SETTINGS_SCHEMA_REVISION    = 33,
GW_KEY_SETTINGS_ID                 = 34,
GW_KEY_SETTINGS_TITLE              = 35,
GW_KEY_SETTINGS_GROUP              = 36,
GW_KEY_SETTINGS_UNIT               = 37,
GW_KEY_SETTINGS_TYPE               = 38,
GW_KEY_SETTINGS_FLAGS              = 39,
GW_KEY_SETTINGS_VALUE              = 40,
GW_KEY_SETTINGS_TRANSACTION_ID     = 41,
GW_KEY_SETTINGS_EXPECTED_REVISION  = 42,
GW_KEY_SETTINGS_NEW_REVISION       = 43,
GW_KEY_SETTINGS_OPTION_INDEX       = 44,
GW_KEY_SETTINGS_MAX_LENGTH         = 45,
GW_KEY_SETTINGS_OPTION_COUNT       = 46,
GW_KEY_SETTINGS_SEQUENCE           = 47,
```

Canonical Settings wire types:

```c
GW_SETTING_TYPE_NONE   = 0,
GW_SETTING_TYPE_BOOL   = 1,
GW_SETTING_TYPE_INT    = 2,
GW_SETTING_TYPE_FLOAT  = 3,
GW_SETTING_TYPE_STRING = 4,
GW_SETTING_TYPE_ENUM   = 5,
```

Canonical Settings flags:

```c
GW_SETTING_FLAG_READONLY = 1 << 0
GW_SETTING_FLAG_SECRET   = 1 << 1
GW_SETTING_FLAG_ADVANCED = 1 << 2
```

Do not create a second private key table in Gateway or Device.


# 1. Objective

Freeze actual bytes that both repositories must accept.

This phase begins only after D0 exit gate passes.

# 2. Why helper-only fixtures are insufficient

A codec helper can be correct while its caller passes the wrong type.

Example:

```text
helper test:
gw_settings_encode_item(... GW_SETTING_TYPE_INT ...)
PASS

runtime:
gw_settings_encode_item(... (uint8_t)DEVICE_SETTING_INT ...)
wrong wire type
```

Therefore vectors require two sources:

```text
Layer A: direct codec vectors
Layer B: actual Device command pipeline output
```

# 3. Fixture ownership

Recommended canonical source repository:

```text
esp-ble-device/test/fixtures/settings_v2/
```

Gateway checks in an identical copy or consumes generated artifacts in CI.

Do not generate expected bytes independently in both repos.

# 4. Required Device→Gateway vectors

```text
capabilities_settings_supported
settings_begin
settings_item_bool
settings_item_int
settings_item_string
settings_item_enum
settings_option_item_0
settings_option_item_1
settings_end
settings_values_begin
settings_value_bool
settings_value_int
settings_value_string
settings_value_enum
settings_values_end
describe_ack
read_ack
```

# 5. Required Gateway→Device vectors

```text
describe_settings
read_settings
settings_tx_begin
settings_tx_set_bool
settings_tx_set_int
settings_tx_set_string
settings_tx_set_enum
settings_tx_commit
settings_tx_abort
settings_commit_confirm
```

# 6. Fixture format

Recommended C representation:

```c
typedef struct {
    const char *name;
    const uint8_t *bytes;
    size_t len;
} settings_fixture_t;
```

Example file:

```text
settings_v2_vectors.h
settings_v2_vectors.c
```

Optional machine-readable companion:

```text
settings_v2_vectors.hex
```

# 7. Device actual-pipeline capture

Inject/mock notify function.

Test flow:

```text
construct canonical Device descriptors
register descriptors
build Gateway request
run real Device command handler
capture each notify frame
compare with decoded semantic expectation
save canonical bytes
```

Do not rely on serial logs as fixture source.

# 8. Gateway decode test

For every Device vector:

```c
gw_message_t msg;
int rc = gateway_decode(fixture.bytes, fixture.len, &msg);
TEST_ASSERT_EQUAL(ESP_OK, rc);
```

Then assert exact semantics.

# 9. Device decode test

For every Gateway request vector:

```c
gw_message_t msg;
TEST_ASSERT_EQUAL(GW_OK,
    gw_message_decode(fixture.bytes, fixture.len, &msg));
```

For transaction commands additionally call specialized Settings decode.

# 10. Versioning rule

Fixture update requires intentional protocol/runtime change.

Recommended metadata:

```c
#define SETTINGS_FIXTURE_PROTOCOL_VERSION 4
#define SETTINGS_FIXTURE_SCHEMA_REVISION  1
```

# 11. CI rule

```text
Device CI:
  actual runtime output matches canonical semantic assertions

Gateway CI:
  decodes every canonical Device vector

Gateway CI:
  encodes every canonical request vector

Device CI:
  decodes every canonical Gateway request vector
```

# 12. Logging


## Logging requirements

Use the following levels:

```text
INFO    operation begin/end, ACK, state transition
DEBUG   per schema item/value/option
WARN    retry, reject, mismatch
ERROR   terminal operation failure
VERBOSE optional bounded CBOR hexdump
```

Never log:

```text
secret plaintext
Wi-Fi password
credential/token
full normal STRING value by default
```

Normal STRING values should log only:

```text
len=N
```

SECRET values:

```text
value=<redacted>
```

All Settings operations should be traceable using:

```text
device_id
request_id
command
sequence/total where applicable
transaction_id where applicable
revision where applicable
```


Golden-vector tests should not compare full human-formatted logs.

Instead expose semantic instrumentation hooks if needed.

# 13. Exit gate

```text
[ ] all Device→Gateway fixtures frozen
[ ] all Gateway→Device fixtures frozen
[ ] Gateway decodes all Device fixtures
[ ] Device decodes all Gateway fixtures
[ ] actual Device pipeline used for runtime vectors
[ ] no fixture is generated from the implementation under test inside the same assertion
```
