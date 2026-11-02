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
  mcp_cpp は両世代に対応（下の「modern 世代」）。
* Python の MCPServer（2.x、FastMCP から改名）は logging capability を宣言しない（`logging/setLevel` は -32601）。
  クライアントは capability があるときだけ setLevel を呼ぶ。通常の例外の文面は隠されるので、参照サーバでは `ToolError` を使う。
  `-> dict` の戻り値には structuredContent が付かない（テキストに JSON）。
* tools/call は stdio では別スレッドで実行（実行中も ping や cancelled を受けられる）。HTTP では、Accept に
  text/event-stream があれば SSE で返し、progress や log を同じストリームに流す。それ以外は JSON で返す。
* キャンセル：`notifications/cancelled` でフラグを立てる。ツールは次の `ctx.progress()` / `ctx.log()` /
  `ctx.check()` で打ち切られる。HTTP で SSE が切れたときも、そのセッションの実行中リクエストにフラグを立てる。

## modern 世代（2026-07-28）の実装メモ

* 本家サーバの実際の応答を stdio / curl で記録して合わせた。
* 判定：stdio は Session::era（0 未定 / 1 handshake / 2 modern）。最初のリクエストが initialize なら 1、
  `_meta` に protocolVersion があるか server/discover なら 2。modern に決まった後の initialize は -32022
  （data に supported と requested）。HTTP は、MCP-Protocol-Version が handshake の版以外か、
  ヘッダもセッションも無く本文が modern なら modern の経路（セッション無し、1 POST に 1 メッセージ）。
* modern の検査の順番（本家と同じ）：`_meta` の必須キー（-32602）→ ヘッダ（版・Mcp-Method・Mcp-Name が本文と一致、
  重複なし。-32020）→ 版が対応しているか（-32022）。tools/call では `x-mcp-header` 付きのプロパティの
  `Mcp-Param-<token>` も照合する（トップレベルのプロパティのみ）。ヘッダ値は `=?base64?...?=` で包まれることがある。
* HTTP の status：-32700 / -32600 / -32602 / -32020 / -32021 / -32022 は 400、-32601 は 404、それ以外は 200。
* 結果には `resultType: "complete"` と `_meta["io.modelcontextprotocol/serverInfo"]`、一覧系と resources/read と
  discover には `cacheScope: "private"`、`ttlMs: 0`。ping と logging/setLevel は -32601。
  ログは、リクエストの `_meta` に logLevel があるときだけ送る。capabilities に logging を出さない。
* C++ クライアントの `connect("auto")`：discover を新方式の `_meta` 付きで送り、supportedVersions に 2026-07-28 があれば新方式。
  Error なら従来方式へ（-32022 で handshake 版を含まないときだけ失敗）。新方式では毎回 `_meta` と HTTP のヘッダを付ける。

## 確認済み（2026-11-02）

* 新方式対応の後：`tests/run_tests.sh` が新旧2方式 × 6通り＝12通り、すべて PASS。Claude Code からも引き続き使える。

* `tests/run_tests.sh`：公式 Python クライアント → mcp_cpp、mcp_cpp クライアント → mcp_cpp、
  mcp_cpp クライアント → Python 参照サーバ、を stdio と HTTP で計6通り、すべて PASS。

* Claude Code 2.1.316 に `claude mcp add cpp-demo -- <demo_server.exe>`（local スコープ）で登録し、`claude mcp list` で
  Connected。`claude -p` の別セッションに add / stats / greet / リソース greeting://太郎 を使わせ、4つとも正しい結果が返った。

## 次にやること

1. サーバからクライアントへの問い合わせ（従来方式は sampling/createMessage・elicitation・roots/list のリクエスト、
   新方式は `resultType: "input_required"` の結果と requestState）。
2. 新方式の subscriptions/listen（listChanged 通知）。
3. tools / resources / prompts の listChanged 通知、resources/subscribe、completion/complete の中身。
5. 既存の C++ 資産（ppocr_cpp や yolo_lpr_cpp）を MCP ツールとして公開するサンプル。
