idf_component_register(
    SRCS "device_control_scheduler.c"
         "device_control_scheduler_worker.c"
         "device_control_scheduler_queue.c"
         "device_control_scheduler_lease.c"
    INCLUDE_DIRS "include"
    REQUIRES device_command_types device_command_service device_types device_store memory_policy
    PRIV_REQUIRES freertos esp_timer
)
