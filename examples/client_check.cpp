// Connect to a demo MCP server (ours or the Python reference) and check every feature.
//   client_check --stdio build/Release/demo_server.exe
//   client_check --stdio python tests/py_ref_server.py
//   client_check --http http://127.0.0.1:8000/mcp
#include "mcp_client.hpp"

static int failures = 0;
#define CHECK(cond, what)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::cerr << "FAIL: " << what << "\n";                              \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: client_check --stdio <command...> | --http <url>\n";
        return 2;
    }
    std::string mode = argv[1];
    std::vector<std::string> rest(argv + 2, argv + argc);
    try {
        mcp::Client c = mode == "--http" ? mcp::Client::http(rest[0]) : mcp::Client::stdio(rest);
        std::vector<std::string> logs;
        c.on_log([&](const std::string& level, const mcp::json& data) { logs.push_back(level + ": " + data.dump()); });
        mcp::json info = c.initialize();
        std::cout << "server: " << info["serverInfo"].dump() << " protocol " << info["protocolVersion"] << "\n";

        std::vector<std::string> names;
        for (auto& t : c.list_tools()) names.push_back(t["name"]);
        std::sort(names.begin(), names.end());
        CHECK(std::find(names.begin(), names.end(), "add") != names.end() &&
                  std::find(names.begin(), names.end(), "count_slowly") != names.end(),
              "tools/list");

        mcp::json r = c.call_tool("add", {{"a", 2}, {"b", 3.5}});
        CHECK(r["structuredContent"]["result"] == 5.5, "add: " + r.dump());
        r = c.call_tool("greet", {{"name", "Taro"}, {"loud", true}});
        CHECK(r["content"][0]["text"] == "HELLO, TARO!", "greet: " + r.dump());
        r = c.call_tool("stats", {{"numbers", {1, 2, 3, 4}}});
        // structured output if the server gives it, else the JSON in the text block
        mcp::json st = r.contains("structuredContent") ? r["structuredContent"]
                                                       : mcp::json::parse(r["content"][0]["text"].get<std::string>());
        CHECK(st.value("mean", 0.0) == 2.5, "stats: " + r.dump());
        r = c.call_tool("stats", {{"numbers", mcp::json::array()}});
        CHECK(r.value("isError", false) && r["content"][0]["text"].get<std::string>().find("at least one") != std::string::npos,
              "stats error: " + r.dump());
        std::cout << "tools ok\n";

        bool has_logging = info["capabilities"].contains("logging");
        if (has_logging) c.set_logging_level("info");  // only when the server advertises it
        std::vector<double> prog;
        r = c.call_tool("count_slowly", {{"n", 5}},
                        [&](double p, std::optional<double> total, const std::string&) {
                            prog.push_back(p);
                            CHECK(total && *total == 5, "progress total");
                        });
        CHECK(r["content"][0]["text"] == "counted to 5", "count_slowly: " + r.dump());
        CHECK(prog == std::vector<double>({1, 2, 3, 4, 5}), "progress values");
        bool logged = false;
        for (auto& l : logs) logged = logged || l.find("counting to 5") != std::string::npos;
        if (has_logging) CHECK(logged, "log notification");
        std::cout << "progress ok (" << prog.size() << " updates), logs " << logs.size() << "\n";

        r = c.read_resource("greeting://花子");
        CHECK(r["contents"][0]["text"] == "こんにちは、花子さん", "resource template: " + r.dump());
        r = c.read_resource("config://app");
        CHECK(r["contents"][0]["text"].get<std::string>().find("theme") != std::string::npos, "resource: " + r.dump());
        CHECK(c.list_resource_templates()[0]["uriTemplate"] == "greeting://{name}", "templates");
        std::cout << "resources ok\n";

        r = c.get_prompt("review_code", {{"code", "int x;"}, {"language", "C++"}});
        CHECK(r["messages"][0]["content"]["text"].get<std::string>().find("C++ code") != std::string::npos, "prompt: " + r.dump());
        std::cout << "prompts ok\n";

        try {
            c.call("no/such/method", mcp::json::object());
            CHECK(false, "unknown method should fail");
        } catch (const mcp::Error& e) {
            CHECK(e.code == mcp::METHOD_NOT_FOUND, "error code " + std::to_string(e.code));
        }
        c.ping();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: exception " << e.what() << "\n";
        return 1;
    }
    std::cout << (failures ? "FAILED " + std::to_string(failures) : std::string("ALL OK")) << "\n";
    return failures ? 1 : 0;
}
