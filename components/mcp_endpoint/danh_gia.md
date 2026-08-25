Đánh giá tổng thể: tài liệu xác định đúng phần lớn vấn đề thực tế, nhưng chưa đủ chính xác để triển khai nguyên trạng. Điểm mạnh nằm ở chẩn đoán HTTP/BLE; điểm yếu lớn nhất là kế hoạch MCP 2026 còn thiếu các trường bắt buộc và thiết kế async chưa xử lý hết vòng đời tài nguyên.

| Khía cạnh | Đánh giá |
| --- | ---: |
| Độ chính xác khi phân tích code hiện tại | 8.5/10 |
| Tính hợp lý của kiến trúc đề xuất | 7/10 |
| Mức sẵn sàng để triển khai | 5.5/10 |
| Hiệu quả tổng thể | 7/10 |

## Những điểm cần sửa trong tài liệu

### 1. Phần MCP 2026 chưa đầy đủ và có ví dụ không hợp lệ

Nhận định MCP `2026-07-28` đã bỏ handshake/session, chuyển sang metadata theo từng request và header routing là đúng. Đặc tả này đã chính thức phát hành và yêu cầu `MCP-Protocol-Version`, `Mcp-Method` cùng `Mcp-Name` khi phù hợp. [Thông báo MCP 2026 chính thức](https://blog.modelcontextprotocol.io/posts/2026-07-28/).

Tuy nhiên, các CallToolResult đề xuất ở [mục 32](</Users/lamphuchai/Desktop/esp32-ble-gateway/components/mcp_endpoint/Phân tích vấn đề và kế hoạch cải tiến components-mcp_endpoint.md:1210>) thiếu:

```json
"resultType": "complete"
```

Đây là trường bắt buộc trên wire trong revision `2026-07-28`. Ví dụ đúng phải là:

```json
{
  "resultType": "complete",
  "content": [
    {
      "type": "text",
      "text": "Device lamp-1 acknowledged 'toggle'"
    }
  ],
  "isError": false
}
```

Kế hoạch MCP cũng mới nhắc chung chung nhưng chưa đặc tả đầy đủ:

- `_meta.io.modelcontextprotocol/protocolVersion` trên request;
- `resultType` trên các result;
- `ttlMs` và `cacheScope` bắt buộc cho `tools/list`;
- lỗi `HeaderMismatch` `-32020`;
- lỗi thiếu capability `-32021`;
- lỗi protocol version không hỗ trợ `-32022`;
- HTTP `400` tương ứng;
- server identity trong response `_meta`;
- hình dạng chính xác của `server/discover`.

Các yêu cầu này được thể hiện trong [schema MCP 2026](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/schema/2026-07-28/schema.json) và [changelog chính thức](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/changelog.mdx).

Vì vậy Phase 5 hiện chưa đủ để đạt “MCP 2026 compliant”.

### 2. Async worker đúng hướng nhưng chưa an toàn để triển khai

Phân tích tại [mục async worker](</Users/lamphuchai/Desktop/esp32-ble-gateway/components/mcp_endpoint/Phân tích vấn đề và kế hoạch cải tiến components-mcp_endpoint.md:566>) là đúng: BLE ACK hiện chờ tối đa 2 giây ngay trên HTTP task tại [device_command.c](/Users/lamphuchai/Desktop/esp32-ble-gateway/components/command_dispatcher/device_command.c:128).

API `httpd_req_async_handler_begin()`/`complete()` là lựa chọn phù hợp. [ESP-IDF cũng cung cấp ví dụ async handlers chính thức](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/protocols/esp_http_server.html).

Nhưng thiết kế còn thiếu:

- Phải gọi `httpd_req_async_handler_complete()` trên mọi success/error path.
- Phải xử lý lỗi của `httpd_req_async_handler_begin()`.
- Queue đầy phải trả HTTP `503` trước khi chuyển ownership request.
- Async request giữ một socket trong suốt thời gian chờ.
- Cấu hình hiện có tối đa 7 socket; `2 worker + queue 4` có thể giữ tới 6 socket.
- Cần dành tối thiểu một hoặc hai socket cho Web UI.
- Cần xử lý client disconnect trong khi worker đang chạy.
- Cần quy định khởi tạo/dừng worker và cleanup nếu route registration thất bại.
- Worker stack phải chứa `dispatch_result_t` khoảng 4 KB và có thể thêm khoảng 1,5 KB danh sách device.

Nên bắt đầu với:

```text
worker = 1
queue = 2
```

và chỉ đưa `device_command` sang worker. Các gateway command nhanh như `get_status` hoặc `list_devices` có thể tiếp tục xử lý đồng bộ.

Việc đo stack không nên chờ đến Phase 6; phải thực hiện ngay khi xây worker.

### 3. Security đúng mục tiêu nhưng đánh giá token hơi lạc quan

Host, Origin và authentication là cần thiết vì `/mcp` là control plane. Tuy nhiên câu “Bearer random token: tốt trong LAN” tại [mục security](</Users/lamphuchai/Desktop/esp32-ble-gateway/components/mcp_endpoint/Phân tích vấn đề và kế hoạch cải tiến components-mcp_endpoint.md:1029>) cần điều chỉnh:

- Bearer token trên HTTP plaintext có thể bị nghe lén và replay trong LAN.
- Static bearer token không phải MCP OAuth-compliant authorization.
- Cần token rotation, revoke/reset và quy trình recovery.
- So sánh token nên theo constant-time.
- Không được log token.
- Host allowlist cần xử lý DHCP IP, hostname, port và IPv6.
- Origin validation bảo vệ browser/DNS rebinding nhưng không thay thế authentication.
- Nên bổ sung rate limit hoặc giới hạn auth failures.

Giải pháp token vẫn hợp lý cho một mạng LAN tin cậy, nhưng tài liệu phải nêu rõ threat model và giới hạn của nó.

### 4. `device_command` chỉ di chuyển hidden command surface

Phát hiện unknown tool bị biến thành BLE command là hoàn toàn đúng tại [mcp_tools.c](/Users/lamphuchai/Desktop/esp32-ble-gateway/components/mcp_endpoint/mcp_tools.c:138).

Tuy nhiên, tool mới:

```json
{
  "name": "device_command",
  "arguments": {
    "command": "factory_reset"
  }
}
```

vẫn cho phép gửi `factory_reset`. Nó chỉ làm `tools/list` trở thành allowlist ở cấp MCP tool, chưa kiểm soát command thực tế của peripheral.

Cần thêm một trong hai cơ chế:

- registry command theo `device_type`; hoặc
- capability/allowed-command list lấy từ từng peripheral.

Nếu không, security surface vẫn gần như giữ nguyên.

### 5. Schema đề xuất vẫn chưa phản ánh đầy đủ validation

Các schema tại [mục 42](</Users/lamphuchai/Desktop/esp32-ble-gateway/components/mcp_endpoint/Phân tích vấn đề và kế hoạch cải tiến components-mcp_endpoint.md:1628>) tốt hơn hiện tại nhưng còn thiếu:

- `edit_device` phải yêu cầu ít nhất một trong `name` hoặc `device_type`;
- `maxLength`: `device_id` 31, `name` 31, `device_type` 15, `command` 31;
- pattern cho BLE address;
- enum/range thực tế của `ble_addr_type`, không chỉ `0..255`;
- `outputSchema` cho `get_status` và `list_devices`;
- tool annotations như `readOnlyHint`, `destructiveHint`, `idempotentHint`.

Các lỗi unknown tool và malformed request nên trả protocol error; lỗi BLE/device/business nên dùng `isError`, phù hợp [quy tắc tool error MCP chính thức](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/docs/specification/2026-07-28/server/tools.mdx).

## Những phần tài liệu làm tốt

Các nhận định sau đã được đối chiếu và chính xác:

- `HTTPD_SOCK_ERR_TIMEOUT` đang bị retry vô hạn tại [mcp_endpoint.c](/Users/lamphuchai/Desktop/esp32-ble-gateway/components/mcp_endpoint/mcp_endpoint.c:15).
- MCP và Web UI dùng chung HTTP server task với stack 12 KB tại [web_server.c](/Users/lamphuchai/Desktop/esp32-ble-gateway/components/web_server/web_server.c:21).
- BLE ACK có thể block handler 2 giây.
- JSON hợp lệ nhưng root là array phải trả `-32600`, không phải `-32700`.
- `receive_body()` cần trả status có kiểu thay vì `NULL` mơ hồ.
- cJSON allocation failures đang được kiểm tra chưa đầy đủ.
- `tools/list` đang dùng schema chung và không phản ánh contract thật.
- Cần phân biệt protocol error với tool execution error.
- Cần đo stack high-water, minimum heap và largest free block.
- Tool metadata table là hướng refactor tốt.

## Thứ tự triển khai nên điều chỉnh

Mình đề xuất kế hoạch ngắn và hiệu quả hơn:

1. Viết characterization/integration tests cho hành vi hiện tại.
2. Sửa receive timeout, phân loại lỗi, body limit và OOM paths.
3. Thêm authentication, Content-Type và Origin/Host validation theo threat model rõ ràng.
4. Tạo strict tool registry, schema đầy đủ và command allowlist.
5. Async riêng `device_command`, ban đầu 1 worker/queue 2; kiểm tra socket, stack và cleanup.
6. Triển khai MCP `2026-07-28` như một wire codec hoàn chỉnh, gồm headers, `_meta`, `resultType`, cache hints, discovery và error codes.
7. Giữ legacy mode bằng feature flag trong giai đoạn chuyển tiếp.
8. Chạy conformance, concurrency và memory stress tests.

Tóm lại, tài liệu là một bản phân tích kỹ thuật tốt nhưng đang giống “design dossier” hơn là kế hoạch thực thi: gần 3.000 dòng, lặp lại nhiều sơ đồ và chưa khóa đủ wire contract/acceptance criteria. Sau khi sửa các điểm trên, nó có thể trở thành kế hoạch triển khai đáng tin cậy.