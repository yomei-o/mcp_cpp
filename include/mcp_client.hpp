// mcp_client.hpp - MCP client: talk to any MCP server over stdio (spawns it) or Streamable HTTP.
//
//   auto c = mcp::Client::stdio({"python", "server.py"});     // or mcp::Client::http("http://127.0.0.1:8000/mcp")
//   c.initialize();
//   for (auto& t : c.list_tools()) std::cout << t["name"] << "\n";
//   mcp::json r = c.call_tool("add", {{"a", 1}, {"b", 2}});
#pragma once

#include "mcp.hpp"

#include <future>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mcp {

// Moves JSON-RPC messages; one implementation per transport.
class Transport {
public:
    using Handler = std::function<void(const json&)>;
    virtual ~Transport() = default;
    // Send a request; returns its response. Messages that arrive meanwhile (notifications)
    // go to on_message.
    virtual json request(const json& msg, const Handler& on_message, double timeout_s) = 0;
    virtual void notify(const json& msg) = 0;
    virtual void close() {}
};

// ---------------------------------------------------------------------------
// stdio: spawn the server and exchange newline-delimited JSON over its pipes
// ---------------------------------------------------------------------------
class StdioTransport : public Transport {
public:
    explicit StdioTransport(const std::vector<std::string>& argv) {
        if (argv.empty()) throw std::invalid_argument("empty command");
        spawn(argv);
        reader_ = std::thread([this] { read_loop(); });
    }
    ~StdioTransport() override { close(); }

    json request(const json& msg, const Handler& on_message, double timeout_s) override {
        std::string key = detail::dump(msg["id"]);
        auto w = std::make_shared<Waiter>();
        w->on_message = on_message;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (dead_) throw Error(INTERNAL_ERROR, "server process has exited");
            waiting_[key] = w;
        }
        write(msg);
        std::unique_lock<std::mutex> lk(w->m);
        bool ok = w->cv.wait_for(lk, std::chrono::milliseconds((long long)(timeout_s * 1000)), [&] { return w->done; });
        {
            std::lock_guard<std::mutex> g(m_);
            waiting_.erase(key);
        }
        if (!ok) throw Error(INTERNAL_ERROR, "timed out waiting for " + msg.value("method", std::string("response")));
        if (w->response.is_null()) throw Error(INTERNAL_ERROR, "server process has exited");
        return w->response;
    }

    void notify(const json& msg) override { write(msg); }

    // Messages not tied to a pending request (e.g. logs) go here.
    void set_default_handler(Handler h) {
        std::lock_guard<std::mutex> lk(m_);
        default_ = std::move(h);
    }

    void close() override {
        if (closed_.exchange(true)) return;
#ifdef _WIN32
        if (in_w_) CloseHandle(in_w_), in_w_ = nullptr;
        if (WaitForSingleObject(proc_, 2000) == WAIT_TIMEOUT) TerminateProcess(proc_, 1);
        if (reader_.joinable()) reader_.join();
        if (out_r_) CloseHandle(out_r_), out_r_ = nullptr;
        CloseHandle(proc_);
#else
        if (in_w_ >= 0) ::close(in_w_), in_w_ = -1;
        for (int i = 0; i < 200 && waitpid(pid_, nullptr, WNOHANG) == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        kill(pid_, SIGTERM);
        waitpid(pid_, nullptr, 0);
        if (reader_.joinable()) reader_.join();
        if (out_r_ >= 0) ::close(out_r_), out_r_ = -1;
#endif
    }

private:
    struct Waiter {
        std::mutex m;
        std::condition_variable cv;
        json response;
        bool done = false;
        Handler on_message;
    };

    void write(const json& j) {
        std::string line = detail::dump(j) + "\n";
        std::lock_guard<std::mutex> lk(write_m_);
#ifdef _WIN32
        DWORD n = 0;
        if (!WriteFile(in_w_, line.data(), (DWORD)line.size(), &n, nullptr)) throw Error(INTERNAL_ERROR, "write failed");
#else
        size_t off = 0;
        while (off < line.size()) {
            ssize_t n = ::write(in_w_, line.data() + off, line.size() - off);
            if (n <= 0) throw Error(INTERNAL_ERROR, "write failed");
            off += (size_t)n;
        }
#endif
    }

    void dispatch(const std::string& line) {
        json msg = json::parse(line, nullptr, false);
        if (msg.is_discarded() || !msg.is_object()) return;
        if (msg.contains("id") && !msg.contains("method")) {  // a response
            std::shared_ptr<Waiter> w;
            {
                std::lock_guard<std::mutex> lk(m_);
                auto it = waiting_.find(detail::dump(msg["id"]));
                if (it != waiting_.end()) w = it->second;
            }
            if (!w) return;
            std::lock_guard<std::mutex> lk(w->m);
            w->response = msg;
            w->done = true;
            w->cv.notify_all();
            return;
        }
        // notifications (and server->client requests, which we answer as unsupported)
        if (msg.contains("id")) {
            write({{"jsonrpc", "2.0"}, {"id", msg["id"]},
                   {"error", {{"code", METHOD_NOT_FOUND}, {"message", "client does not support " + msg.value("method", "")}}}});
            return;
        }
        std::vector<Handler> hs;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (msg.value("method", "") == "notifications/progress") {  // each waiter checks its own token
                for (auto& [k, w] : waiting_)
                    if (w->on_message) hs.push_back(w->on_message);
            } else if (default_) {
                hs.push_back(default_);
            } else {
                for (auto& [k, w] : waiting_)
                    if (w->on_message) {
                        hs.push_back(w->on_message);
                        break;
                    }
            }
        }
        for (auto& h : hs) h(msg);  // each handler filters what is its own (e.g. by progressToken)
    }

    void read_loop() {
        std::string buf;
        char chunk[4096];
        for (;;) {
#ifdef _WIN32
            DWORD n = 0;
            if (!ReadFile(out_r_, chunk, sizeof chunk, &n, nullptr) || n == 0) break;
#else
            ssize_t n = ::read(out_r_, chunk, sizeof chunk);
            if (n <= 0) break;
#endif
            buf.append(chunk, (size_t)n);
            size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) dispatch(line);
            }
        }
        std::lock_guard<std::mutex> lk(m_);
        dead_ = true;
        for (auto& [k, w] : waiting_) {
            std::lock_guard<std::mutex> wl(w->m);
            w->done = true;
            w->cv.notify_all();
        }
    }

#ifdef _WIN32
    static std::wstring widen(const std::string& s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
        return w;
    }
    static std::wstring quote(const std::wstring& a) {  // CommandLineToArgvW rules
        if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
        std::wstring q = L"\"";
        size_t bs = 0;
        for (wchar_t c : a) {
            if (c == L'\\') { ++bs; continue; }
            if (c == L'"') q.append(bs * 2 + 1, L'\\');
            else q.append(bs, L'\\');
            bs = 0;
            q += c;
        }
        q.append(bs * 2, L'\\');
        return q + L"\"";
    }
    void spawn(const std::vector<std::string>& argv) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE in_r, out_w;
        if (!CreatePipe(&in_r, &in_w_, &sa, 0) || !CreatePipe(&out_r_, &out_w, &sa, 0))
            throw std::runtime_error("CreatePipe failed");
        SetHandleInformation(in_w_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_r_, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in_r;
        si.hStdOutput = out_w;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        std::wstring cmd;
        for (size_t i = 0; i < argv.size(); ++i) {
            std::wstring a = widen(argv[i]);
            if (i == 0)  // CreateProcess wants backslashes in the program path
                for (auto& ch : a)
                    if (ch == L'/') ch = L'\\';
            cmd += (cmd.empty() ? L"" : L" ") + quote(a);
        }
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(in_r);
        CloseHandle(out_w);
        if (!ok) throw std::runtime_error("cannot start " + argv[0]);
        CloseHandle(pi.hThread);
        proc_ = pi.hProcess;
    }
    HANDLE in_w_ = nullptr, out_r_ = nullptr, proc_ = nullptr;
#else
    void spawn(const std::vector<std::string>& argv) {
        int in[2], out[2];
        if (pipe(in) || pipe(out)) throw std::runtime_error("pipe failed");
        pid_ = fork();
        if (pid_ < 0) throw std::runtime_error("fork failed");
        if (pid_ == 0) {
            dup2(in[0], 0);
            dup2(out[1], 1);
            ::close(in[0]), ::close(in[1]), ::close(out[0]), ::close(out[1]);
            std::vector<char*> av;
            for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
            av.push_back(nullptr);
            execvp(av[0], av.data());
            _exit(127);
        }
        ::close(in[0]);
        ::close(out[1]);
        in_w_ = in[1];
        out_r_ = out[0];
        signal(SIGPIPE, SIG_IGN);
    }
    int in_w_ = -1, out_r_ = -1;
    pid_t pid_ = -1;
#endif

    std::mutex m_, write_m_;
    std::map<std::string, std::shared_ptr<Waiter>> waiting_;
    Handler default_;
    std::thread reader_;
    bool dead_ = false;
    std::atomic<bool> closed_{false};
};

// ---------------------------------------------------------------------------
// Streamable HTTP
// ---------------------------------------------------------------------------
class HttpTransport : public Transport {
public:
    // url like "http://127.0.0.1:8000/mcp"
    explicit HttpTransport(const std::string& url) {
        static const std::regex re(R"(^(https?://[^/]+)(/.*)?$)");
        std::smatch m;
        if (!std::regex_match(url, m, re)) throw std::invalid_argument("bad url " + url);
        base_ = m[1];
        path_ = m[2].matched ? std::string(m[2]) : "/";
    }
    ~HttpTransport() override { close(); }

    void set_protocol_version(const std::string& v) { protocol_ = v; }

    json request(const json& msg, const Handler& on_message, double timeout_s) override {
        json response;
        std::string key = detail::dump(msg["id"]);
        post(msg, timeout_s, [&](const json& m) {
            if (m.contains("id") && !m.contains("method") && detail::dump(m["id"]) == key) response = m;
            else if (on_message && !m.contains("id")) on_message(m);
        });
        if (response.is_null()) throw Error(INTERNAL_ERROR, "no response to " + msg.value("method", std::string("request")));
        return response;
    }

    void notify(const json& msg) override {
        post(msg, 30, [](const json&) {});
    }

    void close() override {
        if (session_.empty()) return;
        httplib::Client cli(base_);
        cli.Delete(path_, {{"Mcp-Session-Id", session_}});
        session_.clear();
    }

private:
    void post(const json& msg, double timeout_s, const std::function<void(const json&)>& on_msg) {
        httplib::Client cli(base_);
        cli.set_read_timeout((time_t)timeout_s, 0);
        httplib::Request req;
        req.method = "POST";
        req.path = path_;
        req.headers = {{"Accept", "application/json, text/event-stream"}};
        if (!session_.empty()) req.headers.emplace("Mcp-Session-Id", session_);
        if (!protocol_.empty()) req.headers.emplace("MCP-Protocol-Version", protocol_);
        req.set_header("Content-Type", "application/json");
        req.body = detail::dump(msg);
        std::string body, sse_buf;
        bool is_sse = false, decided = false;
        req.response_handler = [&](const httplib::Response& r) {
            is_sse = r.get_header_value("Content-Type").find("text/event-stream") != std::string::npos;
            decided = true;
            std::string sid = r.get_header_value("Mcp-Session-Id");
            if (!sid.empty()) session_ = sid;
            return true;
        };
        req.content_receiver = [&](const char* data, size_t n, uint64_t, uint64_t) {
            if (!(decided && is_sse)) {
                body.append(data, n);
                return true;
            }
            sse_buf.append(data, n);
            for (;;) {  // events end with a blank line
                size_t a = sse_buf.find("\n\n"), b = sse_buf.find("\r\n\r\n");
                size_t pos = std::min(a, b);
                if (pos == std::string::npos) break;
                std::string ev = sse_buf.substr(0, pos);
                sse_buf.erase(0, pos + (pos == b ? 4 : 2));
                std::string data_lines;
                std::istringstream ss(ev);
                std::string line;
                while (std::getline(ss, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.rfind("data:", 0) == 0) data_lines += line.substr(line.size() > 5 && line[5] == ' ' ? 6 : 5);
                }
                json j = json::parse(data_lines, nullptr, false);
                if (!j.is_discarded()) on_msg(j);
            }
            return true;
        };
        httplib::Response res;
        httplib::Error err = httplib::Error::Success;
        if (!cli.send(req, res, err)) throw Error(INTERNAL_ERROR, "HTTP error: " + httplib::to_string(err));
        if (res.status == 202) return;
        if (!is_sse) {
            json j = json::parse(body, nullptr, false);
            if (j.is_discarded()) throw Error(INTERNAL_ERROR, "HTTP " + std::to_string(res.status) + ": " + body);
            if (j.is_array())
                for (auto& x : j) on_msg(x);
            else
                on_msg(j);
        }
    }

    std::string base_, path_, session_, protocol_;
};

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------
class Client {
public:
    using ProgressFn = std::function<void(double progress, std::optional<double> total, const std::string& message)>;
    using LogFn = std::function<void(const std::string& level, const json& data)>;

    explicit Client(std::unique_ptr<Transport> t) : t_(std::move(t)) {}
    static Client stdio(const std::vector<std::string>& argv) { return Client(std::make_unique<StdioTransport>(argv)); }
    static Client http(const std::string& url) { return Client(std::make_unique<HttpTransport>(url)); }
    Client(Client&&) = default;
    ~Client() {
        if (t_) t_->close();
    }

    void on_log(LogFn f) {
        log_ = std::move(f);
        if (auto* s = dynamic_cast<StdioTransport*>(t_.get()))
            s->set_default_handler([this](const json& m) { route(m, nullptr, {}); });
    }
    double timeout = 300;  // seconds per request

    // The handshake; returns the server's InitializeResult.
    json initialize(const std::string& name = "mcp_cpp-client", const std::string& version = sdk_version) {
        json r = call("initialize", {{"protocolVersion", protocol_versions().front()},
                                     {"capabilities", json::object()},
                                     {"clientInfo", {{"name", name}, {"version", version}}}});
        info_ = r;
        if (auto* h = dynamic_cast<HttpTransport*>(t_.get())) h->set_protocol_version(r.value("protocolVersion", ""));
        t_->notify({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        return r;
    }
    const json& server_info() const { return info_; }

    json list_tools() { return paged("tools/list", "tools"); }
    json list_resources() { return paged("resources/list", "resources"); }
    json list_resource_templates() { return paged("resources/templates/list", "resourceTemplates"); }
    json list_prompts() { return paged("prompts/list", "prompts"); }

    // CallToolResult ({content, structuredContent?, isError}).
    json call_tool(const std::string& name, const json& arguments = json::object(), ProgressFn progress = {}) {
        json params = {{"name", name}, {"arguments", arguments}};
        std::string token;
        if (progress) {
            token = "p" + std::to_string(++*next_id_);
            params["_meta"] = {{"progressToken", token}};
        }
        return call("tools/call", params, progress, token);
    }
    json read_resource(const std::string& uri) { return call("resources/read", {{"uri", uri}}); }
    json get_prompt(const std::string& name, const json& arguments = json::object()) {
        return call("prompts/get", {{"name", name}, {"arguments", arguments}});
    }
    void set_logging_level(const std::string& level) { call("logging/setLevel", {{"level", level}}); }
    void ping() { call("ping", json::object()); }

    // Generic request; throws mcp::Error on a JSON-RPC error.
    json call(const std::string& method, const json& params, ProgressFn progress = {}, const std::string& token = "") {
        json msg = {{"jsonrpc", "2.0"}, {"id", ++*next_id_}, {"method", method}, {"params", params}};
        json r = t_->request(msg, [&](const json& m) { route(m, progress, token); }, timeout);
        if (r.contains("error"))
            throw Error(r["error"].value("code", INTERNAL_ERROR), r["error"].value("message", std::string("error")),
                        r["error"].value("data", json()));
        return r.value("result", json::object());
    }

private:
    void route(const json& m, const ProgressFn& progress, const std::string& token) {
        std::string method = m.value("method", "");
        const json p = m.value("params", json::object());
        if (method == "notifications/progress" && progress && p.contains("progressToken") &&
            p["progressToken"] == json(token)) {
            std::optional<double> total;
            if (p.contains("total") && p["total"].is_number()) total = p["total"].get<double>();
            progress(p.value("progress", 0.0), total, p.value("message", std::string()));
        } else if (method == "notifications/message" && log_) {
            log_(p.value("level", std::string("info")), p.value("data", json()));
        }
    }

    json paged(const std::string& method, const std::string& key) {
        json all = json::array(), params = json::object();
        for (;;) {
            json r = call(method, params);
            for (auto& x : r.value(key, json::array())) all.push_back(x);
            if (!r.contains("nextCursor") || r["nextCursor"].is_null()) return all;
            params["cursor"] = r["nextCursor"];
        }
    }

    std::unique_ptr<Transport> t_;
    json info_;
    LogFn log_;
    std::shared_ptr<std::atomic<long long>> next_id_ = std::make_shared<std::atomic<long long>>(0);
};

}  // namespace mcp
