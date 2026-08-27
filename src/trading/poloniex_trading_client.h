#pragma once
#include "trading_client.h"
#include "../data/http_client.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <map>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace trader {

// ===========================================================================
//  !!! UNVERIFIED AGAINST A REAL ACCOUNT !!!
//
//  Every request below is built from Poloniex's published documentation and
//  from the signing code in their own SDKs (links at the bottom of this
//  comment), but NOT ONE of these calls has ever been executed against a
//  funded Poloniex account by this project. Signature schemes, field names,
//  and rounding rules are exactly the kind of thing that is right in the
//  docs and wrong on the wire. Before letting this trade real money:
//    1. run it against a throwaway account with a few dollars in it,
//    2. confirm a LIMIT/IOC buy fills and that filledQuantity / avgPrice /
//       feeAmount come back where this code looks for them,
//    3. confirm the clientOrderId is echoed back by GET /orders (LiveTrader's
//       crash-recovery guard depends on that and silently degrades to
//       "assume the order is gone" if it is not).
//  Known gap: this client does NOT fetch each symbol's price/quantity
//  precision from the public /markets endpoint and round to it, so an order
//  with too many decimals can be rejected outright by the venue.
// ===========================================================================
//
// What changed and why: the previous implementation targeted Poloniex's
// RETIRED pre-2022 trading API - POST https://poloniex.com/tradingApi with a
// url-encoded `command=buy` body, a `nonce`, HMAC-SHA512, and `Key`/`Sign`
// headers. That endpoint no longer exists, so `--mode live` could not place a
// single order, while the market-data half of this very repo
// (data/poloniex_source.cpp) was already talking to the current API at
// https://api.poloniex.com. This is the rewrite against that same host.
//
// Note on versioning: there is no `/v3` path prefix for Poloniex *spot*. The
// docs split into an unversioned spot API (https://api.poloniex.com/orders,
// /accounts/balances, ...) and a separate v3 futures API. Spot is what this
// project trades, so spot is what this implements.
//
// Authentication (docs + both official SDKs agree):
//   headers: key, signature, signTimestamp        (Content-Type only w/ body)
//   signature = base64( HMAC-SHA256( secret, prehash ) )
//   prehash   = "<METHOD>\n<path>\n<params>"
//   params    = "requestBody=<exact json bytes>&signTimestamp=<ms>"   if a body,
//               otherwise the query params plus signTimestamp, sorted by key
//               ascending and joined with '&' as "k=v".
//   e.g.  GET\n/accounts/balances\nsignTimestamp=1659259836247
//         GET\n/orders\nlimit=5&signTimestamp=1659259836247&symbol=ETH_USDT
//         POST\n/orders\nrequestBody={"price":"1",...}&signTimestamp=1659259836247
// The signed body must be byte-identical to the transmitted body, so the
// serialized JSON string is built once and reused for both.
//
// Unlike the legacy code, API key/secret are read ONLY from environment
// variables (POLONIEX_API_KEY / POLONIEX_API_SECRET) - never hardcoded,
// never obfuscated-and-committed (the legacy simplecrypt.cpp approach is
// deliberately not replicated: obfuscation is not encryption, and baking
// even an "encrypted" key into a repo/binary is not meaningfully safer
// than plaintext). If the env vars are absent, construction throws
// TradingClientError so callers can fall back to paper trading with a
// clear message, per the "API keys provided later" requirement.
//
// Docs consulted (2026-08):
//   https://api-docs.poloniex.com/spot/api/                (auth + overview)
//   https://api-docs.poloniex.com/spot/api/private/order   (POST/GET/DELETE /orders)
//   https://api-docs.poloniex.com/spot/api/private/account (GET /accounts/balances)
//   https://api-docs.poloniex.com/spot/api/private/trade   (GET /orders/{id}/trades, fees)
//   https://api-docs.poloniex.com/spot/error-code          ({code,message} envelope)
//   https://github.com/poloniex/polo-sdk-python            (reference signing code)
//   https://github.com/poloniex/polo-sdk-java              (reference header names)

namespace poloniex_detail {

inline size_t writeCallback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

inline std::string base64(const unsigned char* data, size_t len) {
    std::string out;
    out.resize(4 * ((len + 2) / 3) + 1);
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]), data,
                             static_cast<int>(len));
    if (n < 0) return {};
    out.resize(static_cast<size_t>(n));
    return out;
}

// Poloniex wants a BASE64 HMAC-SHA256, whereas trading/hmac.h only offers
// hex (which is what the retired legacy API used), so the primitive is
// spelled out here rather than bent into that header.
inline std::string hmacSha256Base64(const std::string& key, const std::string& data) {
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int macLen = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(), mac, &macLen);
    return base64(mac, macLen);
}

// encodeURIComponent semantics, which is what Poloniex's Python SDK applies
// to query-parameter values before signing. Their Java SDK signs the values
// raw instead; the two only disagree for values containing characters like
// ',' or ':', and every value this client sends is alphanumeric plus '_',
// where the two encodings are byte-identical. Flagged because a future
// caller passing a comma-separated symbol list would land exactly on that
// unresolved difference.
inline std::string uriEncode(const std::string& s) {
    static const std::string kUnreserved = "-_.!~*'()";
    std::ostringstream os;
    os << std::hex << std::uppercase << std::setfill('0');
    for (unsigned char c : s) {
        if (std::isalnum(c) || kUnreserved.find(static_cast<char>(c)) != std::string::npos) {
            os << static_cast<char>(c);
        } else {
            os << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return os.str();
}

// Plain decimal, never scientific notation. std::to_string would print
// 1e-05 for a small BTC quantity and the venue would reject it, and a
// double dumped straight into JSON has the same problem - Poloniex's own
// examples send prices and quantities as strings, so that is what we send.
inline std::string decimalString(double v, int precision = 10) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(precision) << v;
    std::string s = os.str();
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s.empty() ? "0" : s;
}

// Poloniex returns numbers as JSON strings almost everywhere, but not
// completely consistently, so accept both rather than throwing on one.
inline double asDouble(const nlohmann::json& j, const char* key, double fallback = 0.0) {
    if (!j.contains(key) || j[key].is_null()) return fallback;
    try {
        if (j[key].is_string()) {
            const std::string& s = j[key].get_ref<const std::string&>();
            return s.empty() ? fallback : std::stod(s);
        }
        if (j[key].is_number()) return j[key].get<double>();
    } catch (...) {}
    return fallback;
}

inline std::string asString(const nlohmann::json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return {};
    if (j[key].is_string()) return j[key].get<std::string>();
    return j[key].dump();
}

struct HttpResponse {
    bool transportOk = false;
    long status = 0;
    std::string body;
    std::string error;
};

} // namespace poloniex_detail

// REAL (live) Poloniex spot trading client. See the block comment above -
// in particular the part in capitals.
class PoloniexTradingClient : public TradingClient {
public:
    // `feePct` is the fallback used only when the venue does not tell us what
    // a fill actually cost. The legacy code hardcoded 0.25% "conservative
    // worst-case taker fee" into every OrderResult, which meant live P&L was
    // reported against a number nobody had measured; now the real feeAmount
    // is read from GET /orders/{id}/trades, and the constructor additionally
    // asks GET /feeinfo once for this account's actual taker rate (fees are
    // volume-tiered, so there is no correct constant to hardcode).
    explicit PoloniexTradingClient(double feePct = 0.0015) : feePct_(feePct) {
        const char* key = std::getenv("POLONIEX_API_KEY");
        const char* secret = std::getenv("POLONIEX_API_SECRET");
        if (!key || !secret || std::string(key).empty() || std::string(secret).empty()) {
            throw TradingClientError(
                "Live trading requested but POLONIEX_API_KEY / POLONIEX_API_SECRET "
                "are not set. Export them once you have your API keys, e.g.:\n"
                "  export POLONIEX_API_KEY=...\n  export POLONIEX_API_SECRET=...\n"
                "Falling back to --mode paper meanwhile is recommended.");
        }
        apiKey_ = key;
        apiSecret_ = secret;
        refreshFeeRate();
    }

    std::string name() const override { return "poloniex-live"; }

    // TimeInForce maps onto Poloniex's order type + timeInForce pair:
    //   ImmediateOrCancel -> type=LIMIT,        timeInForce=IOC
    //   FillOrKill        -> type=LIMIT,        timeInForce=FOK
    //   GoodTillCancelled -> type=LIMIT,        timeInForce=GTC
    //   PostOnly          -> type=LIMIT_MAKER   (maker-only; timeInForce not sent)
    OrderResult buy(const std::string& symbol, double price, double amountBase,
                     TimeInForce tif = TimeInForce::ImmediateOrCancel,
                     const std::string& clientOrderId = "") override {
        return order("BUY", symbol, price, amountBase, tif, clientOrderId);
    }

    OrderResult sell(const std::string& symbol, double price, double amountBase,
                      TimeInForce tif = TimeInForce::ImmediateOrCancel,
                      const std::string& clientOrderId = "") override {
        return order("SELL", symbol, price, amountBase, tif, clientOrderId);
    }

    // DELETE /orders/{id}. The response echoes {"orderId","clientOrderId",
    // "state":"PENDING_CANCEL","code":200,"message":""} - note the key is
    // `orderId` here but `id` on create, which is exactly the sort of detail
    // the capitalised warning above is about.
    bool cancelOrder(const std::string& symbol, const std::string& orderId) override {
        (void)symbol; // Poloniex cancels by order id alone
        auto resp = request("DELETE", "/orders/" + orderId, {}, "");
        auto j = parse(resp);
        if (j.is_discarded()) return false;
        if (j.contains("code") && j["code"].is_number() && j["code"].get<int>() != 200) return false;
        return resp.status >= 200 && resp.status < 300;
    }

    // GET /orders?symbol=... returns only live orders (state NEW or
    // PARTIALLY_FILLED); filled/cancelled history lives at /orders/history.
    std::vector<OpenOrder> openOrders(const std::string& symbol) override {
        auto resp = request("GET", "/orders", {{"symbol", symbol}, {"limit", "500"}}, "");
        auto j = parse(resp);
        std::vector<OpenOrder> result;
        if (j.is_discarded() || !j.is_array()) return result;
        for (const auto& o : j) {
            if (!o.is_object()) continue;
            OpenOrder oo;
            oo.orderId = poloniex_detail::asString(o, "id");
            oo.clientOrderId = poloniex_detail::asString(o, "clientOrderId");
            oo.symbol = poloniex_detail::asString(o, "symbol");
            std::string side = poloniex_detail::asString(o, "side");
            std::transform(side.begin(), side.end(), side.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            oo.side = side;
            oo.price = poloniex_detail::asDouble(o, "price");
            oo.amount = poloniex_detail::asDouble(o, "quantity");
            oo.filledAmount = poloniex_detail::asDouble(o, "filledQuantity");
            oo.createdAt = static_cast<int64_t>(poloniex_detail::asDouble(o, "createTime") / 1000.0);
            result.push_back(oo);
        }
        return result;
    }

    // GET /accounts/balances returns one entry per account (SPOT, FUTURES,
    // ...), each holding a nested `balances` array of {currency, available,
    // hold}. We sum `available` across accounts for the symbol's two legs.
    //
    // `hold` is deliberately excluded: it is the part locked inside resting
    // orders, so it is owned but not spendable, and sizing a buy against it
    // would produce an order the venue rejects for insufficient funds.
    // LiveTrader's pending-order guard is what covers the "my money is
    // sitting in a live order" case - it returns before trading at all.
    AccountBalances balances(const std::string& symbol) override {
        AccountBalances out;
        auto resp = request("GET", "/accounts/balances", {{"accountType", "SPOT"}}, "");
        auto j = parse(resp);
        if (j.is_discarded() || !j.is_array()) {
            out.ok = false;
            out.raw = resp.transportOk ? resp.body : ("balances request failed: " + resp.error);
            return out;
        }
        auto [baseCcy, quoteCcy] = splitSymbol(symbol);
        for (const auto& account : j) {
            if (!account.is_object() || !account.contains("balances")) continue;
            if (!account["balances"].is_array()) continue;
            for (const auto& b : account["balances"]) {
                std::string ccy = poloniex_detail::asString(b, "currency");
                double available = poloniex_detail::asDouble(b, "available");
                if (ccy == baseCcy) out.base += available;
                else if (ccy == quoteCcy) out.quote += available;
            }
        }
        out.ok = true;
        out.raw = resp.body;
        return out;
    }

private:
    static constexpr const char* kBaseUrl = "https://api.poloniex.com";

    static int64_t nowMillis() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    static std::string tifString(TimeInForce tif) {
        switch (tif) {
            case TimeInForce::ImmediateOrCancel: return "IOC";
            case TimeInForce::FillOrKill:        return "FOK";
            case TimeInForce::GoodTillCancelled: return "GTC";
            case TimeInForce::PostOnly:          return "GTC"; // carried by LIMIT_MAKER instead
        }
        return "GTC";
    }

    // Signs and performs one private request. Query params are used for
    // GET/DELETE, a pre-serialized JSON string for POST; Poloniex's own SDKs
    // never combine the two on one call, and neither do we.
    poloniex_detail::HttpResponse request(
            const std::string& method, const std::string& path,
            std::vector<std::pair<std::string, std::string>> params,
            const std::string& body) const {
        using namespace poloniex_detail;
        const std::string ts = std::to_string(nowMillis());

        std::string signedParams;
        if (body.empty()) {
            auto all = params;
            all.emplace_back("signTimestamp", ts);
            std::sort(all.begin(), all.end(),
                       [](const auto& a, const auto& b) { return a.first < b.first; });
            for (size_t i = 0; i < all.size(); ++i) {
                if (i) signedParams += "&";
                signedParams += all[i].first + "=" + uriEncode(all[i].second);
            }
        } else {
            signedParams = "requestBody=" + body + "&signTimestamp=" + ts;
        }
        const std::string prehash = method + "\n" + path + "\n" + signedParams;
        const std::string signature = hmacSha256Base64(apiSecret_, prehash);

        std::string url = std::string(kBaseUrl) + path;
        if (body.empty() && !params.empty()) {
            url += "?";
            for (size_t i = 0; i < params.size(); ++i) {
                if (i) url += "&";
                url += params[i].first + "=" + uriEncode(params[i].second);
            }
        }

        HttpResponse resp;
        CURL* curl = curl_easy_init();
        if (!curl) {
            resp.error = "curl_easy_init failed";
            return resp;
        }
        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("key: " + apiKey_).c_str());
        headers = curl_slist_append(headers, ("signature: " + signature).c_str());
        headers = curl_slist_append(headers, ("signTimestamp: " + ts).c_str());
        if (!body.empty()) headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "cli_trader/1.0");
        if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        } else if (method != "GET") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        }

        CURLcode rc = curl_easy_perform(curl);
        resp.transportOk = (rc == CURLE_OK);
        if (!resp.transportOk) resp.error = curl_easy_strerror(rc);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return resp;
    }

    // Parsed body, or a discarded json when the transport failed / the venue
    // answered with its {"code":n,"message":"..."} error envelope.
    static nlohmann::json parse(const poloniex_detail::HttpResponse& resp) {
        if (!resp.transportOk) return nlohmann::json(nlohmann::json::value_t::discarded);
        auto j = nlohmann::json::parse(resp.body, nullptr, false);
        if (j.is_discarded()) return j;
        if (j.is_object() && j.contains("code") && j["code"].is_number() &&
            j["code"].get<int>() != 200) {
            return nlohmann::json(nlohmann::json::value_t::discarded);
        }
        if (resp.status >= 400) return nlohmann::json(nlohmann::json::value_t::discarded);
        return j;
    }

    // Best effort: this account's real taker rate, which is volume-tiered and
    // therefore has no correct hardcoded value. A failure here is not fatal -
    // the constructor argument stays in force and we say so.
    void refreshFeeRate() {
        auto resp = request("GET", "/feeinfo", {}, "");
        auto j = parse(resp);
        if (j.is_discarded() || !j.is_object()) {
            std::cerr << "[poloniex] could not read GET /feeinfo; falling back to the configured "
                         "fee estimate of " << feePct_ * 100.0 << "%\n";
            return;
        }
        double taker = poloniex_detail::asDouble(j, "takerRate", -1.0);
        if (taker >= 0.0) {
            feePct_ = taker;
            std::cerr << "[poloniex] account taker fee rate from /feeinfo: " << taker * 100.0
                      << "%\n";
        }
    }

    // Per-symbol trade limits, fetched once from the public GET /markets/{s}
    // and cached. The venue rejects an order whose price or quantity carries
    // more decimals than the symbol's scale ({"code":21335,"Price scale
    // error"}) - discovered by this client's first-ever real order, a $2
    // verification trade, which means every order the live loop would have
    // placed was going to be rejected the same way. Values are floored, never
    // rounded: rounding a quantity up can exceed the balance, and flooring a
    // price makes a sell strictly more aggressive, which an IOC absorbs.
    struct SymbolLimits {
        bool ok = false;
        int priceScale = 10;
        int quantityScale = 10;
        double minQuantity = 0.0;  // base units
        double minAmount = 0.0;    // quote value
    };

    SymbolLimits symbolLimits(const std::string& symbol) {
        auto it = limitsCache_.find(symbol);
        if (it != limitsCache_.end()) return it->second;
        SymbolLimits lim;
        try {
            HttpClient http;
            auto body = http.get(std::string(kBaseUrl) + "/markets/" + symbol);
            auto j = nlohmann::json::parse(body, nullptr, false);
            if (j.is_array() && !j.empty()) j = j.front();
            if (j.is_object() && j.contains("symbolTradeLimit") &&
                j["symbolTradeLimit"].is_object()) {
                const auto& t = j["symbolTradeLimit"];
                lim.priceScale = static_cast<int>(poloniex_detail::asDouble(t, "priceScale", 10));
                lim.quantityScale =
                    static_cast<int>(poloniex_detail::asDouble(t, "quantityScale", 10));
                lim.minQuantity = poloniex_detail::asDouble(t, "minQuantity");
                lim.minAmount = poloniex_detail::asDouble(t, "minAmount");
                lim.ok = true;
            }
        } catch (...) {
            // Transport failure: keep the permissive defaults and let the
            // venue be the judge, exactly as before this cache existed.
        }
        if (!lim.ok)
            std::cerr << "[poloniex] could not fetch trade limits for " << symbol
                      << "; sending the order unrounded\n";
        limitsCache_[symbol] = lim;
        return lim;
    }

    static double floorToScale(double v, int scale) {
        double p = std::pow(10.0, std::max(0, std::min(12, scale)));
        return std::floor(v * p + 1e-9) / p;
    }

    OrderResult order(const std::string& side, const std::string& symbol, double price,
                       double amountBase, TimeInForce tif, const std::string& clientOrderId) {
        using namespace poloniex_detail;

        SymbolLimits lim = symbolLimits(symbol);
        if (lim.ok) {
            price = floorToScale(price, lim.priceScale);
            amountBase = floorToScale(amountBase, lim.quantityScale);
            if (amountBase < lim.minQuantity || amountBase * price < lim.minAmount) {
                OrderResult r;
                r.ok = false;
                r.status = OrderStatus::Rejected;
                r.raw = "below venue minimum for " + symbol + ": quantity " +
                        decimalString(amountBase) + " (min " + decimalString(lim.minQuantity) +
                        "), value " + decimalString(amountBase * price, 2) + " (min " +
                        decimalString(lim.minAmount, 2) + "). Not sent.";
                return r;
            }
        }

        nlohmann::json req;
        req["symbol"] = symbol;
        req["side"] = side;
        req["type"] = (tif == TimeInForce::PostOnly) ? "LIMIT_MAKER" : "LIMIT";
        req["accountType"] = "SPOT";
        req["price"] = decimalString(price, lim.ok ? lim.priceScale : 10);
        req["quantity"] = decimalString(amountBase, lim.ok ? lim.quantityScale : 10);
        if (tif != TimeInForce::PostOnly) req["timeInForce"] = tifString(tif);
        if (!clientOrderId.empty()) req["clientOrderId"] = clientOrderId;

        // Serialize once: the signature covers these exact bytes, so the
        // signed copy and the transmitted copy must not be two dumps.
        const std::string body = req.dump();
        auto resp = request("POST", "/orders", {}, body);

        OrderResult r;
        r.raw = resp.transportOk ? resp.body : ("transport failure: " + resp.error);
        auto j = parse(resp);
        if (j.is_discarded() || !j.is_object()) {
            r.ok = false;
            r.status = OrderStatus::Rejected;
            return r;
        }
        r.orderId = asString(j, "id");
        if (r.orderId.empty()) {
            r.ok = false;
            r.status = OrderStatus::Rejected;
            return r;
        }

        // The create response carries only {id, clientOrderId} - no fill
        // information at all - so the actual outcome has to be read back.
        // An IOC/FOK is terminal within milliseconds, but "within
        // milliseconds" is not "before the POST returns", hence the retries.
        nlohmann::json detail;
        for (int attempt = 0; attempt < 4; ++attempt) {
            auto detailResp = request("GET", "/orders/" + r.orderId, {}, "");
            detail = parse(detailResp);
            if (!detail.is_discarded() && detail.is_object()) {
                std::string state = asString(detail, "state");
                bool terminal = state == "FILLED" || state == "CANCELED" ||
                                state == "PARTIALLY_CANCELED" || state == "REJECTED" ||
                                state == "EXPIRED" || state == "FAILED";
                bool resting = tif == TimeInForce::GoodTillCancelled ||
                                tif == TimeInForce::PostOnly;
                if (terminal || resting) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        if (detail.is_discarded() || !detail.is_object()) {
            // The order exists (we have an id) but we cannot see what it did.
            // Reporting a fill we did not observe would be worse than
            // reporting nothing: LiveTrader keeps its pending marker on a
            // Rejected result only if we say ok=false, and the next cycle's
            // balance reconciliation is what recovers the truth.
            r.ok = false;
            r.status = OrderStatus::Rejected;
            r.raw += " | order placed but GET /orders/" + r.orderId + " failed; "
                     "balance reconciliation will resolve it";
            return r;
        }

        const std::string state = asString(detail, "state");
        r.filledAmount = asDouble(detail, "filledQuantity");
        double filledQuote = asDouble(detail, "filledAmount"); // quote units filled
        double avgPrice = asDouble(detail, "avgPrice");
        r.filledPrice = avgPrice > 0.0
                            ? avgPrice
                            : (r.filledAmount > 0.0 ? filledQuote / r.filledAmount : price);
        r.fee = feeForOrder(r.orderId, symbol, filledQuote, r.filledPrice, r.raw);
        r.raw += " | " + detail.dump();

        if (state == "REJECTED" || state == "FAILED") {
            r.ok = false;
            r.status = OrderStatus::Rejected;
            return r;
        }
        if (state == "NEW" || state == "PENDING_NEW") {
            r.ok = true;
            r.status = OrderStatus::Open;
            return r;
        }
        if (state == "PARTIALLY_FILLED") {
            // Still live at the venue with part of it done.
            r.ok = true;
            r.status = OrderStatus::PartiallyFilled;
            return r;
        }
        if (state == "FILLED") {
            r.ok = true;
            r.status = OrderStatus::Filled;
            return r;
        }
        // CANCELED / PARTIALLY_CANCELED / EXPIRED: terminal. An IOC that
        // crossed part of the book lands here with a nonzero fill, which is
        // a successful (partial) trade, not a failure.
        r.ok = r.filledAmount > 0.0;
        r.status = r.ok ? OrderStatus::PartiallyFilled : OrderStatus::Cancelled;
        return r;
    }

    // The order object carries no fee, only the trades do (GET
    // /orders/{id}/trades -> [{feeAmount, feeCurrency, ...}]). Fees are
    // charged in whichever currency Poloniex chose, so a fee billed in the
    // base asset is converted at the fill price to keep OrderResult::fee in
    // quote currency as documented. If the fee is billed in something else
    // entirely (the TRX discount programme does this) we cannot convert it
    // without another price lookup, so we fall back to the rate estimate and
    // say so in `raw` rather than silently reporting a wrong number.
    double feeForOrder(const std::string& orderId, const std::string& symbol,
                        double filledQuote, double fillPrice, std::string& raw) {
        using namespace poloniex_detail;
        if (filledQuote <= 0.0) return 0.0;

        auto resp = request("GET", "/orders/" + orderId + "/trades", {}, "");
        auto j = parse(resp);
        if (j.is_discarded() || !j.is_array() || j.empty()) {
            raw += " | fee estimated at " + decimalString(feePct_ * 100.0, 4) +
                   "% (no trade detail available)";
            return filledQuote * feePct_;
        }
        auto [baseCcy, quoteCcy] = splitSymbol(symbol);
        double fee = 0.0;
        bool unconvertible = false;
        for (const auto& t : j) {
            double amount = asDouble(t, "feeAmount");
            if (amount <= 0.0) continue;
            std::string ccy = asString(t, "feeCurrency");
            if (ccy == quoteCcy) fee += amount;
            else if (ccy == baseCcy && fillPrice > 0.0) fee += amount * fillPrice;
            else unconvertible = true;
        }
        if (unconvertible) {
            raw += " | part of the fee was billed in a third currency and could not be "
                   "converted to quote; falling back to the rate estimate";
            return filledQuote * feePct_;
        }
        return fee;
    }

    std::map<std::string, SymbolLimits> limitsCache_;
    std::string apiKey_;
    std::string apiSecret_;
    double feePct_;
};

} // namespace trader
