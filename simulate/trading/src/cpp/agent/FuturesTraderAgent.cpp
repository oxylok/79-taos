/*
 * SPDX-FileCopyrightText: 2025 Rayleigh Research <to@rayleigh.re>
 * SPDX-License-Identifier: MIT
 */
#include "FuturesTraderAgent.hpp"

#include "taosim/message/ExchangeAgentMessagePayloads.hpp"
#include "taosim/message/MessagePayload.hpp"
#include "DistributionFactory.hpp"
#include "RayleighDistribution.hpp"
#include "Simulation.hpp"

#include <boost/algorithm/string/regex.hpp>
#include <boost/bimap.hpp>

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/random.hpp>

#include <algorithm>

//-------------------------------------------------------------------------

namespace br = boost::random;

//-------------------------------------------------------------------------

FuturesTraderAgent::FuturesTraderAgent(Simulation* simulation) noexcept
    : Agent{simulation}
{}

//-------------------------------------------------------------------------

void FuturesTraderAgent::configure(const pugi::xml_node& node)
{
    Agent::configure(node);

    m_rng = &simulation()->rng();

    pugi::xml_attribute attr;
    static constexpr auto ctx = std::source_location::current().function_name();

    if (attr = node.attribute("exchange"); attr.empty()) {
        throw std::invalid_argument(fmt::format(
            "{}: missing required attribute 'exchange'", ctx));
    }
    m_exchange = attr.as_string();

    if (simulation()->exchange() == nullptr) {
        throw std::runtime_error(fmt::format(
            "{}: exchange must be configured a priori", ctx));
    }
    m_bookCount = simulation()->exchange()->books().size();

    attr = node.attribute("sigmaN");
    m_sigmaN = ( attr.empty() || attr.as_double() < 0.0f) ? 0.7 : attr.as_double();

    if (attr = node.attribute("sigmaEps"); attr.empty() || attr.as_double() <= 0.0f) {
        throw std::invalid_argument(fmt::format(
            "{}: attribute 'sigmaEps' should have a value greater than 0.0f", ctx));
    }
    m_sigmaEps = attr.as_double();

    if (attr = node.attribute("minOPLatency"); attr.as_ullong() == 0) {
        throw std::invalid_argument(fmt::format(
            "{}: attribute 'minLatency' should have a value greater than 0", ctx));
    }
    m_opl.min = attr.as_ullong();
    if (attr = node.attribute("maxOPLatency"); attr.as_ullong() == 0) {
        throw std::invalid_argument(fmt::format(
            "{}: attribute 'maxLatency' should have a value greater than 0", ctx));
    }
    m_opl.max = attr.as_ullong();
    if (m_opl.min >= m_opl.max) {
        throw std::invalid_argument(fmt::format(
            "{}: 'minOPLatency' ({}) should be strictly less 'maxOPLatency' ({})",
            ctx, m_opl.min, m_opl.max));
    }

    attr = node.attribute("volume");
    m_volume = (attr.empty() || attr.as_double() <= 0.0) ? 1.0 : attr.as_double();
    m_volumeFactor = std::vector<float>(m_bookCount,1.0);
    m_factorCounter = std::vector<uint32_t>(m_bookCount, 0);
    attr = node.attribute("lambda");
    m_lambda = (attr.empty() || attr.as_float() <= 0.0) ? 0.01155 : attr.as_float();

    attr = node.attribute("tau");
    m_tau = (attr.empty() || attr.as_ullong() == 0) ? 120'000'000'000 : attr.as_ullong();

    
    attr = node.attribute("orderTypeProb");
    m_orderTypeProb = (attr.empty() || attr.as_float() <= 0.0f) ? 0.5f : attr.as_float();

    m_lastUpdate = std::vector<Timestamp>(m_bookCount,0);
    
    m_orderFlag = std::vector<bool>(m_bookCount, false);

    m_historySize = node.attribute("tauHist").as_ullong(200);

    for (BookId bookId = 0; bookId < m_bookCount; ++bookId) {
        m_priceHist.push_back([&] {
            decltype(m_priceHist)::value_type hist{m_historySize};
            for (uint32_t i = 0; i < m_historySize; ++i) {
                hist.push_back(0.0);
            }
            return hist;
        }());
        m_logReturns.push_back([&] {
            decltype(m_logReturns)::value_type logReturns{m_historySize};
            for (uint32_t i = 0; i < m_historySize; ++i) {
                logReturns.push_back(0.0);
            }
            return logReturns;
        }());
    }

    m_priceIncrement = 1 / std::pow(10, simulation()->exchange()->config().parameters().priceIncrementDecimals);
    m_volumeIncrement = 1 / std::pow(10, simulation()->exchange()->config().parameters().volumeIncrementDecimals);

    m_debug = node.attribute("debug").as_bool();

    m_marketFeedLatencyDistribution = std::normal_distribution<double>{
        [&] {
            static constexpr const char* name = "MFLmean";
            if (auto attr = node.attribute(name); attr.empty()) {
                throw std::invalid_argument{fmt::format(
                    "{}: Missing attribute '{}'", ctx, name)};
            } else {
                return attr.as_double();
            }
        }(),
        [&] {
            static constexpr const char* name = "MFLstd";
            if (auto attr = node.attribute(name); attr.empty()) {
                throw std::invalid_argument{fmt::format(
                    "{}: Missing attribute '{}'", ctx, name)};
            } else {
                return attr.as_double();
            }
        }()
    };

    m_tradePrice.resize(m_bookCount);
    
    attr = node.attribute("opLatencyScaleRay"); 
    const double scale = (attr.empty() || attr.as_double() == 0.0) ? 0.235 : attr.as_double();
    const double percentile = 1-std::exp(-1/(2*scale*scale));
    m_orderPlacementLatencyDistribution =  std::make_unique<taosim::stats::RayleighDistribution>(scale, percentile); 
    

    // m_rayleigh = std::make_unique<taosim::stats::RayleighDistribution>(
    //     [&] {
    //         static constexpr const char* name = "scaleR";
    //         if (auto sigma = node.attribute(name).as_double(); !(sigma >= 0)) {
    //             throw std::invalid_argument{fmt::format(
    //                 "{}: Attribute '{}' should be >= 0, was {}", ctx, name, sigma)};
    //         } else {
    //             return sigma;
    //         }
    //     }()
    // );


    m_baseName = [&] {
        std::string res = name();
        boost::algorithm::erase_regex(res, boost::regex("(_\\d+)$"));
        return res;
    }();
}

//-------------------------------------------------------------------------

void FuturesTraderAgent::receiveMessage(Message::Ptr msg)
{

    if (msg->type == "EVENT_SIMULATION_START") {
        handleSimulationStart();
    }
    else if (msg->type == "EVENT_SIMULATION_END") {
        handleSimulationStop();
    }
    else if (msg->type == "RESPONSE_SUBSCRIBE_EVENT_TRADE") {
        handleTradeSubscriptionResponse();
    }
    else if (msg->type == "RESPONSE_RETRIEVE_L1") {
        handleRetrieveL1Response(msg);
    }
    else if (msg->type == "RESPONSE_PLACE_ORDER_MARKET") {
        handleMarketOrderPlacementResponse(msg);
    }
    else if (msg->type == "ERROR_RESPONSE_PLACE_ORDER_MARKET") {
        handleMarketOrderPlacementErrorResponse(msg);
    }
    else if (msg->type == "RESPONSE_PLACE_ORDER_LIMIT") {
        handleLimitOrderPlacementResponse(msg);
    }
    else if (msg->type == "ERROR_RESPONSE_PLACE_ORDER_LIMIT") {
        handleLimitOrderPlacementErrorResponse(msg);
    }
    else if (msg->type == "EVENT_TRADE") {
        handleTrade(msg);
    }
}

//-------------------------------------------------------------------------

void FuturesTraderAgent::handleSimulationStart()
{
    simulation()->dispatchMessage(
        simulation()->currentTimestamp(),
        1,
        name(),
        m_exchange,
        "SUBSCRIBE_EVENT_TRADE");
}

//-------------------------------------------------------------------------

void FuturesTraderAgent::handleSimulationStop()
{}

//-------------------------------------------------------------------------

void FuturesTraderAgent::handleTradeSubscriptionResponse()
{
    for (BookId bookId = 0; bookId < m_bookCount; ++bookId) {
        auto rng = std::mt19937{simulation()->currentTimestamp() + bookId};
        simulation()->dispatchMessage(
            simulation()->currentTimestamp(),
            static_cast<Timestamp>(std::min(
            std::abs(m_marketFeedLatencyDistribution(rng)),
            m_marketFeedLatencyDistribution.mean()
            + 3.0 * (m_marketFeedLatencyDistribution.stddev()))),
            name(),
            m_exchange,
            "RETRIEVE_L1",
            MessagePayload::create<RetrieveL1Payload>(bookId));
    }
}

//-------------------------------------------------------------------------

void FuturesTraderAgent::handleRetrieveL1Response(Message::Ptr msg)
{
    const auto payload = std::dynamic_pointer_cast<RetrieveL1ResponsePayload>(msg->payload);

    const BookId bookId = payload->bookId;

    auto rng = std::mt19937{simulation()->currentTimestamp() + bookId};
    simulation()->dispatchMessage(
        simulation()->currentTimestamp(),
        static_cast<Timestamp>(std::min(
            std::abs(m_marketFeedLatencyDistribution(rng)),
            m_marketFeedLatencyDistribution.mean()
            + 3.0 * (m_marketFeedLatencyDistribution.stddev()))),
        name(),
        m_exchange,
        "RETRIEVE_L1",
        MessagePayload::create<RetrieveL1Payload>(bookId));

    const uint64_t processCount = getProcessCount(bookId, "external");
    if (processCount == 0 && m_priceHist.at(bookId).back() < 0.0001) {
        const double newProcessValue = getProcessValue(bookId, "external");
        m_priceHist.at(bookId).push_back(newProcessValue);  
        return;
    }
    if (m_lastUpdate.at(bookId) != processCount) {
        const double newProcessValue = getProcessValue(bookId, "external"); 
        m_lastUpdate.at(bookId) = processCount;

        const double lastProcessValue = m_priceHist.at(bookId).back();
        if (lastProcessValue == 0) {
            m_logReturns.at(bookId).push_back(0.0);
        } else {
            m_logReturns.at(bookId).push_back(std::log(newProcessValue / lastProcessValue));
        }
        m_volumeFactor.at(bookId) = std::min(2.0,std::exp(std::abs(m_logReturns.at(bookId).back())));
        m_factorCounter.at(bookId) = 0;
        m_priceHist.at(bookId).push_back(newProcessValue);
    } else {
        m_factorCounter.at(bookId) += 1;
        m_volumeFactor.at(bookId) = m_volumeFactor.at(bookId)*std::exp(-m_lambda*m_factorCounter.at(bookId));
    }

    if (m_orderFlag.at(bookId)) return;
    

    const uint32_t numActingAgents = 1; 

    const auto& agentBaseNamesToCounts =
        simulation()->localAgentManager()->roster()->baseNamesToCounts();

    const auto categoryIdToAgentType = [&] {
        boost::bimap<uint32_t, std::string> res;
        auto filteredBaseNames = agentBaseNamesToCounts
            | views::keys
            | views::filter([](const auto& baseName) { return baseName.contains("FUTURES_TRADER_AGENT"); });
        for (const auto& [id, baseName] : views::enumerate(filteredBaseNames)) {
            res.insert({static_cast<uint32_t>(id), baseName});
        }
        return res;
    }();

    auto multinomial = [&] {
        const auto weights = agentBaseNamesToCounts
            | views::filter([](const auto& kv) { return kv.first.contains("FUTURES_TRADER_AGENT"); })
            | views::values
            | ranges::to<std::vector>;
        return std::discrete_distribution{weights.begin(), weights.end()};
    }();

    const auto categoryIdDraws =
        views::iota(0u, numActingAgents)
        | views::transform([&](auto) { return multinomial(rng); });

    const auto actorIdsNonCanon =
        categoryIdDraws
        | views::transform([&](auto draw) {
            return std::uniform_int_distribution<uint32_t>{
                0, agentBaseNamesToCounts.at(categoryIdToAgentType.left.at(draw)) - 1}(rng);
        });

    for (auto [categoryId, actorId] : views::zip(categoryIdDraws, actorIdsNonCanon)) {
        if (categoryIdToAgentType.right.at(m_baseName) != categoryId) continue;
        if (!name().ends_with(fmt::format("_{}", actorId))) continue;
        const double bestBid = taosim::util::decimal2double(payload->bestBidPrice);
        const double bestAsk = taosim::util::decimal2double(payload->bestAskPrice);
        placeOrder(bookId, bestAsk, bestBid);
    }    
}

//-------------------------------------------------------------------------

void FuturesTraderAgent::handleMarketOrderPlacementResponse(Message::Ptr msg)
{
    const auto payload = std::dynamic_pointer_cast<PlaceOrderMarketResponsePayload>(msg->payload);
    m_orderFlag.at(payload->requestPayload->bookId) = false;
}

//-------------------------------------------------------------------------

void FuturesTraderAgent::handleMarketOrderPlacementErrorResponse(Message::Ptr msg)
{
    const auto payload =
        std::dynamic_pointer_cast<PlaceOrderMarketErrorResponsePayload>(msg->payload);

    const BookId bookId = payload->requestPayload->bookId;

    m_orderFlag.at(bookId) = false;
}

//-------------------------------------------------------------------------

void FuturesTraderAgent::handleLimitOrderPlacementResponse(Message::Ptr msg)
{
    const auto payload = std::dynamic_pointer_cast<PlaceOrderLimitResponsePayload>(msg->payload);

    simulation()->dispatchMessage(
        simulation()->currentTimestamp(),
        m_tau,
        name(),
        m_exchange,
        "CANCEL_ORDERS",
        MessagePayload::create<CancelOrdersPayload>(
            std::vector{taosim::event::Cancellation(payload->id)}, payload->requestPayload->bookId));

    m_orderFlag.at(payload->requestPayload->bookId) = false;
}

//-------------------------------------------------------------------------

void FuturesTraderAgent::handleLimitOrderPlacementErrorResponse(Message::Ptr msg)
{
    const auto payload =
        std::dynamic_pointer_cast<PlaceOrderLimitErrorResponsePayload>(msg->payload);

    const BookId bookId = payload->requestPayload->bookId;

    m_orderFlag.at(bookId) = false;
}

//-------------------------------------------------------------------------

void FuturesTraderAgent::handleCancelOrdersResponse(Message::Ptr msg)
{}

//-------------------------------------------------------------------------

void FuturesTraderAgent::handleCancelOrdersErrorResponse(Message::Ptr msg)
{}

//-------------------------------------------------------------------------

void FuturesTraderAgent::handleTrade(Message::Ptr msg)
{
    const auto payload = std::dynamic_pointer_cast<EventTradePayload>(msg->payload);
    const double tradePrice = taosim::util::decimal2double(payload->trade.price());
    m_tradePrice.at(payload->bookId) = {
        .timestamp = msg->arrival,
        .price = tradePrice
    };
}


//-------------------------------------------------------------------------

void FuturesTraderAgent::placeOrder(BookId bookId, double bestAsk, double bestBid)
{
    const double logReturn = m_logReturns.at(bookId).back();
    if (logReturn == 0) return;
    const double sign = logReturn < 0.0 ? -1.0 : 1.0;
    const double epsilon = std::normal_distribution{0.0, m_sigmaEps}(*m_rng);
    const double forecast =  sign + epsilon;
    const float newMean = std::log(m_volume) * m_volumeFactor.at(bookId);
    std::lognormal_distribution<> lognormalDist(newMean, 1); 
    double volume = lognormalDist(*m_rng);
    volume =  std::floor(volume / m_volumeIncrement) * m_volumeIncrement;
    const double priceShift = std::round((newMean - volume) / 1) * m_priceIncrement;
    if (volume == 0) return;
    const bool draw = std::bernoulli_distribution{m_orderTypeProb}(*m_rng);
    if (forecast > 0) {
        if (draw) {
            placeBuy(bookId, volume);
        } else {
            placeBid(bookId, volume, bestBid + priceShift);
        }
    }
    else if (forecast < 0) {
        if (draw) {
            placeSell(bookId, volume);
        } else {
            placeAsk(bookId, volume, bestAsk - priceShift);
        }
    }
}
//-------------------------------------------------------------------------

void FuturesTraderAgent::placeBid(BookId bookId, double volume, double price)
{
    m_orderFlag.at(bookId) = true;

    simulation()->dispatchMessage(
        simulation()->currentTimestamp(),
        orderPlacementLatency(),
        name(),
        m_exchange,
        "PLACE_ORDER_LIMIT",
        MessagePayload::create<PlaceOrderLimitPayload>(
            OrderDirection::BUY,
            taosim::util::double2decimal(volume),
            taosim::util::double2decimal(price),
            bookId));
}
//-------------------------------------------------------------------------

void FuturesTraderAgent::placeBuy(BookId bookId, double volume)
{
    m_orderFlag.at(bookId) = true;

    simulation()->dispatchMessage(
        simulation()->currentTimestamp(),
        orderPlacementLatency(),
        name(),
        m_exchange,
        "PLACE_ORDER_MARKET",
        MessagePayload::create<PlaceOrderMarketPayload>(
            OrderDirection::BUY,
            taosim::util::double2decimal(volume),
            bookId));
}

//-------------------------------------------------------------------------

void FuturesTraderAgent::placeAsk(BookId bookId, double volume, double price)
{
    m_orderFlag.at(bookId) = true;
    simulation()->dispatchMessage(
        simulation()->currentTimestamp(),
        orderPlacementLatency(),
        name(),
        m_exchange,
        "PLACE_ORDER_LIMIT",
        MessagePayload::create<PlaceOrderLimitPayload>(
            OrderDirection::SELL,
            taosim::util::double2decimal(volume),
            taosim::util::double2decimal(price),
            bookId));
}

//-------------------------------------------------------------------------

void FuturesTraderAgent::placeSell(BookId bookId, double volume)
{
    m_orderFlag.at(bookId) = true;
    simulation()->dispatchMessage(
        simulation()->currentTimestamp(),
        orderPlacementLatency(),
        name(),
        m_exchange,
        "PLACE_ORDER_MARKET",
        MessagePayload::create<PlaceOrderMarketPayload>(
            OrderDirection::SELL,
            taosim::util::double2decimal(volume),
            bookId));
}

//-------------------------------------------------------------------------

Timestamp FuturesTraderAgent::orderPlacementLatency() {
    return static_cast<Timestamp>(std::lerp(m_opl.min, m_opl.max, m_orderPlacementLatencyDistribution->sample(*m_rng)));
}

//-------------------------------------------------------------------------

double FuturesTraderAgent::getProcessValue(BookId bookId, const std::string& name)
{
    return simulation()->exchange()->process(name, bookId)->value();
}

//-------------------------------------------------------------------------
uint64_t FuturesTraderAgent::getProcessCount(BookId bookId, const std::string& name)
{
    return simulation()->exchange()->process(name, bookId)->count();
}
//-------------------------------------------------------------------------