#!/usr/bin/env python3
"""Drive the C++ demo server with the official Python MCP SDK (pip install mcp).

    python tests/py_client_test.py stdio build/Release/demo_server.exe
    python tests/py_client_test.py http  http://127.0.0.1:8000/mcp     (demo_server --http 8000)
"""
import asyncio
import base64
import sys
import time

import anyio
from mcp import Client
from mcp.client.stdio import StdioServerParameters


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


async def run(target):
    logs = []

    async def on_log(params):
        logs.append(params.data)

    async with Client(target, logging_callback=on_log) as c:
        info = c.server_info
        check(info.name == "cpp-demo", info)
        print("connected:", info.name, info.version, "protocol", c.protocol_version)

        tools = (await c.list_tools()).tools
        names = sorted(t.name for t in tools)
        check(names == ["add", "count_slowly", "greet", "red_dot", "stats"], names)
        greet = next(t for t in tools if t.name == "greet")
        check(greet.input_schema["required"] == ["name"], greet.input_schema)
        check(greet.input_schema["properties"]["greeting"]["default"] == "Hello", greet.input_schema)

        r = await c.call_tool("add", {"a": 2, "b": 3.5})
        check(r.structured_content == {"result": 5.5} and r.content[0].text == "5.5", r)
        r = await c.call_tool("greet", {"name": "Taro", "loud": True})
        check(r.content[0].text == "HELLO, TARO!", r)
        r = await c.call_tool("stats", {"numbers": [1, 2, 3, 4]})
        check(r.structured_content["mean"] == 2.5, r)
        r = await c.call_tool("stats", {"numbers": []})
        check(r.is_error and "at least one" in r.content[0].text, r)
        r = await c.call_tool("add", {"a": "x", "b": 1})
        check(r.is_error and "wrong type" in r.content[0].text, r)
        r = await c.call_tool("red_dot", {})
        img = r.content[1]
        check(img.type == "image" and base64.b64decode(img.data)[:4] == b"\x89PNG", r)
        print("tools ok")

        # progress + logging
        await c.set_logging_level("info")
        prog = []

        async def on_progress(p, total, message):
            prog.append((p, total, message))

        r = await c.call_tool("count_slowly", {"n": 5}, progress_callback=on_progress)
        check(r.content[0].text == "counted to 5", r)
        check([p for p, _, _ in prog] == [1, 2, 3, 4, 5] and prog[-1][1] == 5, prog)
        check(any("counting to 5" in str(x) for x in logs), logs)
        print("progress ok:", prog[-1], "; log:", logs[-1])

        # cancellation: cancel a 3 s tool after 0.4 s, the server keeps working
        t0 = time.monotonic()
        with anyio.move_on_after(0.4):
            await c.call_tool("count_slowly", {"n": 30})
        r = await c.call_tool("add", {"a": 1, "b": 1})
        check(r.structured_content == {"result": 2.0}, r)
        print(f"cancel ok: server answered again after {time.monotonic() - t0:.2f}s")

        res = (await c.list_resources()).resources
        check([str(x.uri) for x in res] == ["config://app"], res)
        tmpl = (await c.list_resource_templates()).resource_templates
        check(tmpl[0].uri_template == "greeting://{name}", tmpl)
        r = await c.read_resource("greeting://花子")
        check(r.contents[0].text == "こんにちは、花子さん", r)
        r = await c.read_resource("config://app")
        check('"theme"' in r.contents[0].text, r)
        print("resources ok:", r.contents[0].text)

        prompts = (await c.list_prompts()).prompts
        check(prompts[0].name == "review_code", prompts)
        r = await c.get_prompt("review_code", {"code": "int x;", "language": "C++"})
        check("C++ code" in r.messages[0].content.text, r)
        print("prompts ok")
        await c.send_ping()


def main():
    mode, where = sys.argv[1], sys.argv[2]
    target = StdioServerParameters(command=where) if mode == "stdio" else where
    asyncio.run(run(target))
    print(mode, "ALL OK")


if __name__ == "__main__":
    main()
