// mcp.hpp - Model Context Protocol (MCP) for C++: servers (and clients, see mcp_client.hpp).
// Header-only, on top of cpp-httplib + nlohmann/json.
//
//   mcp::Server srv("demo", "1.0.0");
//   srv.tool("add", "Add two numbers", [](int a, int b) { return a + b; }, {mcp::arg("a"), mcp::arg("b")});
//   srv.run_stdio();                         // or srv.run_http("127.0.0.1", 8000)  -> http://127.0.0.1:8000/mcp
//
// Protocol: JSON-RPC 2.0, the initialize handshake (2025-11-25, negotiating down to 2024-11-05),
// transports stdio (newline-delimited JSON) and Streamable HTTP.
#pragma once

#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00  // cpp-httplib needs Windows 10 APIs
#endif

#include "httplib.h"
#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace mcp {

using json = nlohmann::json;

inline constexpr const char* sdk_version = "0.1.0";

// Handshake-era protocol revisions we speak, newest first.
inline const std::vector<std::string>& protocol_versions() {
    static const std::vector<std::string> v = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
    return v;
}

// JSON-RPC / MCP error codes
enum : int {
    PARSE_ERROR = -32700,
    INVALID_REQUEST = -32600,
    METHOD_NOT_FOUND = -32601,
    INVALID_PARAMS = -32602,
    INTERNAL_ERROR = -32603,
    RESOURCE_NOT_FOUND = -32002,
};

// A JSON-RPC error response.
struct Error : std::runtime_error {
    int code;
    json data;
    Error(int c, const std::string& msg, json d = nullptr) : std::runtime_error(msg), code(c), data(std::move(d)) {}
};

// Throw from a tool to report a tool failure to the model (result with isError: true).
struct ToolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

namespace detail {

inline std::string random_hex(size_t n) {
    static std::mutex m;
    static std::mt19937_64 rng{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::lock_guard<std::mutex> lk(m);
    std::string s;
    for (size_t i = 0; i < n; ++i) s += hex[rng() & 15];
    return s;
}

inline std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

struct Cancelled {};

}  // namespace detail

inline std::string base64_encode(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
        out += t[v >> 18], out += t[(v >> 12) & 63], out += t[(v >> 6) & 63], out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t v = (uint8_t)in[i] << 16;
        out += t[v >> 18], out += t[(v >> 12) & 63], out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8);
        out += t[v >> 18], out += t[(v >> 12) & 63], out += t[(v >> 6) & 63], out += '=';
    }
    return out;
}

inline std::string base64_decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::string out;
    uint32_t buf = 0;
    int bits = 0;
    for (char c : in) {
        int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((buf >> bits) & 0xFF);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Content blocks (tool results, prompt messages)
// ---------------------------------------------------------------------------
namespace content {
inline json text(const std::string& s) { return {{"type", "text"}, {"text", s}}; }
inline json image(const std::string& bytes, const std::string& mime_type) {
    return {{"type", "image"}, {"data", base64_encode(bytes)}, {"mimeType", mime_type}};
}
inline json audio(const std::string& bytes, const std::string& mime_type) {
    return {{"type", "audio"}, {"data", base64_encode(bytes)}, {"mimeType", mime_type}};
}
inline json resource_link(const std::string& uri, const std::string& name, const std::string& mime_type = "",
                          const std::string& description = "") {
    json j = {{"type", "resource_link"}, {"uri", uri}, {"name", name}};
    if (!mime_type.empty()) j["mimeType"] = mime_type;
    if (!description.empty()) j["description"] = description;
    return j;
}
inline json embedded_text(const std::string& uri, const std::string& text, const std::string& mime_type = "text/plain") {
    return {{"type", "resource"}, {"resource", {{"uri", uri}, {"mimeType", mime_type}, {"text", text}}}};
}
}  // namespace content

// Full control over a tool's result.
struct ToolResult {
    std::vector<json> content;
    json structured;  // structuredContent (object) or null
    bool is_error = false;
};
inline void to_json(json& j, const ToolResult& r) {
    j = {{"content", r.content}, {"isError", r.is_error}};
    if (!r.structured.is_null()) j["structuredContent"] = r.structured;
}

// Binary resource contents (returned by a resource reader instead of std::string).
struct Blob {
    std::string bytes;
};

// One prompt message.
struct PromptMessage {
    std::string role;  // "user" / "assistant"
    json content;      // a content block, e.g. mcp::content::text("...")
    static PromptMessage user(const std::string& t) { return {"user", content::text(t)}; }
    static PromptMessage assistant(const std::string& t) { return {"assistant", content::text(t)}; }
};
inline void to_json(json& j, const PromptMessage& m) { j = {{"role", m.role}, {"content", m.content}}; }

// ---------------------------------------------------------------------------
// Context: take `mcp::Context&` (anywhere in a tool's parameters) to report progress,
// log to the client, or check for cancellation.
// ---------------------------------------------------------------------------
inline int log_level_rank(const std::string& l) {
    static const std::map<std::string, int> r = {{"debug", 0},   {"info", 1},     {"notice", 2}, {"warning", 3},
                                                 {"error", 4},   {"critical", 5}, {"alert", 6},  {"emergency", 7}};
    auto it = r.find(l);
    return it == r.end() ? 1 : it->second;
}

class Context {
public:
    // progress in [0, total] (or any increasing number if total is unknown)
    void progress(double progress, std::optional<double> total = {}, const std::string& message = "") const {
        check();
        if (progress_token_.is_null() || !send_) return;
        json p = {{"progressToken", progress_token_}, {"progress", progress}};
        if (total) p["total"] = *total;
        if (!message.empty()) p["message"] = message;
        send_({{"jsonrpc", "2.0"}, {"method", "notifications/progress"}, {"params", p}});
    }
    // notifications/message, filtered by the level the client set with logging/setLevel
    void log(const std::string& level, const json& data, const std::string& logger = "") const {
        check();
        if (!send_ || log_level_rank(level) < *min_level_) return;
        json p = {{"level", level}, {"data", data}};
        if (!logger.empty()) p["logger"] = logger;
        send_({{"jsonrpc", "2.0"}, {"method", "notifications/message"}, {"params", p}});
    }
    void debug(const json& d) const { log("debug", d); }
    void info(const json& d) const { log("info", d); }
    void warning(const json& d) const { log("warning", d); }
    void error(const json& d) const { log("error", d); }

    bool cancelled() const { return cancelled_ && *cancelled_; }
    // Throws (ending the tool) if the client cancelled the request; progress() / log() call it too.
    void check() const {
        if (cancelled()) throw detail::Cancelled{};
    }
    const json& request_id() const { return request_id_; }
    const json& meta() const { return meta_; }
    const std::string& session_id() const { return session_id_; }
    const json& client_info() const { return client_info_; }

private:
    friend class Server;
    std::function<void(const json&)> send_;
    json progress_token_, request_id_, meta_, client_info_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    std::shared_ptr<std::atomic<int>> min_level_ = std::make_shared<std::atomic<int>>(0);
    std::string session_id_;
};

// ---------------------------------------------------------------------------
// Typed tool arguments
// ---------------------------------------------------------------------------
struct Arg {
    std::string name;
    std::string description;
    json default_value;  // null: required (unless the parameter is std::optional)
    bool has_default = false;
};
inline Arg arg(std::string name, std::string description = "") { return {std::move(name), std::move(description), nullptr, false}; }
inline Arg arg(std::string name, std::string description, json default_value) {
    return {std::move(name), std::move(description), std::move(default_value), true};
}

namespace detail {

template <class T>
struct fn_traits : fn_traits<decltype(&std::decay_t<T>::operator())> {};
template <class R, class... A>
struct fn_traits<R (*)(A...)> {
    using ret = R;
    using args = std::tuple<std::decay_t<A>...>;
};
template <class R, class... A>
struct fn_traits<R(A...)> : fn_traits<R (*)(A...)> {};
template <class C, class R, class... A>
struct fn_traits<R (C::*)(A...) const> : fn_traits<R (*)(A...)> {};
template <class C, class R, class... A>
struct fn_traits<R (C::*)(A...)> : fn_traits<R (*)(A...)> {};

template <class T>
struct is_optional : std::false_type {};
template <class T>
struct is_optional<std::optional<T>> : std::true_type {};
template <class T>
struct is_vector : std::false_type {};
template <class T, class A>
struct is_vector<std::vector<T, A>> : std::true_type {};
template <class T>
struct is_str_map : std::false_type {};
template <class V>
struct is_str_map<std::map<std::string, V>> : std::true_type {};

template <class T>
json schema_of() {
    if constexpr (is_optional<T>::value) {
        return schema_of<typename T::value_type>();
    } else if constexpr (std::is_same_v<T, bool>) {
        return {{"type", "boolean"}};
    } else if constexpr (std::is_integral_v<T>) {
        return {{"type", "integer"}};
    } else if constexpr (std::is_floating_point_v<T>) {
        return {{"type", "number"}};
    } else if constexpr (std::is_same_v<T, std::string>) {
        return {{"type", "string"}};
    } else if constexpr (is_vector<T>::value) {
        return {{"type", "array"}, {"items", schema_of<typename T::value_type>()}};
    } else if constexpr (is_str_map<T>::value) {
        return {{"type", "object"}, {"additionalProperties", schema_of<typename T::mapped_type>()}};
    } else {
        return json::object();  // json or anything else: no constraint
    }
}

template <class T>
inline constexpr bool is_context_v = std::is_same_v<T, Context>;

template <class Args, size_t... J>
constexpr size_t count_plain(std::index_sequence<J...>) {
    return (size_t(0) + ... + (is_context_v<std::tuple_element_t<J, Args>> ? 0 : 1));
}

template <class T>
T get_arg(const json& args, const Arg& a) {
    if (args.is_object() && args.contains(a.name) && !(is_optional<T>::value && args[a.name].is_null())) {
        try {
            if constexpr (is_optional<T>::value) return args[a.name].template get<typename T::value_type>();
            else return args[a.name].template get<T>();
        } catch (const json::exception&) {
            throw ToolError("argument '" + a.name + "' has the wrong type (expected " + schema_of<T>().dump() + ")");
        }
    }
    if (a.has_default) {
        if constexpr (is_optional<T>::value) return a.default_value.template get<typename T::value_type>();
        else return a.default_value.template get<T>();
    }
    if constexpr (is_optional<T>::value) return std::nullopt;
    else throw ToolError("missing required argument '" + a.name + "'");
}

template <class T, class Args, size_t I>
decltype(auto) arg_at(const json& args, const std::vector<Arg>& names, Context& ctx) {
    if constexpr (is_context_v<T>) return (ctx);
    else return get_arg<T>(args, names[count_plain<Args>(std::make_index_sequence<I>{})]);
}

// A tool's return value -> CallToolResult (plus the outputSchema it implies).
template <class R>
json output_schema_for() {
    if constexpr (std::is_void_v<R> || std::is_same_v<R, ToolResult> || std::is_same_v<R, json> ||
                  std::is_same_v<R, std::string>) {
        return nullptr;
    } else {
        return {{"type", "object"}, {"properties", {{"result", schema_of<R>()}}}, {"required", {"result"}}};
    }
}

template <class R>
json to_result(R&& r) {
    using T = std::decay_t<R>;
    if constexpr (std::is_same_v<T, ToolResult>) {
        return json(r);
    } else if constexpr (std::is_same_v<T, std::string>) {
        return json(ToolResult{{content::text(r)}, nullptr, false});
    } else if constexpr (std::is_same_v<T, json>) {
        ToolResult tr{{content::text(r.is_string() ? r.template get<std::string>() : dump(r))},
                      r.is_object() ? r : json(nullptr), false};
        return json(tr);
    } else {
        json v = r;
        return json(ToolResult{{content::text(v.is_string() ? v.template get<std::string>() : dump(v))},
                               json{{"result", v}}, false});
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
class Server {
public:
    using ToolFn = std::function<json(const json& arguments, Context& ctx)>;  // returns a CallToolResult

    struct Tool {
        std::string name, title, description;
        json input_schema, output_schema, annotations;
        ToolFn fn;
        // fluent setters
        Tool& with_title(std::string t) { title = std::move(t); return *this; }
        Tool& with_annotations(json a) { annotations = std::move(a); return *this; }
        Tool& read_only(bool b = true) { annotations["readOnlyHint"] = b; return *this; }
        Tool& destructive(bool b = true) { annotations["destructiveHint"] = b; return *this; }
        Tool& idempotent(bool b = true) { annotations["idempotentHint"] = b; return *this; }
        Tool& open_world(bool b = true) { annotations["openWorldHint"] = b; return *this; }
    };

    struct Resource {
        std::string uri, name, title, description, mime_type;
        std::function<json(const std::map<std::string, std::string>& params)> read;  // -> contents array
        std::regex pattern;  // for templates
        std::vector<std::string> vars;
        bool is_template = false;
    };

    struct PromptArgument {
        std::string name, description;
        bool required = false;
    };
    struct Prompt {
        std::string name, title, description;
        std::vector<PromptArgument> arguments;
        std::function<std::vector<PromptMessage>(const std::map<std::string, std::string>&)> fn;
    };

    explicit Server(std::string name, std::string version = "0.1.0", std::string instructions = "")
        : name_(std::move(name)), version_(std::move(version)), instructions_(std::move(instructions)) {}
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // ---- tools ----

    // Typed tool: parameters become JSON-schema properties named by `args` (in order;
    // a `mcp::Context&` parameter is skipped). Return std::string, a number, bool, a
    // vector/map, mcp::json, mcp::ToolResult or void. Throw mcp::ToolError for a tool failure.
    template <class F>
    Tool& tool(const std::string& name, const std::string& description, F fn, std::vector<Arg> args = {}) {
        using Tr = detail::fn_traits<F>;
        using Args = typename Tr::args;
        using R = typename Tr::ret;
        static constexpr size_t N = std::tuple_size_v<Args>;
        static constexpr size_t NP = detail::count_plain<Args>(std::make_index_sequence<N>{});
        if (args.size() != NP)
            throw std::invalid_argument("tool '" + name + "': " + std::to_string(NP) + " parameter(s) but " +
                                        std::to_string(args.size()) + " mcp::arg name(s)");
        json props = json::object(), required = json::array();
        fill_schema<Args>(args, props, required, std::make_index_sequence<N>{});
        json schema = {{"type", "object"}, {"properties", props}};
        if (!required.empty()) schema["required"] = required;
        auto names = std::make_shared<std::vector<Arg>>(std::move(args));
        ToolFn wrapped = [fn, names](const json& a, Context& ctx) mutable -> json {
            return invoke<F, Args, R>(fn, a, *names, ctx, std::make_index_sequence<N>{});
        };
        return tool_raw(name, description, schema, std::move(wrapped), detail::output_schema_for<R>());
    }

    // Untyped tool with your own input schema; fn returns a CallToolResult (json(ToolResult{...})).
    Tool& tool_raw(const std::string& name, const std::string& description, json input_schema, ToolFn fn,
                   json output_schema = nullptr) {
        static const std::regex valid("^[A-Za-z0-9_.\\-]{1,128}$");
        if (!std::regex_match(name, valid)) throw std::invalid_argument("invalid tool name '" + name + "'");
        std::lock_guard<std::mutex> lk(m_);
        for (auto& t : tools_)
            if (t->name == name) throw std::invalid_argument("duplicate tool '" + name + "'");
        auto t = std::make_shared<Tool>();
        t->name = name;
        t->description = description;
        t->input_schema = std::move(input_schema);
        t->output_schema = std::move(output_schema);
        t->fn = std::move(fn);
        tools_.push_back(t);
        return *t;
    }

    // ---- resources ----

    // Fixed URI. read() returns std::string (text) or mcp::Blob (binary).
    template <class F>
    Resource& resource(const std::string& uri, const std::string& name, const std::string& description, F read,
                       const std::string& mime_type = "text/plain") {
        auto r = std::make_shared<Resource>();
        r->uri = uri, r->name = name, r->description = description, r->mime_type = mime_type;
        r->read = [read, uri, mime_type](const std::map<std::string, std::string>&) {
            return contents(uri, mime_type, read());
        };
        std::lock_guard<std::mutex> lk(m_);
        resources_.push_back(r);
        return *r;
    }

    // URI template like "file:///logs/{name}" (RFC 6570 level 1): read(params) gets the variables.
    template <class F>
    Resource& resource_template(const std::string& uri_template, const std::string& name, const std::string& description,
                                F read, const std::string& mime_type = "text/plain") {
        auto r = std::make_shared<Resource>();
        r->uri = uri_template, r->name = name, r->description = description, r->mime_type = mime_type;
        r->is_template = true;
        std::string re;
        for (size_t i = 0; i < uri_template.size(); ++i) {
            char c = uri_template[i];
            if (c == '{') {
                size_t e = uri_template.find('}', i);
                if (e == std::string::npos) throw std::invalid_argument("bad uri template " + uri_template);
                r->vars.push_back(uri_template.substr(i + 1, e - i - 1));
                re += "([^/]+)";
                i = e;
            } else if (std::string(".^$|()[]*+?\\").find(c) != std::string::npos) {
                re += '\\', re += c;
            } else {
                re += c;
            }
        }
        r->pattern = std::regex("^" + re + "$");
        r->read = [read, mime_type](const std::map<std::string, std::string>& p) {
            return contents(p.at("__uri"), mime_type, read(p));
        };
        std::lock_guard<std::mutex> lk(m_);
        resources_.push_back(r);
        return *r;
    }

    // ---- prompts ----

    // fn(arguments) returns std::string (one user message) or std::vector<PromptMessage>.
    template <class F>
    Prompt& prompt(const std::string& name, const std::string& description, F fn, std::vector<PromptArgument> arguments = {}) {
        auto p = std::make_shared<Prompt>();
        p->name = name, p->description = description, p->arguments = std::move(arguments);
        p->fn = [fn](const std::map<std::string, std::string>& a) -> std::vector<PromptMessage> {
            using R = std::decay_t<decltype(fn(a))>;
            if constexpr (std::is_same_v<R, std::vector<PromptMessage>>) return fn(a);
            else return {PromptMessage::user(fn(a))};
        };
        std::lock_guard<std::mutex> lk(m_);
        prompts_.push_back(p);
        return *p;
    }

    // ---- protocol core ----

    struct Session {
        std::string id;
        std::string protocol_version;
        json client_info, client_capabilities;
        bool initialized = false;
        std::shared_ptr<std::atomic<int>> min_level = std::make_shared<std::atomic<int>>(0);
        std::mutex m;
        std::map<std::string, std::shared_ptr<std::atomic<bool>>> running;  // request id (dumped) -> cancel flag
        std::chrono::steady_clock::time_point touched = std::chrono::steady_clock::now();
    };

    // Handle one JSON-RPC message. `send` delivers notifications produced while handling a
    // request (progress, logs). Returns the response, or null for notifications/responses.
    json handle(const json& msg, Session& s, const std::function<void(const json&)>& send) {
        if (!msg.is_object() || msg.value("jsonrpc", "") != "2.0")
            return error_response(msg.is_object() && msg.contains("id") ? msg["id"] : json(nullptr), INVALID_REQUEST,
                                  "not a JSON-RPC 2.0 message");
        bool is_request = msg.contains("method") && msg.contains("id");
        bool is_notification = msg.contains("method") && !msg.contains("id");
        if (!is_request && !is_notification) return nullptr;  // a response to a server request: ignored
        const std::string method = msg["method"].is_string() ? msg["method"].get<std::string>() : "";
        const json params = msg.contains("params") && msg["params"].is_object() ? msg["params"] : json::object();
        if (is_notification) {
            if (method == "notifications/initialized") s.initialized = true;
            else if (method == "notifications/cancelled" && params.contains("requestId")) {
                std::lock_guard<std::mutex> lk(s.m);
                auto it = s.running.find(detail::dump(params["requestId"]));
                if (it != s.running.end()) *it->second = true;
            }
            return nullptr;
        }
        const json id = msg["id"];
        try {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", dispatch(method, params, id, s, send)}};
        } catch (const Error& e) {
            return error_response(id, e.code, e.what(), e.data);
        } catch (const detail::Cancelled&) {
            return error_response(id, INTERNAL_ERROR, "request cancelled");
        } catch (const std::exception& e) {
            return error_response(id, INTERNAL_ERROR, e.what());
        }
    }

    // ---- transports ----

    // stdio: newline-delimited JSON-RPC on stdin/stdout (logs must go to stderr).
    void run_stdio() {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        Session s;
        std::mutex out_m;
        auto write = [&](const json& j) {
            std::string line = detail::dump(j) + "\n";
            std::lock_guard<std::mutex> lk(out_m);
            std::fwrite(line.data(), 1, line.size(), stdout);
            std::fflush(stdout);
        };
        std::atomic<int> active{0};
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            json msg = json::parse(line, nullptr, false);
            if (msg.is_discarded()) {
                write(error_response(nullptr, PARSE_ERROR, "invalid JSON"));
                continue;
            }
            std::vector<json> batch = msg.is_array() ? msg.get<std::vector<json>>() : std::vector<json>{msg};
            for (auto& m : batch) {
                bool slow = m.is_object() && m.contains("id") && m.value("method", "") == "tools/call";
                if (!slow) {
                    json r = handle(m, s, write);
                    if (!r.is_null()) write(r);
                    continue;
                }
                ++active;  // tools run on their own thread, so pings and cancellations still get through
                std::thread([this, m, &s, write, &active] {
                    json r = handle(m, s, write);
                    if (!r.is_null()) write(r);
                    --active;
                }).detach();
            }
        }
        while (active > 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Streamable HTTP on an existing server, at `path` (default "/mcp").
    void mount(httplib::Server& svr, const std::string& path = "/mcp") {
        svr.Post(path, [this](const httplib::Request& req, httplib::Response& res) { http_post(req, res); });
        svr.Get(path, [](const httplib::Request&, httplib::Response& res) {
            res.status = 405;  // no server-initiated stream
            res.set_header("Allow", "POST, DELETE");
        });
        svr.Delete(path, [this](const httplib::Request& req, httplib::Response& res) {
            std::lock_guard<std::mutex> lk(sessions_m_);
            res.status = sessions_.erase(req.get_header_value("Mcp-Session-Id")) ? 200 : 404;
        });
    }

    // Streamable HTTP server; blocks. Binds to localhost by default (MCP guidance).
    void run_http(const std::string& host = "127.0.0.1", int port = 8000, const std::string& path = "/mcp") {
        httplib::Server svr;
        mount(svr, path);
        if (!svr.bind_to_port(host, port)) throw std::runtime_error("cannot bind " + host + ":" + std::to_string(port));
        std::cerr << "MCP server " << name_ << " on http://" << host << ":" << port << path << std::endl;
        svr.listen_after_bind();
    }

    // Origins allowed to call the HTTP endpoint besides localhost (DNS-rebinding protection).
    void allow_origin(const std::string& origin) { allowed_origins_.insert(origin); }

private:
    template <class Args, size_t... I>
    static void fill_schema(const std::vector<Arg>& args, json& props, json& required, std::index_sequence<I...>) {
        (add_prop<std::tuple_element_t<I, Args>, Args, I>(args, props, required), ...);
    }
    template <class T, class Args, size_t I>
    static void add_prop(const std::vector<Arg>& args, json& props, json& required) {
        if constexpr (!detail::is_context_v<T>) {
            const Arg& a = args[detail::count_plain<Args>(std::make_index_sequence<I>{})];
            json s = detail::schema_of<T>();
            if (!a.description.empty()) s["description"] = a.description;
            if (a.has_default) s["default"] = a.default_value;
            props[a.name] = s;
            if (!a.has_default && !detail::is_optional<T>::value) required.push_back(a.name);
        }
    }
    template <class F, class Args, class R, size_t... I>
    static json invoke(F& fn, const json& a, const std::vector<Arg>& names, Context& ctx, std::index_sequence<I...>) {
        if constexpr (std::is_void_v<R>) {
            fn(detail::arg_at<std::tuple_element_t<I, Args>, Args, I>(a, names, ctx)...);
            return json(ToolResult{});
        } else {
            return detail::to_result(fn(detail::arg_at<std::tuple_element_t<I, Args>, Args, I>(a, names, ctx)...));
        }
    }

    template <class V>
    static json contents(const std::string& uri, const std::string& mime, V&& v) {
        using T = std::decay_t<V>;
        json c = {{"uri", uri}, {"mimeType", mime}};
        if constexpr (std::is_same_v<T, Blob>) c["blob"] = base64_encode(v.bytes);
        else c["text"] = std::string(v);
        return json::array({c});
    }

    static json error_response(const json& id, int code, const std::string& message, const json& data = nullptr) {
        json e = {{"code", code}, {"message", message}};
        if (!data.is_null()) e["data"] = data;
        return {{"jsonrpc", "2.0"}, {"id", id}, {"error", e}};
    }

    json dispatch(const std::string& method, const json& params, const json& id, Session& s,
                  const std::function<void(const json&)>& send) {
        if (method == "initialize") return initialize(params, s);
        if (method == "ping") return json::object();
        if (method == "tools/list") {
            json list = json::array();
            std::lock_guard<std::mutex> lk(m_);
            for (auto& t : tools_) {
                json j = {{"name", t->name}, {"description", t->description}, {"inputSchema", t->input_schema}};
                if (!t->title.empty()) j["title"] = t->title;
                if (!t->output_schema.is_null()) j["outputSchema"] = t->output_schema;
                if (!t->annotations.is_null()) j["annotations"] = t->annotations;
                list.push_back(j);
            }
            return {{"tools", list}};
        }
        if (method == "tools/call") return call_tool(params, id, s, send);
        if (method == "resources/list" || method == "resources/templates/list") {
            bool templates = method == "resources/templates/list";
            json list = json::array();
            std::lock_guard<std::mutex> lk(m_);
            for (auto& r : resources_) {
                if (r->is_template != templates) continue;
                json j = {{templates ? "uriTemplate" : "uri", r->uri}, {"name", r->name}, {"mimeType", r->mime_type}};
                if (!r->description.empty()) j["description"] = r->description;
                if (!r->title.empty()) j["title"] = r->title;
                list.push_back(j);
            }
            return {{templates ? "resourceTemplates" : "resources", list}};
        }
        if (method == "resources/read") {
            std::string uri = params.value("uri", "");
            std::shared_ptr<Resource> hit;
            std::map<std::string, std::string> vars;
            {
                std::lock_guard<std::mutex> lk(m_);
                for (auto& r : resources_)
                    if (!r->is_template && r->uri == uri) hit = r;
                if (!hit)
                    for (auto& r : resources_) {
                        std::smatch mm;
                        if (r->is_template && std::regex_match(uri, mm, r->pattern)) {
                            for (size_t k = 0; k < r->vars.size(); ++k) vars[r->vars[k]] = mm[k + 1];
                            hit = r;
                            break;
                        }
                    }
            }
            if (!hit) throw Error(RESOURCE_NOT_FOUND, "resource not found", {{"uri", uri}});
            vars["__uri"] = uri;
            return {{"contents", hit->read(vars)}};
        }
        if (method == "prompts/list") {
            json list = json::array();
            std::lock_guard<std::mutex> lk(m_);
            for (auto& p : prompts_) {
                json args = json::array();
                for (auto& a : p->arguments) {
                    json aj = {{"name", a.name}, {"required", a.required}};
                    if (!a.description.empty()) aj["description"] = a.description;
                    args.push_back(aj);
                }
                json j = {{"name", p->name}, {"description", p->description}, {"arguments", args}};
                if (!p->title.empty()) j["title"] = p->title;
                list.push_back(j);
            }
            return {{"prompts", list}};
        }
        if (method == "prompts/get") {
            std::string name = params.value("name", "");
            std::shared_ptr<Prompt> p;
            {
                std::lock_guard<std::mutex> lk(m_);
                for (auto& q : prompts_)
                    if (q->name == name) p = q;
            }
            if (!p) throw Error(INVALID_PARAMS, "unknown prompt '" + name + "'");
            std::map<std::string, std::string> a;
            if (params.contains("arguments") && params["arguments"].is_object())
                for (auto& [k, v] : params["arguments"].items()) a[k] = v.is_string() ? v.get<std::string>() : v.dump();
            for (auto& pa : p->arguments)
                if (pa.required && !a.count(pa.name)) throw Error(INVALID_PARAMS, "missing prompt argument '" + pa.name + "'");
            return {{"description", p->description}, {"messages", p->fn(a)}};
        }
        if (method == "logging/setLevel") {
            *s.min_level = log_level_rank(params.value("level", "info"));
            return json::object();
        }
        if (method == "completion/complete") return {{"completion", {{"values", json::array()}, {"hasMore", false}}}};
        throw Error(METHOD_NOT_FOUND, "method not found: " + method);
    }

    json initialize(const json& params, Session& s) {
        std::string want = params.value("protocolVersion", "");
        const auto& vs = protocol_versions();
        s.protocol_version = std::find(vs.begin(), vs.end(), want) != vs.end() ? want : vs.front();
        s.client_info = params.value("clientInfo", json::object());
        s.client_capabilities = params.value("capabilities", json::object());
        json caps = {{"logging", json::object()}};
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!tools_.empty()) caps["tools"] = {{"listChanged", false}};
            if (!resources_.empty()) caps["resources"] = {{"subscribe", false}, {"listChanged", false}};
            if (!prompts_.empty()) caps["prompts"] = {{"listChanged", false}};
        }
        json r = {{"protocolVersion", s.protocol_version},
                  {"capabilities", caps},
                  {"serverInfo", {{"name", name_}, {"version", version_}}}};
        if (!instructions_.empty()) r["instructions"] = instructions_;
        return r;
    }

    json call_tool(const json& params, const json& id, Session& s, const std::function<void(const json&)>& send) {
        std::string name = params.value("name", "");
        std::shared_ptr<Tool> t;
        {
            std::lock_guard<std::mutex> lk(m_);
            for (auto& x : tools_)
                if (x->name == name) t = x;
        }
        if (!t) throw Error(INVALID_PARAMS, "unknown tool '" + name + "'");
        Context ctx;
        ctx.send_ = send;
        ctx.request_id_ = id;
        ctx.meta_ = params.value("_meta", json::object());
        if (ctx.meta_.contains("progressToken")) ctx.progress_token_ = ctx.meta_["progressToken"];
        ctx.cancelled_ = std::make_shared<std::atomic<bool>>(false);
        ctx.min_level_ = s.min_level;
        ctx.session_id_ = s.id;
        ctx.client_info_ = s.client_info;
        std::string key = detail::dump(id);
        {
            std::lock_guard<std::mutex> lk(s.m);
            s.running[key] = ctx.cancelled_;
        }
        struct Done {
            Session& s;
            std::string key;
            ~Done() {
                std::lock_guard<std::mutex> lk(s.m);
                s.running.erase(key);
            }
        } done{s, key};
        json args = params.value("arguments", json::object());
        try {
            return t->fn(args, ctx);
        } catch (const ToolError& e) {
            return json(ToolResult{{content::text(e.what())}, nullptr, true});
        } catch (const Error&) {
            throw;
        } catch (const detail::Cancelled&) {
            throw;
        } catch (const std::exception& e) {
            return json(ToolResult{{content::text(std::string("tool error: ") + e.what())}, nullptr, true});
        }
    }

    // ---- Streamable HTTP ----
    bool origin_ok(const httplib::Request& req) const {
        std::string o = req.get_header_value("Origin");
        if (o.empty()) return true;  // not a browser
        static const std::regex local(R"(^https?://(localhost|127\.0\.0\.1|\[::1\])(:\d+)?$)");
        return std::regex_match(o, local) || allowed_origins_.count(o);
    }

    static void json_error(httplib::Response& res, int status, const json& id, int code, const std::string& msg) {
        res.status = status;
        res.set_content(detail::dump(error_response(id, code, msg)), "application/json");
    }

    void http_post(const httplib::Request& req, httplib::Response& res) {
        if (!origin_ok(req)) return json_error(res, 403, nullptr, INVALID_REQUEST, "origin not allowed");
        json msg = json::parse(req.body, nullptr, false);
        if (msg.is_discarded()) return json_error(res, 400, nullptr, PARSE_ERROR, "invalid JSON");
        std::string pv = req.get_header_value("MCP-Protocol-Version");
        if (!pv.empty()) {
            const auto& vs = protocol_versions();
            if (std::find(vs.begin(), vs.end(), pv) == vs.end())
                return json_error(res, 400, nullptr, INVALID_REQUEST, "unsupported MCP-Protocol-Version " + pv);
        }
        std::vector<json> batch = msg.is_array() ? msg.get<std::vector<json>>() : std::vector<json>{msg};
        bool any_request = false, has_init = false;
        for (auto& m : batch)
            if (m.is_object() && m.contains("method") && m.contains("id")) {
                any_request = true;
                if (m["method"] == "initialize") has_init = true;
            }

        std::shared_ptr<Session> s;
        if (has_init) {
            s = std::make_shared<Session>();
            s->id = detail::random_hex(32);
            std::lock_guard<std::mutex> lk(sessions_m_);
            prune_sessions();
            sessions_[s->id] = s;
            res.set_header("Mcp-Session-Id", s->id);
        } else {
            std::string sid = req.get_header_value("Mcp-Session-Id");
            json id = batch.size() == 1 && batch[0].is_object() && batch[0].contains("id") ? batch[0]["id"] : json(nullptr);
            if (sid.empty()) {
                // e.g. a 2026-era client probing with server/discover: answer as an unknown method
                if (batch.size() == 1 && batch[0].is_object() && batch[0].value("method", "") == "server/discover")
                    return json_error(res, 200, id, METHOD_NOT_FOUND, "method not found: server/discover");
                return json_error(res, 400, id, INVALID_REQUEST, "missing Mcp-Session-Id (send initialize first)");
            }
            std::lock_guard<std::mutex> lk(sessions_m_);
            auto it = sessions_.find(sid);
            if (it == sessions_.end()) return json_error(res, 404, id, INVALID_REQUEST, "unknown session");
            s = it->second;
            s->touched = std::chrono::steady_clock::now();
        }

        if (!any_request) {  // notifications / responses only
            for (auto& m : batch) handle(m, *s, nullptr);
            res.status = 202;
            return;
        }
        bool sse = req.get_header_value("Accept").find("text/event-stream") != std::string::npos;
        bool slow = false;
        for (auto& m : batch)
            if (m.is_object() && m.value("method", "") == "tools/call") slow = true;
        if (!sse || !slow) {  // plain JSON response(s)
            json out = json::array();
            for (auto& m : batch) {
                json r = handle(m, *s, nullptr);
                if (!r.is_null()) out.push_back(r);
            }
            res.set_content(detail::dump(msg.is_array() ? out : out[0]), "application/json");
            return;
        }
        // tools/call: an SSE stream carrying progress / log notifications, then the response
        res.set_header("Cache-Control", "no-cache");
        res.set_chunked_content_provider("text/event-stream", [this, batch, s](size_t, httplib::DataSink& sink) {
            bool alive = true;
            std::mutex wm;
            auto write = [&](const json& j) {
                std::lock_guard<std::mutex> lk(wm);
                std::string ev = "event: message\ndata: " + detail::dump(j) + "\n\n";
                if (alive && !sink.write(ev.data(), ev.size())) {
                    alive = false;  // client went away: cancel what it asked for
                    std::lock_guard<std::mutex> sl(s->m);
                    for (auto& [k, f] : s->running) *f = true;
                }
            };
            for (auto& m : batch) {
                json r = handle(m, *s, write);
                if (!r.is_null()) write(r);
            }
            sink.done();
            return true;
        });
    }

    void prune_sessions() {
        auto now = std::chrono::steady_clock::now();
        for (auto it = sessions_.begin(); it != sessions_.end();)
            it = now - it->second->touched > std::chrono::hours(24) ? sessions_.erase(it) : std::next(it);
    }

    std::string name_, version_, instructions_;
    std::mutex m_;
    std::vector<std::shared_ptr<Tool>> tools_;
    std::vector<std::shared_ptr<Resource>> resources_;
    std::vector<std::shared_ptr<Prompt>> prompts_;
    std::mutex sessions_m_;
    std::map<std::string, std::shared_ptr<Session>> sessions_;
    std::set<std::string> allowed_origins_;
};

}  // namespace mcp
