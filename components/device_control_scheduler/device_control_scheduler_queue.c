#include "device_control_scheduler_internal.h"
/* Queue arbitration is intentionally worker-owned; fixed slots replace a
 * producer-visible queue so completion cannot be dropped under pressure. */
