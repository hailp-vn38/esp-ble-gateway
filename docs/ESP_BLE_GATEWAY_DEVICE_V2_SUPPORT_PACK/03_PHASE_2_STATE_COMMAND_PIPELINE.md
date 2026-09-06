# Phase 2 — Runtime State and Command Pipeline

## 1. Tổng quan

Gateway hiện đã hỗ trợ apply BOOL/INT từ ACK/event vào `device_state`.

Điểm thiếu lớn:

```text
initial state seed hiện chỉ active-read BOOL properties
```

Device v2 hỗ trợ typed `read_feature_state`, nên Gateway phải seed cả INT.

---

## 2. Cần thêm / sửa gì

### 2.1. Seed all readable feature states

Current hard-code kiểu:

```text
ON_OFF
CONTACT
```

Target:

```text
for each committed feature:
    if property != NONE:
        submit read_feature_state
```

Không cần Gateway đoán BOOL/INT khi gửi read.

Device trả typed ACK.

---

### 2.2. State type handling

`device_state_on_command_ack()` đã có:

```text
feature_value_bool
feature_value_int
```

Giữ.

`device_state_on_notify()` cũng giữ typed event path.

---

### 2.3. Command validation

Gateway command validation hiện đã dùng tool:

```text
value_type
min/max/step
```

và kiểm tra:

```text
feature_id
property_id
writable_tool_index
```

Đây là đúng v2.

Không đưa min/max/step sang feature.

---

### 2.4. Additional schema consistency

Trước khi command path dùng schema:

- feature value_type phải match bound tool;
- template property type phải match feature value_type.

Có thể validate khi schema commit để runtime path đơn giản.

---

### 2.5. State capacity

Demo 10 features/device.

Kiểm tra:

```text
DEVICE_STATE_MAX_ENTRIES
```

phải đủ:

```text
max devices * realistic state count
```

Nếu hiện thiết kế chỉ đủ thấp, tăng có kiểm soát hoặc tính lại policy.

Không tăng mù; đo static RAM.

---

### 2.6. Seed burst control

Một schema commit có tối đa 12 state reads.

Không spawn task.

Dùng existing command service queue.

Nếu queue có giới hạn thấp:

- submit sequential/bounded;
- hoặc best-effort và rely event fallback;
- không block schema worker lâu.

---

## 3. Sửa ở đâu

| File | Thay đổi |
|---|---|
| `components/device_state/device_state.c` | remove BOOL-only seed restriction |
| `components/device_state/include/device_state.h` | capacity nếu cần |
| command service/executor modules | chỉ nếu seed queue cần adjustment |
| `components/device_schema/device_schema.c` | giữ validation source-of-truth |
| tests `device_state` | INT seed/ACK/event |

---

## 4. Checklist

### Seed

- [ ] relay seed.
- [ ] contact seed.
- [ ] fan seed.
- [ ] dimmer seed.
- [ ] temperature seed.
- [ ] humidity seed.
- [ ] generic setpoint seed.
- [ ] drying time seed.

### Runtime

- [ ] BOOL ACK updates state.
- [ ] INT ACK updates state.
- [ ] BOOL event updates state.
- [ ] INT event updates state.
- [ ] updated_at correct.
- [ ] gateway event published.

### Validation

- [ ] BOOL type mismatch reject.
- [ ] INT type mismatch reject.
- [ ] range reject.
- [ ] step reject.
- [ ] feature/tool mismatch reject.

---

## 5. Test plan

### T2.1 — Initial INT seed

After schema commit:

```text
fan_main
temperature_main
dryer_temperature
```

Expected state valid without waiting spontaneous event.

### T2.2 — Authoritative clamp

Request raw 655.

Device ACK raw 650.

Expected:

```text
device_state=650
WebSocket event/state flow reflects 650
```

### T2.3 — Async sensor

Temperature event raw 251.

Expected state 251.

### T2.4 — Local relay event

Expected BOOL state update.

### T2.5 — Range validation

Tool 300..1000 step5.

Try 652.

Expected range/step error before command transmit.

### T2.6 — Schema refresh

State seed reruns safely.

No duplicate table corruption.

### T2.7 — Disconnect during seed

Expected best-effort failure, no crash, reconnect recovers.

---

## 6. Exit criteria

- [ ] BOOL + INT seed.
- [ ] authoritative ACK state.
- [ ] async events.
- [ ] tool-driven validation.
- [ ] state capacity verified.
