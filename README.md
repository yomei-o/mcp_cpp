# mcp_cpp

[Model Context Protocol](https://modelcontextprotocol.io/)（MCP）の C++ 版 SDK。ヘッダーオンリー。
[cpp-httplib](https://github.com/yhirose/cpp-httplib) と [nlohmann/json](https://github.com/nlohmann/json) だけで動く。

C++ で書いた処理（OCR、物体検出、DB 検索など）を、Claude Desktop / Claude Code などの LLM から
「ツール」として呼べるようにする。サーバだけでなくクライアントもある。

```cpp
#include "mcp.hpp"

int main() {
    mcp::Server srv("demo", "1.0.0");
    srv.tool("add", "Add two numbers.", [](double a, double b) { return a + b; },
             {mcp::arg("a", "first number"), mcp::arg("b", "second number")});
    srv.run_stdio();          // または srv.run_http("127.0.0.1", 8000)  →  http://127.0.0.1:8000/mcp
}
```

## できること

| | |
|---|---|
| ツール | 型付きの関数をそのまま登録。引数の型（`int` / `double` / `bool` / `std::string` / `std::vector<T>` / `std::map<std::string, T>` / `std::optional<T>` / `mcp::json`）から JSON Schema を自動で作る。`mcp::arg(name, 説明, 既定値)` |
| 戻り値 | `std::string` → テキスト、数値や bool → テキスト＋構造化出力（outputSchema 付き）、`mcp::json` → 構造化出力、`mcp::ToolResult` → 画像・音声・リソースリンクなど自由に |
| エラー | `throw mcp::ToolError("...")` → `isError: true` の結果（モデルが読んで直せる）。引数の型違い・不足も同じ形で返す |
| 進捗・ログ・キャンセル | 引数に `mcp::Context&` を置くと `ctx.progress(i, n, "msg")`、`ctx.info(...)`、`ctx.cancelled()`。キャンセルされると次の progress / log で処理が打ち切られる |
| リソース | `srv.resource(uri, ...)`（テキストまたは `mcp::Blob`）、`srv.resource_template("greeting://{name}", ...)` |
| プロンプト | `srv.prompt(name, 説明, fn, 引数)`。fn は文字列か `std::vector<mcp::PromptMessage>` を返す |
| トランスポート | stdio（改行区切り JSON）、Streamable HTTP（セッション ID、SSE で進捗を流す、Origin 検査、localhost に bind） |
| クライアント | `mcp_client.hpp`：`mcp::Client::stdio({"cmd", "args"})`（子プロセスを起動）／`mcp::Client::http(url)`。tools / resources / prompts / progress / logging / ping |
| プロトコル | `initialize` で始まる版（2025-11-25。2025-06-18 / 2025-03-26 / 2024-11-05 にも合わせる）。2026-07-28 の新方式（`server/discover`）のクライアントには「未対応」と返し、従来方式に切り替えてもらう |

## 本家との相互運用（確認済み）

公式 Python SDK（`mcp` 2.3.0）と、次の6通りすべてで同じ検査（ツール、構造化出力、エラー、画像、進捗、ログ、キャンセル、リソース、テンプレート、プロンプト、ping）が通る：

| クライアント | サーバ | stdio | HTTP |
|---|---|---|---|
| 公式 Python SDK | mcp_cpp | ✓ | ✓ |
| mcp_cpp | mcp_cpp | ✓ | ✓ |
| mcp_cpp | 公式 Python SDK（`tests/py_ref_server.py`） | ✓ | ✓ |

```sh
PY=<mcp を入れた python> bash tests/run_tests.sh
```

## Claude Code / Claude Desktop から使う

```sh
claude mcp add cpp-demo -- C:/path/to/demo_server.exe                 # stdio
claude mcp add --transport http cpp-demo http://127.0.0.1:8000/mcp     # HTTP（demo_server --http 8000）
```

Claude Desktop は `claude_desktop_config.json` の `mcpServers` に `{"command": "C:/path/to/demo_server.exe"}`。

stdio サーバでは、標準出力は JSON-RPC 専用。ログは `std::cerr` か `ctx.info()` に出す。

## ビルド

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

`include/` と `third_party/` を include パスに足し、Windows では `ws2_32` をリンクする（MSVC は `/utf-8`）。

## サンプル

* `examples/demo_server.cpp` — ツール5つ（add / greet / stats / count_slowly / red_dot）、リソース、テンプレート、プロンプト
* `examples/client_check.cpp` — 任意のサーバに繋いで全機能を検査するクライアント
* `tests/py_ref_server.py` — 同じ内容を公式 Python SDK で書いた参照サーバ

## 同梱ライブラリ

`third_party/` に cpp-httplib 0.18.3（MIT）、nlohmann/json 3.11.3（MIT）。
