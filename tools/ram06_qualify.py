#!/usr/bin/env python3
"""Bounded RAM-06 LAN qualification runner.

Runs only non-destructive REST/MCP traffic and records every status snapshot.
Hardware power-cycle, BLE-link, OTA and long soak scenarios remain manual.
"""
import argparse
import json
import time
import urllib.request
import urllib.error


def request(url, data=None, headers=None):
    body = None if data is None else json.dumps(data).encode()
    req = urllib.request.Request(url, data=body, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=8) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        payload = error.read()
        try:
            decoded = json.loads(payload)
        except json.JSONDecodeError:
            decoded = {"error": payload.decode(errors="replace")}
        return error.code, decoded


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ip", default="192.168.1.114")
    parser.add_argument("--token", required=True)
    parser.add_argument("--loops", type=int, default=100)
    args = parser.parse_args()
    base = f"http://{args.ip}"
    mcp_headers = {
        "Content-Type": "application/json",
        "MCP-Protocol-Version": "2026-07-28",
        "Mcp-Method": "tools/list",
        "Authorization": f"Bearer {args.token}",
    }
    call_headers = dict(mcp_headers, **{"Mcp-Method": "tools/call", "Mcp-Name": "get_status"})
    samples = []
    for i in range(args.loops):
        status_code, status = request(f"{base}/api/status")
        list_code, tools = request(
            f"{base}/mcp",
            {"jsonrpc": "2.0", "id": i, "method": "tools/list", "params": {
                "_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28", "io.modelcontextprotocol/clientCapabilities": {}}}},
            mcp_headers,
        )
        call_code, call = request(
            f"{base}/mcp",
            {"jsonrpc": "2.0", "id": i, "method": "tools/call", "params": {
                "name": "get_status", "arguments": {},
                "_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28", "io.modelcontextprotocol/clientCapabilities": {}}}},
            call_headers,
        )
        samples.append({"iteration": i, "status_http": status_code,
                        "list_http": list_code, "call_http": call_code,
                        "internal": status.get("internal", {}),
                        "psram": status.get("psram", {}),
                        "tools": len(tools.get("result", {}).get("tools", [])),
                        "call_error": call.get("error")})
        time.sleep(0.35)
    print(json.dumps({"loops": args.loops, "samples": samples}, separators=(",", ":")))


if __name__ == "__main__":
    main()
