// A demo MCP server: tools, resources, prompts, progress, logging, cancellation.
//   demo_server            -> stdio (for Claude Desktop / Claude Code / any MCP client)
//   demo_server --http 8000 -> Streamable HTTP at http://127.0.0.1:8000/mcp
#include "mcp.hpp"

#include <cmath>
#include <ctime>

int main(int argc, char** argv) {
    mcp::Server srv("cpp-demo", "0.1.0", "A demo server written in C++ with mcp_cpp.");

    srv.tool("add", "Add two numbers.", [](double a, double b) { return a + b; },
             {mcp::arg("a", "first number"), mcp::arg("b", "second number")})
        .read_only();

    srv.tool("greet", "Greet someone, optionally loudly.",
             [](const std::string& name, std::optional<bool> loud, const std::string& greeting) {
                 std::string s = greeting + ", " + name + "!";
                 if (loud && *loud)
                     for (auto& c : s) c = (char)std::toupper((unsigned char)c);
                 return s;
             },
             {mcp::arg("name", "who to greet"), mcp::arg("loud", "shout it"), mcp::arg("greeting", "the word", "Hello")});

    srv.tool("stats", "Mean and standard deviation of a list of numbers.",
             [](const std::vector<double>& xs) {
                 if (xs.empty()) throw mcp::ToolError("need at least one number");
                 double m = 0, v = 0;
                 for (double x : xs) m += x;
                 m /= xs.size();
                 for (double x : xs) v += (x - m) * (x - m);
                 return mcp::json{{"count", xs.size()}, {"mean", m}, {"stddev", std::sqrt(v / xs.size())}};
             },
             {mcp::arg("numbers", "the data")});

    // Long-running: reports progress, logs, and stops when the client cancels.
    srv.tool("count_slowly", "Count to n, one step every 100 ms, reporting progress.",
             [](int n, mcp::Context& ctx) {
                 ctx.info("counting to " + std::to_string(n));
                 for (int i = 1; i <= n; ++i) {
                     std::this_thread::sleep_for(std::chrono::milliseconds(100));
                     ctx.progress(i, n, "step " + std::to_string(i));
                 }
                 return "counted to " + std::to_string(n);
             },
             {mcp::arg("n", "how far")});

    // Rich result: text + a tiny PNG image (1x1 red pixel).
    srv.tool("red_dot", "Return a 1x1 red PNG.", [] {
        static const unsigned char png[] = {
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49,
            0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xDD, 0x8D, 0xB0,
            0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
        return mcp::ToolResult{{mcp::content::text("a red dot"),
                                mcp::content::image(std::string((const char*)png, sizeof png), "image/png")},
                               nullptr, false};
    });

    srv.resource("config://app", "app config", "Static configuration",
                 [] { return std::string(R"({"theme": "dark", "lang": "ja"})"); }, "application/json");
    srv.resource_template("greeting://{name}", "greeting", "A personal greeting",
                          [](const std::map<std::string, std::string>& p) { return "こんにちは、" + p.at("name") + "さん"; });

    srv.prompt("review_code", "Ask for a code review.",
               [](const std::map<std::string, std::string>& a) {
                   return std::vector<mcp::PromptMessage>{
                       mcp::PromptMessage::user("Please review this " + (a.count("language") ? a.at("language") : "") +
                                                " code:\n\n" + a.at("code"))};
               },
               {{"code", "the code", true}, {"language", "programming language", false}});

    int port = 0;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--http") port = std::stoi(argv[i + 1]);
    if (port) srv.run_http("127.0.0.1", port);
    else srv.run_stdio();
}
