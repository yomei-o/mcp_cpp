#!/usr/bin/env python3
"""The same demo server written with the official Python SDK (mcp 2.x MCPServer),
used to test the C++ client against the reference implementation.

    python tests/py_ref_server.py              # stdio
    python tests/py_ref_server.py --http 8001  # Streamable HTTP at http://127.0.0.1:8001/mcp
"""
import asyncio
import statistics
import sys

from mcp.server.mcpserver import Context, MCPServer
from mcp.server.mcpserver.exceptions import ToolError

mcp = MCPServer("py-demo", instructions="Reference server written with the Python SDK.")


@mcp.tool()
def add(a: float, b: float) -> float:
    """Add two numbers."""
    return a + b


@mcp.tool()
def greet(name: str, loud: bool | None = None, greeting: str = "Hello") -> str:
    """Greet someone, optionally loudly."""
    s = f"{greeting}, {name}!"
    return s.upper() if loud else s


@mcp.tool()
def stats(numbers: list[float]) -> dict:
    """Mean and standard deviation of a list of numbers."""
    if not numbers:
        raise ToolError("need at least one number")
    return {"count": len(numbers), "mean": statistics.fmean(numbers), "stddev": statistics.pstdev(numbers)}


@mcp.tool()
async def count_slowly(n: int, ctx: Context) -> str:
    """Count to n, one step every 100 ms, reporting progress."""
    await ctx.info(f"counting to {n}")
    for i in range(1, n + 1):
        await asyncio.sleep(0.1)
        await ctx.report_progress(i, n, f"step {i}")
    return f"counted to {n}"


@mcp.resource("config://app", mime_type="application/json")
def config() -> str:
    """Static configuration"""
    return '{"theme": "dark", "lang": "ja"}'


@mcp.resource("greeting://{name}")
def greeting(name: str) -> str:
    """A personal greeting"""
    return f"こんにちは、{name}さん"


@mcp.prompt()
def review_code(code: str, language: str = "") -> str:
    """Ask for a code review."""
    return f"Please review this {language} code:\n\n{code}"


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--http":
        mcp.run("streamable-http", host="127.0.0.1", port=int(sys.argv[2]))
    else:
        mcp.run("stdio")
