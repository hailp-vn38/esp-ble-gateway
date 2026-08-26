# Device Capabilities

Component lưu command capability do BLE peripheral protocol v3 quảng bá.

Luồng chính:

```text
BLE READY -> describe_capabilities
          <- capabilities_begin
          <- capability_item (0..N-1)
          <- capabilities_end
          <- device_ack
```

Snapshot mới được dựng ở vùng staging và chỉ commit khi đủ `N` item đúng
`snapshot_id`/`sequence`. Snapshot tốt trước đó được giữ lại nếu refresh lỗi.
Cache hỗ trợ tối đa 12 command cho mỗi device và được persist bằng blob có
version trong namespace NVS `dev_caps`, key `cap00`..`cap15`.

`device_capabilities_validate_command()` thực hiện policy tương thích
`known_only`: nếu chưa biết capability thì cho phép flow legacy v2; nếu đã có
snapshot thì từ chối command không quảng bá hoặc argument sai kiểu/range.

Component không phụ thuộc `command_executor` để tránh dependency cycle.
Application inject submit hook bằng `device_capabilities_set_submitter()`.
