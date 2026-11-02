# RESUME — mcp_cpp の続きから

## 目標

* MCP の C++ SDK（サーバとクライアント）。ヘッダーオンリー、cpp-httplib + nlohmann/json。
* 公式 Python SDK と相互に通じること（Python ⇔ C++ のどの組み合わせでも同じ検査が通る）。

## 決まったこと

* ビルドは VS2022（mingw の exe はウイルス対策に消される）。
* 基準にしたのは公式 Python SDK `mcp` 2.3.0（2026-11 時点）。この版は2つの世代を話す：
  * handshake 世代（`initialize`）：2024-11-05 / 2025-03-26 / 2025-06-18 / 2025-11-25
  * modern 世代：2026-07-28（`server/discover` と、リクエストごとの envelope。logging と ping は廃止予定）
  Python クライアントの既定（auto）は、まず `server/discover` を送り、MCPError が返れば `initialize` に切り替える。
  mcp_cpp は今のところ handshake 世代だけ。stdio では discover に -32601、HTTP では
  セッション無しの discover に JSON-RPC の -32601（HTTP 200）を返して、従来方式に切り替えさせる。
* Python の MCPServer（2.x、FastMCP から改名）は logging capability を宣言しない（`logging/setLevel` は -32601）。
  クライアントは capability があるときだけ setLevel を呼ぶ。通常の例外の文面は隠されるので、参照サーバでは `ToolError` を使う。
  `-> dict` の戻り値には structuredContent が付かない（テキストに JSON）。
* tools/call は stdio では別スレッドで実行（実行中も ping や cancelled を受けられる）。HTTP では、Accept に
  text/event-stream があれば SSE で返し、progress や log を同じストリームに流す。それ以外は JSON で返す。
* キャンセル：`notifications/cancelled` でフラグを立てる。ツールは次の `ctx.progress()` / `ctx.log()` /
  `ctx.check()` で打ち切られる。HTTP で SSE が切れたときも、そのセッションの実行中リクエストにフラグを立てる。

## 確認済み（2026-11-02）

* `tests/run_tests.sh`：公式 Python クライアント → mcp_cpp、mcp_cpp クライアント → mcp_cpp、
  mcp_cpp クライアント → Python 参照サーバ、を stdio と HTTP で計6通り、すべて PASS。

* Claude Code 2.1.316 に `claude mcp add cpp-demo -- <demo_server.exe>`（local スコープ）で登録し、`claude mcp list` で
  Connected。`claude -p` の別セッションに add / stats / greet / リソース greeting://太郎 を使わせ、4つとも正しい結果が返った。

## 次にやること

1. modern 世代（2026-07-28：`server/discover`、リクエストごとの envelope）。
2. サーバからクライアントへのリクエスト（sampling/createMessage、elicitation、roots/list）。
3. tools / resources / prompts の listChanged 通知、resources/subscribe、completion/complete の中身。
5. 既存の C++ 資産（ppocr_cpp や yolo_lpr_cpp）を MCP ツールとして公開するサンプル。
