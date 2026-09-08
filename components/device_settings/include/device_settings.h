#ifndef DEVICE_SETTINGS_H
#define DEVICE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cbor_codec.h"
#include "device_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Limits ────────────────────────────────────────────────────────── */

#define DEVICE_SETTINGS_MAX_SETTINGS    16
#define DEVICE_SETTINGS_MAX_ENUM_OPTS   16
#define DEVICE_SETTINGS_MAX_STRING_POOL 512
#define DEVICE_SETTINGS_MAX_DEVICES     16

/* Internal aliases retain existing consumers while the wire contract lives
 * in cbor_codec.h. */
#define DS_TYPE_NONE   GW_SETTING_TYPE_NONE
#define DS_TYPE_BOOL   GW_SETTING_TYPE_BOOL
#define DS_TYPE_INT    GW_SETTING_TYPE_INT
#define DS_TYPE_FLOAT  GW_SETTING_TYPE_FLOAT
#define DS_TYPE_STRING GW_SETTING_TYPE_STRING
#define DS_TYPE_ENUM   GW_SETTING_TYPE_ENUM

/* Internal descriptor flags. Wire READONLY is deliberately translated into
 * the inverse WRITABLE capability when a descriptor is decoded. */
#define DS_FLAG_WRITABLE (1u << 0)
#define DS_FLAG_SECRET   (1u << 1)
#define DS_FLAG_ADVANCED (1u << 2)

/* ── Schema states ─────────────────────────────────────────────────── */

typedef enum {
    DS_SCHEMA_UNKNOWN = 0,
    DS_SCHEMA_DISCOVERING,
    DS_SCHEMA_READY,
    DS_SCHEMA_UNSUPPORTED,
    DS_SCHEMA_ERROR,
} ds_schema_state_t;

/* ── Operation states ──────────────────────────────────────────────── */

typedef enum {
    DS_OP_IDLE = 0,
    DS_OP_QUEUED,
    DS_OP_RUNNING,
    DS_OP_WAITING_REBOOT,
    DS_OP_VERIFYING,
} ds_op_state_t;

typedef enum {
    DS_OP_NONE = 0,
    DS_OP_DESCRIBE,
    DS_OP_GET,
    DS_OP_SET,
    DS_OP_COMMIT,
} ds_op_kind_t;

typedef enum {
    DS_OP_RESULT_OK = 0,
    DS_OP_RESULT_BUSY,
    DS_OP_RESULT_TIMEOUT,
    DS_OP_RESULT_NOT_SUPPORTED,
    DS_OP_RESULT_PROTOCOL_ERROR,
    DS_OP_RESULT_MEMORY_ERROR,
    DS_OP_RESULT_INTERNAL,
    DS_OP_RESULT_CONFLICT,
    DS_OP_RESULT_REJECTED,
    DS_OP_RESULT_FAILED,
} ds_op_result_t;

/* ── Transaction change request ───────────────────────────────────────
 * One setting value change.  Deep-copied into PSRAM on save(). */

typedef struct {
    char     setting_id[GW_SETTINGS_CBOR_MAX_ID_LEN];
    uint8_t  type;       /* DS_TYPE_* */
    union {
        bool    bool_val;
        int32_t int_val;
        float   float_val;
        int32_t enum_val;
        struct {
            char str[128];
        } string_val;
    };
} ds_change_request_t;

/* ── Transaction states ────────────────────────────────────────────── */

typedef enum {
    DS_TX_IDLE = 0,
    DS_TX_PREVALIDATING,
    DS_TX_BEGIN_SENT,
    DS_TX_SET_SENT,
    DS_TX_COMMIT_SENT,
    DS_TX_CONFIRM_SENT,
    DS_TX_WAITING_REBOOT,    /* COMMIT ACK received, awaiting reboot + re-read */
    DS_TX_VERIFYING,         /* Reconnecting, values refreshed, verifying */
    DS_TX_SUCCEEDED,
    DS_TX_FAILED,
    DS_TX_CONFLICT,
    DS_TX_CANCELLED,
    DS_TX_OUTCOME_UNKNOWN,
} ds_tx_state_t;

/* ── Transaction result ─────────────────────────────────────────────── */

typedef enum {
    DS_TX_RESULT_OK = 0,
    DS_TX_RESULT_BUSY,
    DS_TX_RESULT_VALIDATION_FAILED,
    DS_TX_RESULT_MEMORY_ERROR,
    DS_TX_RESULT_DEVICE_CONFLICT,
    DS_TX_RESULT_DEVICE_REJECTED,
    DS_TX_RESULT_TIMEOUT,
    DS_TX_RESULT_DISCONNECTED,
    DS_TX_RESULT_CANCELLED,
    DS_TX_RESULT_FAILED,
    DS_TX_RESULT_OUTCOME_UNKNOWN,
    DS_TX_RESULT_INTERNAL,
} ds_tx_result_t;

/* ── Transaction completion callback ────────────────────────────────── */

typedef void (*ds_tx_completion_fn)(ds_tx_result_t result, void *context);

/* ── Transaction object (internal) ────────────────────────────────────
 * One per device.  All variable data lives in PSRAM. */

#define DS_TX_MAX_CHANGES  16

typedef struct {
    bool                active;
    char                device_id[32];
    ds_tx_state_t       state;

    /* Changes — PSRAM-allocated array, deep-copied from caller. */
    ds_change_request_t *changes;
    uint16_t            change_count;
    uint16_t            next_change_index;

    /* Non-zero transaction correlation ID sent in Settings key 41. */
    uint64_t            transaction_id;

    /* Expected config revision from caller's snapshot. */
    uint32_t            expected_config_rev;

    /* New revision from device COMMIT ACK. */
    uint32_t            new_config_rev;

    /* Completion. */
    ds_tx_completion_fn completion;
    void               *context;

    /* Reconciliation timer (opaque handle — FreeRTOS). */
    void               *recon_timer;
} ds_transaction_t;

/* ── Compact setting descriptor ──────────────────────────────────────
 * All string fields are uint16_t offsets into the schema's string pool.
 * This avoids fixed-size char arrays and keeps descriptors ~24 bytes. */

typedef struct {
    uint16_t id_off;         /* offset into string_pool */
    uint16_t title_off;      /* offset into string_pool */
    uint16_t group_off;      /* offset into string_pool, 0 = no group */
    uint16_t unit_off;       /* offset into string_pool, 0 = no unit */
    int32_t  min_value;
    int32_t  max_value;
    uint32_t step;
    uint16_t flags;          /* internal DS_FLAG_* (never raw wire flags) */
    uint16_t max_length;     /* for DS_TYPE_STRING */
    uint16_t option_index;   /* index into enum_option_pool */
    uint8_t  type;           /* DS_TYPE_* */
    uint8_t  option_count;   /* for DS_TYPE_ENUM */
} ds_setting_desc_t;

/* ── Enum option ───────────────────────────────────────────────────── */

typedef struct {
    uint8_t  value;
    uint16_t label_off;      /* offset into string_pool */
} ds_enum_option_t;

/* ── String pool ───────────────────────────────────────────────────── */

typedef struct {
    uint16_t total_size;     /* bytes used in pool[] */
    uint16_t capacity;       /* bytes allocated for pool[] */
    char    *pool;           /* contiguous byte buffer (PSRAM) */
} ds_string_pool_t;

/* ── Schema snapshot (immutable after commit) ──────────────────────── */

typedef struct {
    uint32_t           schema_revision;
    uint16_t           setting_count;
    ds_setting_desc_t  descriptors[DEVICE_SETTINGS_MAX_SETTINGS];
    ds_enum_option_t   enum_option_pool[DEVICE_SETTINGS_MAX_SETTINGS *
                                        DEVICE_SETTINGS_MAX_ENUM_OPTS];
    uint16_t           enum_option_count;
    ds_string_pool_t   strings;
} ds_schema_t;

/* ── Value entry ───────────────────────────────────────────────────── */

typedef struct {
    uint16_t id_off;         /* offset into string_pool (same pool as schema) */
    uint8_t  type;           /* DS_TYPE_* */
    bool     has_value;
    union {
        bool     bool_val;
        int32_t  int_val;
        float    float_val;
        uint16_t string_off; /* offset into value_string_pool */
        int32_t  enum_val;
    };
} ds_value_entry_t;

/* ── Values snapshot (immutable after commit) ──────────────────────── */

typedef struct {
    uint32_t           config_revision;
    uint16_t           value_count;
    ds_value_entry_t   values[DEVICE_SETTINGS_MAX_SETTINGS];
    ds_string_pool_t   string_pool;  /* for string-typed values */
} ds_values_t;

/* ── Per-device control record (internal SRAM) ─────────────────────── */

typedef struct {
    bool               used;
    char               device_id[32]; /* device_store identity */
    ds_schema_state_t  schema_state;

    /* Schema/values ownership — refcounted snapshots (PSRAM). */
    ds_schema_t       *schema;
    ds_values_t       *values;
    uint32_t           schema_rev;  /* last committed schema revision */
    uint32_t           config_rev;  /* last committed config revision */

    /* Discovery stream state. */
    uint16_t           staging_expected_count;  /* from settings_begin total */
    uint16_t           staging_received_count;  /* items received so far */
    uint32_t           staging_snapshot_id;
    uint16_t           advertised_schema_rev;
    bool               schema_stream_active;
    bool               values_stream_active;

    /* Reconciliation state — set after COMMIT ACK, verified on reconnect. */
    bool               pending_reconciliation;
    uint32_t           reconciliation_expected_rev;  /* new rev from COMMIT ACK */
    uint32_t           reconciliation_old_rev;       /* rev before commit */
} ds_device_record_t;

/* ── Init / deinit ─────────────────────────────────────────────────── */

esp_err_t device_settings_init(void);
void      device_settings_deinit(void);

/* ── Schema snapshot lifecycle ────────────────────────────────────────
 * Acquire returns a refcounted pointer.  The caller MUST call release
 * when done.  The snapshot is freed only after all readers release. */

const ds_schema_t *device_settings_schema_acquire(const char *device_id);
void               device_settings_schema_release(const ds_schema_t *schema);

const ds_values_t *device_settings_values_acquire(const char *device_id);
void               device_settings_values_release(const ds_values_t *values);

/* ── Schema builder (used during discovery) ──────────────────────────
 * Builds a staging schema in PSRAM, validates, then atomically swaps
 * into the committed slot.  Old snapshot freed after readers drop. */

typedef struct {
    ds_string_pool_t  strings;
    ds_enum_option_t  enum_options[DEVICE_SETTINGS_MAX_SETTINGS *
                                   DEVICE_SETTINGS_MAX_ENUM_OPTS];
    uint16_t          enum_option_count;
    ds_setting_desc_t descriptors[DEVICE_SETTINGS_MAX_SETTINGS];
    uint16_t          setting_count;
    uint32_t          schema_revision;
} ds_schema_builder_t;

esp_err_t ds_schema_builder_init(ds_schema_builder_t *builder);
void      ds_schema_builder_reset(ds_schema_builder_t *builder);
esp_err_t ds_schema_builder_add_string(ds_schema_builder_t *builder,
                                       const char *str, uint16_t *out_off);
esp_err_t ds_schema_builder_add_setting(ds_schema_builder_t *builder,
                                        const ds_setting_desc_t *desc);
esp_err_t ds_schema_builder_add_enum_option(ds_schema_builder_t *builder,
                                            uint8_t value,
                                            const char *label,
                                            uint16_t *out_index);
ds_schema_t *ds_schema_builder_commit(ds_schema_builder_t *builder);

/* ── Values builder ────────────────────────────────────────────────── */

typedef struct {
    ds_string_pool_t  string_pool;
    ds_value_entry_t  entries[DEVICE_SETTINGS_MAX_SETTINGS];
    uint16_t          value_count;
    uint32_t          config_revision;
} ds_values_builder_t;

esp_err_t ds_values_builder_init(ds_values_builder_t *builder);
void      ds_values_builder_reset(ds_values_builder_t *builder);
esp_err_t ds_values_builder_add(ds_values_builder_t *builder,
                                const ds_value_entry_t *entry);
ds_values_t *ds_values_builder_commit(ds_values_builder_t *builder);

/* ── Protocol handler ────────────────────────────────────────────────
 * Processes settings_begin / settings_item / settings_end messages.
 * Called from the BLE notify path. */

bool device_settings_on_notify(const char *device_id,
                               const gw_message_t *message);

/* ── Disconnect handler ────────────────────────────────────────────
 * Called when a BLE device disconnects.  Frees staging state; preserved
 * committed snapshots remain valid (but may be marked stale). */

void device_settings_on_disconnect(const char *device_id);

void device_settings_on_capability(const char *device_id,
                                   bool supported,
                                   uint16_t schema_revision);

/* ── Reconciliation timeout ───────────────────────────────────────────
 * If device does not reconnect within this window after COMMIT ACK,
 * the transaction is resolved as OUTCOME_UNKNOWN. */

#define DS_RECONCILIATION_TIMEOUT_MS  30000

/* ── Reconciliation ───────────────────────────────────────────────────
 * Called after values are refreshed following a reconnect when a
 * device has a pending reconciliation.  Compares config_rev against
 * the expected new revision and verifies changed values.  Resolves
 * the pending transaction as SUCCEEDED / FAILED / CONFLICT / OUTCOME_UNKNOWN.
 *
 * Safe to call if no reconciliation is pending (no-op). */

void device_settings_reconcile(const char *device_id);

/* ── Operation API ─────────────────────────────────────────────────── */

typedef void (*ds_op_completion_fn)(ds_op_result_t result, void *context);

esp_err_t device_settings_describe(const char *device_id,
                                   ds_op_completion_fn completion,
                                   void *context);
esp_err_t device_settings_get(const char *device_id,
                              ds_op_completion_fn completion,
                              void *context);
esp_err_t device_settings_cancel(const char *device_id);

/* ── Transaction API (G3) ────────────────────────────────────────────
 * Orchestrates BEGIN → SET → COMMIT for atomic settings update.
 * Changes are deep-copied into PSRAM; caller retains ownership. */

esp_err_t device_settings_save(const char *device_id,
                               const ds_change_request_t *changes,
                               uint16_t change_count,
                               uint32_t expected_config_rev,
                               uint64_t *out_transaction_id,
                               ds_tx_completion_fn completion,
                               void *context);

esp_err_t device_settings_tx_cancel(const char *device_id);

/* ── Operation internals (exposed for worker and testing) ──────────── */

extern bool      s_ops_active[];
extern uint32_t  s_ops_generation[];
void      s_ops_invoke_completion(int slot, ds_op_result_t result);
const char *s_ops_get_device_id(int slot);
ds_op_kind_t s_ops_get_kind(int slot);
ds_op_state_t s_ops_get_state(int slot);
void      s_ops_set_state(int slot, ds_op_state_t state);
void      s_ops_set_kind(int slot, ds_op_kind_t kind);

/* ── Worker API (G3) ──────────────────────────────────────────────── */

esp_err_t device_settings_worker_init(void);
void      device_settings_worker_deinit(void);
void      device_settings_worker_submit(void);
void      device_settings_worker_on_disconnect(const char *device_id);
void      device_settings_worker_on_values_complete(const char *device_id,
                                                     bool success);
void      device_settings_protocol_on_disconnect(const char *device_id);

/* ── Transaction internals (exposed for testing) ─────────────────── */

ds_transaction_t *ds_tx_find(const char *device_id);
ds_transaction_t *ds_tx_alloc(void);
void ds_tx_free(ds_transaction_t *tx);
void ds_tx_reset_for_test(void);
void device_settings_tx_store_result(const char *device_id,
                                     ds_tx_result_t result,
                                     uint32_t config_revision);

/* ── Query API ─────────────────────────────────────────────────────── */

esp_err_t device_settings_get_state(const char *device_id,
                                    ds_schema_state_t *out_state);
esp_err_t device_settings_get_record(const char *device_id,
                                     ds_device_record_t *out);

/* ── Transaction status (for web layer) ──────────────────────────────
 * Returns the current transaction state for a device.  If no transaction
 * is active, returns the last completed result from the result registry.
 * out_active: true if a transaction is currently in progress.
 * out_state: current ds_tx_state_t.
 * out_last_result: last completed result (valid when !out_active). */

bool device_settings_tx_get_status(const char *device_id,
                                   bool *out_active,
                                   ds_tx_state_t *out_state,
                                   ds_tx_result_t *out_last_result);

/* ── State name helpers ────────────────────────────────────────────── */

const char *device_settings_schema_state_name(ds_schema_state_t state);
const char *device_settings_tx_state_name(ds_tx_state_t state);
const char *device_settings_tx_result_name(ds_tx_result_t result);

/* ── Observability counters ────────────────────────────────────────
 * Compact diagnostic counters for production monitoring.
 * All fields are uint32_t counters — use ds_diag_snapshot() for
 * a consistent read.  Counters are cumulative since boot. */

typedef struct {
    uint32_t discovery_schema_success;
    uint32_t discovery_schema_fail;
    uint32_t discovery_values_success;
    uint32_t discovery_values_fail;
    uint32_t psram_alloc_success;
    uint32_t psram_alloc_fail;
    uint32_t tx_success;
    uint32_t tx_fail;
    uint32_t tx_conflict;
    uint32_t tx_prevalidate_fail;
    uint32_t outcome_unknown;
    uint32_t reconcile_success;
    uint32_t reconcile_fail;
    uint32_t reconcile_conflict;
} ds_diag_t;

/* Take a consistent snapshot of all diagnostic counters. */
void device_settings_diag_snapshot(ds_diag_t *out);

/* Reset all diagnostic counters to zero (for testing). */
void device_settings_diag_reset(void);

/* Live counter instance — extern for increment from sibling files. */
extern ds_diag_t s_diag;

#define DS_DIAG_INC(field)  do { ++s_diag.field; } while (0)

/* ── Per-device record access (internal) ───────────────────────────── */

ds_device_record_t *device_settings_find_record(const char *device_id);
ds_device_record_t *device_settings_find_or_create_record(
    const char *device_id);
esp_err_t device_settings_commit_schema(const char *device_id,
                                        ds_schema_t *schema);
esp_err_t device_settings_commit_values(const char *device_id,
                                        ds_values_t *values);

/* ── Memory helpers (exposed for testing) ──────────────────────────── */

void *ds_settings_alloc(size_t size);
void  ds_settings_free(void *ptr);
bool  ds_settings_ref_acquire(ds_schema_t *schema);
void  ds_settings_ref_release(ds_schema_t *schema);
bool  ds_values_ref_acquire(ds_values_t *values);
void  ds_values_ref_release(ds_values_t *values);

/* ── String pool helpers ───────────────────────────────────────────── */

const char *ds_string_pool_get(const ds_string_pool_t *pool, uint16_t off);
esp_err_t   ds_string_pool_add(ds_string_pool_t *pool, const char *str,
                               uint16_t *out_off);

/* ── Test helpers ──────────────────────────────────────────────────── */

void device_settings_reset_for_test(void);
void device_settings_operation_reset_for_test(void);
void device_settings_fill_all_ops_for_test(void);
void device_settings_worker_reset_for_test(void);
bool device_settings_worker_is_idle_for_test(void);
uint32_t device_settings_worker_get_generation_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_SETTINGS_H */
