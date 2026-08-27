#pragma once
#include <string>
#include <fstream>
#include <filesystem>
#include <optional>
#include <vector>
#include <nlohmann/json.hpp>

namespace trader {

// Modern replacement for the legacy "trade_log<PAIR>_0" plain-text append
// log (which stored raw exchange JSON responses / free-text lines, only
// human-tailable via `status.sh`). Here each trade is a single JSON line
// (JSONL), so it's both human-tailable (`tail -f trade_log.jsonl`) and
// machine-parseable for win/loss reporting.
struct TradeLogEntry {
    int64_t time = 0;
    std::string symbol;
    std::string side;       // "buy" | "sell"
    double price = 0.0;
    double amount = 0.0;    // base-asset amount
    double quoteValue = 0.0;// price*amount (approx, before fees)
    double fee = 0.0;
    std::optional<double> pnl; // set on sell (closing a position)
    std::string note;
    // Idempotency key LiveTrader sent with the order (empty when none).
    // Logged so a duplicate-looking pair of fills can be checked against the
    // exchange: same clientOrderId means one order reported twice, two
    // different ids mean the bot really did trade twice.
    std::string clientOrderId;

    nlohmann::json toJson() const {
        nlohmann::json j;
        j["time"] = time;
        j["symbol"] = symbol;
        j["side"] = side;
        j["price"] = price;
        j["amount"] = amount;
        j["quote_value"] = quoteValue;
        j["fee"] = fee;
        if (pnl.has_value()) j["pnl"] = *pnl;
        if (!note.empty()) j["note"] = note;
        if (!clientOrderId.empty()) j["client_order_id"] = clientOrderId;
        return j;
    }
};

class TradeLog {
public:
    explicit TradeLog(std::string path) : path_(std::move(path)) {
        std::filesystem::create_directories(std::filesystem::path(path_).parent_path());
    }

    void append(const TradeLogEntry& entry) {
        std::ofstream out(path_, std::ios::app);
        out << entry.toJson().dump() << "\n";
    }

    // Reads the last N lines (trades) from the log, most-recent-last.
    std::vector<nlohmann::json> tail(size_t n) const {
        std::ifstream in(path_);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) lines.push_back(line);
        }
        std::vector<nlohmann::json> out;
        size_t start = lines.size() > n ? lines.size() - n : 0;
        for (size_t i = start; i < lines.size(); ++i) {
            try { out.push_back(nlohmann::json::parse(lines[i])); } catch (...) {}
        }
        return out;
    }

private:
    std::string path_;
};

} // namespace trader
