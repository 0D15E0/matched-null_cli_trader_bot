#pragma once
#include <string>
#include <vector>
#include <utility>

namespace trader {

// Thin libcurl wrapper for simple GET requests. Each call uses its own
// CURL easy handle (curl easy handles are not thread-safe to share), so
// this class is safe to use concurrently from multiple threads/pool
// workers as long as each call creates its own instance or the instance
// is not shared across threads without external synchronization.
class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    // Performs an HTTP GET, returns body as string. Throws std::runtime_error
    // on transport-level failure. Caller should check HTTP status via
    // lastStatusCode() if needed.
    std::string get(const std::string& url);

    // Performs an HTTP POST with the given raw body and extra headers
    // (each "Key: Value" pair). Used for signed private trading API calls
    // (see trading/poloniex_trading_client.h) - never for public data fetch.
    std::string post(const std::string& url, const std::string& body,
                      const std::vector<std::string>& headers = {});

    long lastStatusCode() const { return lastStatus_; }

private:
    void* curl_ = nullptr; // CURL*
    long lastStatus_ = 0;
};

} // namespace trader
