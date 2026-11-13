#!/usr/bin/env python3
"""Peekaboo MCP bridge.

A thin stdio <-> HTTP shim that lets Claude Desktop talk to a *running* Peekaboo instance.
Peekaboo can't itself be a stdio MCP server (it's a long-lived GUI that holds the live emulator
session), so it exposes a tiny JSON-over-HTTP API on 127.0.0.1 and this bridge translates MCP
stdio JSON-RPC into calls against that API.

The tool list is fetched from Peekaboo at startup (GET /tools), so this bridge is generic: there
is no per-tool code here and the C++ manifest stays the single source of truth.

Environment:
    PEEKABOO_MCP_URL    base URL of Peekaboo's MCP API (default http://127.0.0.1:7475)
    PEEKABOO_MCP_TOKEN  shared auth token (copy it from Peekaboo > Preferences > Claude / MCP)
"""

import asyncio
import json
import os
import sys
import urllib.error
import urllib.request

from mcp.server import Server
from mcp.server.stdio import stdio_server
import mcp.types as types

URL = os.environ.get("PEEKABOO_MCP_URL", "http://127.0.0.1:7475").rstrip("/")
TOKEN = os.environ.get("PEEKABOO_MCP_TOKEN", "")

server = Server("peekaboo")


def _headers():
    h = {"Content-Type": "application/json"}
    if TOKEN:
        h["X-Peekaboo-Token"] = TOKEN
    return h


def _http_get(path: str):
    req = urllib.request.Request(URL + path, headers=_headers(), method="GET")
    with urllib.request.urlopen(req, timeout=15) as r:
        return json.loads(r.read().decode("utf-8"))


def _http_post(path: str, payload: dict):
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(URL + path, data=data, headers=_headers(), method="POST")
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        try:
            return json.loads(e.read().decode("utf-8"))
        except Exception:
            return {"error": f"HTTP {e.code}: {e.reason}"}
    except urllib.error.URLError as e:
        return {"error": f"cannot reach Peekaboo at {URL} ({e.reason}). Is it running with the "
                         f"MCP server enabled?"}


@server.list_tools()
async def list_tools():
    try:
        manifest = await asyncio.to_thread(_http_get, "/tools")
    except Exception as e:  # noqa: BLE001
        print(f"peekaboo-mcp: failed to fetch tool manifest: {e}", file=sys.stderr)
        return []
    tools = []
    for t in manifest:
        tools.append(
            types.Tool(
                name=t["name"],
                description=t.get("description", ""),
                inputSchema=t.get("inputSchema", {"type": "object"}),
            )
        )
    return tools


@server.call_tool()
async def call_tool(name: str, arguments: dict | None):
    result = await asyncio.to_thread(_http_post, "/tool", {"name": name, "args": arguments or {}})

    if isinstance(result, dict) and "image_png_base64" in result:
        # The image is only half of a screenshot result. partialFrame/beam say whether the
        # capture is a whole field, and displayMasks/note say whether layers were filtered out
        # of it -- exactly the things you cannot see by looking at the picture. Returning the
        # ImageContent alone dropped all of them, so a mid-frame capture arrived looking like a
        # program that draws nothing. Text first, so the caveat is read alongside the image.
        meta = {k: v for k, v in result.items() if k != "image_png_base64"}
        content = []
        if meta:
            content.append(types.TextContent(type="text", text=json.dumps(meta, indent=2)))
        content.append(
            types.ImageContent(
                type="image", data=result["image_png_base64"], mimeType="image/png"
            )
        )
        return content
    if isinstance(result, dict) and "error" in result:
        return [types.TextContent(type="text", text="Error: " + str(result["error"]))]
    return [types.TextContent(type="text", text=json.dumps(result, indent=2))]


async def main():
    async with stdio_server() as (read, write):
        await server.run(read, write, server.create_initialization_options())


if __name__ == "__main__":
    asyncio.run(main())
