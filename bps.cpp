#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <stdexcept>
#include <chrono>
#include <regex>
#include <iomanip>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/utsname.h>
#endif

#define RESET "\033[0m"
#define PINK "\033[1;35m"
#define PURPLE "\033[1;34m"
#define DARK_PINK "\033[0;35m"
#define RED "\033[1;31m"
#define GREEN "\033[1;32m"
#define YELLOW "\033[1;33m"

class Platform {
public:
    static std::string os_name() {
#ifdef _WIN32
        return "Windows";
#else
        struct utsname info;
        if (uname(&info) == 0) return std::string(info.sysname);
        return "Unknown";
#endif
    }

    static std::string machine_type() {
        return os_name();
    }

    static std::string hostname() {
        char name[256];
#ifdef _WIN32
        DWORD size = sizeof(name);
        if (GetComputerNameA(name, &size)) return std::string(name);
#else
        if (gethostname(name, sizeof(name)) == 0) return std::string(name);
#endif
        return "Unknown";
    }

    static bool supports_ansi() {
#ifdef _WIN32
        return std::getenv("TERM") && std::string(std::getenv("TERM")).find("xterm") != std::string::npos;
#else
        return std::getenv("TERM") && std::string(std::getenv("TERM")) != "dumb";
#endif
    }
};

class ValidationError : public std::runtime_error {
public:
    ValidationError(const std::string& msg) : std::runtime_error(msg) {}
};

class PortScanner {
private:
    std::string host;
    int start_port, end_port, timeout_ms, concurrency;
    bool verbose;
    std::vector<int> open_ports;
    std::mutex mutex;
    const int max_concurrency = 500;
    const int max_port_range = 10000;

public:
    PortScanner(const std::string& h, int sp, int ep, int t, int c, bool v)
        : host(h), start_port(sp), end_port(ep), timeout_ms(t), concurrency(c), verbose(v) {
        validate_inputs();
    }

    void scan(std::function<void(int, bool)> result_callback = nullptr) {
        std::queue<int> ports;
        for (int p = start_port; p <= end_port; ++p) ports.push(p);
        std::vector<std::thread> threads;

#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
#endif

        for (int i = 0; i < concurrency; ++i) {
            threads.emplace_back([this, &ports, result_callback]() {
                while (true) {
                    int port;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (ports.empty()) break;
                        port = ports.front();
                        ports.pop();
                    }
                    bool is_open = check_port(port);
                    if (is_open) {
                        std::lock_guard<std::mutex> lock(mutex);
                        open_ports.push_back(port);
                    }
                    if (result_callback) result_callback(port, is_open);
                }
            });
        }

        for (auto& t : threads) t.join();
        std::sort(open_ports.begin(), open_ports.end());

#ifdef _WIN32
        WSACleanup();
#endif
    }

    const std::vector<int>& get_open_ports() const { return open_ports; }

private:
    bool check_port(int port) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return false;

        struct sockaddr_in server;
        server.sin_family = AF_INET;
        server.sin_port = htons(port);
#ifdef _WIN32
        inet_pton(AF_INET, host.c_str(), &server.sin_addr);
#else
        server.sin_addr.s_addr = inet_addr(host.c_str());
#endif

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

        bool is_open = connect(sock, (struct sockaddr*)&server, sizeof(server)) == 0;
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return is_open;
    }

    void validate_inputs() {
        if (start_port > end_port) throw ValidationError("Start port must be <= end port");
        if (start_port < 1 || end_port > 65535) throw ValidationError("Ports must be 1-65535");
        if (end_port - start_port + 1 > max_port_range) throw ValidationError("Port range too large (max " + std::to_string(max_port_range) + ")");
        if (concurrency < 1 || concurrency > max_concurrency) throw ValidationError("Concurrency must be 1-" + std::to_string(max_concurrency));
        if (timeout_ms < 1) throw ValidationError("Timeout must be > 0");

        struct sockaddr_in sa;
        if (inet_pton(AF_INET, host.c_str(), &(sa.sin_addr)) != 1) {
            struct addrinfo hints = {}, *res;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0) {
                throw ValidationError("Invalid host or IP: " + host);
            }
            freeaddrinfo(res);
        }
    }
};

class ScannerCLI {
public:
    static void run_interactive() {
        clear_screen();
        std::cout << PINK << "OS: " << Platform::os_name() << " | Hostname: " << Platform::hostname()
                  << " | Machine: " << Platform::machine_type() << RESET << "\n";
        std::cout << DARK_PINK << "Made by yurz, fuck macos" << RESET << "\n\n";

        std::string host;
        int start_port = 1, end_port = 1024, timeout = 500, concurrency = 20;
        bool verbose = false;

        std::cout << PURPLE << "Enter host/IP: " << RESET;
        std::getline(std::cin, host);
        host = trim(host);
        if (host.empty()) host = "127.0.0.1";

        std::cout << PURPLE << "Enter start port (default 1): " << RESET;
        std::string input;
        std::getline(std::cin, input);
        if (!input.empty()) start_port = std::stoi(input);

        std::cout << PURPLE << "Enter end port (default 1024): " << RESET;
        std::getline(std::cin, input);
        if (!input.empty()) end_port = std::stoi(input);

        std::cout << PURPLE << "Enter timeout (ms, default 500): " << RESET;
        std::getline(std::cin, input);
        if (!input.empty()) timeout = std::stoi(input);

        std::cout << PURPLE << "Enter concurrency (default 20): " << RESET;
        std::getline(std::cin, input);
        if (!input.empty()) concurrency = std::stoi(input);

        std::cout << PURPLE << "Verbose mode? (y/n, default n): " << RESET;
        std::getline(std::cin, input);
        verbose = !input.empty() && (input[0] == 'y' || input[0] == 'Y');

        try {
            std::cout << "\n" << PINK << "Scanning " << host << " ports " << start_port << "-" << end_port << "..." << RESET << "\n";
            PortScanner scanner(host, start_port, end_port, timeout, concurrency, verbose);
            std::vector<int> open_ports;

            scanner.scan([&](int port, bool is_open) {
                if (verbose) {
                    std::cout << port << ": " << (is_open ? GREEN : RED) << (is_open ? "open" : "closed") << RESET << "\n";
                }
                if (is_open) open_ports.push_back(port);
            });

            std::cout << "\n" << PINK << "Scan complete." << RESET << "\n";
            if (open_ports.empty()) {
                std::cout << YELLOW << "No open ports found." << RESET << "\n";
            } else {
                std::cout << PINK << "Open ports:" << RESET << "\n";
                for (int port : open_ports) {
                    std::cout << DARK_PINK << port << RESET << "\n";
                }
            }
        } catch (const ValidationError& e) {
            std::cout << RED << "Error: " << e.what() << RESET << "\n";
            exit(1);
        } catch (const std::exception& e) {
            std::cout << RED << "Error: " << e.what() << RESET << "\n";
            exit(1);
        }
    }

private:
    static void clear_screen() {
#ifdef _WIN32
        system("cls");
#else
        system("clear");
#endif
    }

    static std::string trim(const std::string& str) {
        std::string s = str;
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char c) { return !std::isspace(c); }));
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), s.end());
        return s;
    }
};

struct Args {
    std::string host = "";
    int start_port = 1, end_port = 1024, timeout = 500, concurrency = 20;
    bool verbose = false;
    bool help = false;
};

Args parse_args(int argc, char* argv[]) {
    Args args;
    std::regex opt_regex("--?(\\w+)(?:=(\\S+))?");
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        std::smatch match;
        if (std::regex_match(arg, match, opt_regex)) {
            std::string key = match[1].str();
            std::string value = match[2].str();
            if (key == "h" || key == "host") args.host = value;
            else if (key == "s" || key == "start") args.start_port = std::stoi(value);
            else if (key == "e" || key == "end") args.end_port = std::stoi(value);
            else if (key == "t" || key == "timeout") args.timeout = std::stoi(value);
            else if (key == "c" || key == "concurrency") args.concurrency = std::stoi(value);
            else if (key == "v" || key == "verbose") args.verbose = true;
            else if (key == "help") args.help = true;
        }
    }
    return args;
}

int main(int argc, char* argv[]) {
    Args args = parse_args(argc, argv);

    if (args.help) {
        std::cout << "Usage: bps [options]\n"
                  << "Options:\n"
                  << "  -h, --host=HOST        Target host/IP\n"
                  << "  -s, --start=START      Start port\n"
                  << "  -e, --end=END          End port\n"
                  << "  -t, --timeout=TIMEOUT  Timeout (ms)\n"
                  << "  -c, --concurrency=CONC Concurrency\n"
                  << "  -v, --verbose          Verbose mode\n"
                  << "  --help                 Show help\n"
                  << "Run without args for interactive CLI.\n";
        return 0;
    }

    if (args.host.empty() || argc == 1) {
        ScannerCLI::run_interactive();
    } else {
        std::cout << PINK << "OS: " << Platform::os_name() << " | Hostname: " << Platform::hostname()
                  << " | Machine: " << Platform::machine_type() << RESET << "\n";
        std::cout << DARK_PINK << "Made by yurz, fuck macos" << RESET << "\n\n";

        try {
            std::cout << PINK << "Scanning " << args.host << " ports " << args.start_port << "-" << args.end_port << "..." << RESET << "\n";
            PortScanner scanner(args.host, args.start_port, args.end_port, args.timeout, args.concurrency, args.verbose);
            std::vector<int> open_ports;

            scanner.scan([&](int port, bool is_open) {
                if (args.verbose) {
                    std::cout << port << ": " << (is_open ? GREEN : RED) << (is_open ? "open" : "closed") << RESET << "\n";
                }
                if (is_open) open_ports.push_back(port);
            });

            std::cout << "\n" << PINK << "Scan complete." << RESET << "\n";
            if (open_ports.empty()) {
                std::cout << YELLOW << "No open ports found." << RESET << "\n";
            } else {
                std::cout << PINK << "Open ports:" << RESET << "\n";
                for (int port : open_ports) {
                    std::cout << DARK_PINK << port << RESET << "\n";
                }
            }
        } catch (const ValidationError& e) {
            std::cout << RED << "Error: " << e.what() << RESET << "\n";
            return 1;
        } catch (const std::exception& e) {
            std::cout << RED << "Error: " << e.what() << RESET << "\n";
            return 1;
        }
    }

    return 0;
}
