/*
 * SPDX-FileCopyrightText: 2025 Rayleigh Research <to@rayleigh.re>
 * SPDX-License-Identifier: MIT
 */
#include "ALGOTraderAgent.hpp"

#include "DistributionFactory.hpp"
#include "RayleighDistribution.hpp"
#include "Simulation.hpp"

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/variance.hpp>
#include <boost/math/statistics/linear_regression.hpp>  
//-------------------------------------------------------------------------

namespace taosim::agent
{

//-------------------------------------------------------------------------

ALGOTraderVolumeStats::ALGOTraderVolumeStats(size_t period, 
    double alpha, double beta, double omega, double gamma, double initPrice)
    : m_period{period},
        m_alpha{alpha},
        m_beta{beta},
        m_omega{omega},
        m_gamma{gamma},
        m_initPrice{initPrice}
{
    if (m_period == 0) {
        throw std::invalid_argument{fmt::format(
            "{}: period should be > 0, was {}",
            std::source_location::current().file_name(), m_period)};
    }
    if (m_alpha < 0.0) {
        throw std::invalid_argument{fmt::format(
            "{}: alpha should be > 0, was {}",
            std::source_location::current().file_name(), m_alpha)};
    }
    if (m_beta < 0.0) {
        throw std::invalid_argument{fmt::format(
            "{}: beta should be > 0, was {}",
            std::source_location::current().file_name(), m_beta)};
    }
    if (m_omega <= 0.0) {
        throw std::invalid_argument{fmt::format(
            "{}: omega should be > 0, was {}",
            std::source_location::current().file_name(), m_omega)};
    }
    m_priceLast = 0.0;
}
//-------------------------------------------------------------------------

void ALGOTraderVolumeStats::push_levels(Timestamp timestamp, std::vector<BookLevel>& bids, std::vector<BookLevel>& asks)
{
    BookStat volumes = {.bid=volumeSum(bids), .ask=volumeSum(asks)};
    double midquote = taosim::util::decimal2double(bids.front().price) + taosim::util::decimal2double(asks.front().price); 
    m_bookVolumes[timestamp] = volumes;
    m_lastSeq = timestamp; 
    double logret;
    if (m_priceLast <= 0.0) {
        m_priceLast = midquote;
        logret = std::log(midquote / m_initPrice);
        m_estimatedVol = m_omega/(1-m_alpha-m_beta);
    } 
    else {
        logret = std::log(midquote/m_priceLast);
        m_priceLast = midquote;
        m_estimatedVol = m_omega + m_alpha * std::pow(logret,2) + m_beta * m_estimatedVol + m_gamma * m_variance;
    }
}

double ALGOTraderVolumeStats::volumeSum(std::vector<BookLevel>& side) {
    auto volumes = side | views::take(5) | views::transform([] (const auto& level) { return taosim::util::decimal2double(level.quantity);});;
    return std::accumulate(volumes.begin(),volumes.end(),0);
}

double ALGOTraderVolumeStats::slopeOLS(std::vector<BookLevel>& side) {
    using boost::math::statistics::simple_ordinary_least_squares;

    std::vector<double> x = side | views::transform([](const auto& level) { return taosim::util::decimal2double(level.price); }) | ranges::to<std::vector>();;
    std::vector<double> y(side.size());
    ranges::partial_sum(side
    | views::transform([](const auto& level) { return taosim::util::decimal2double(level.price); }), y);
    auto [c0, c1] = simple_ordinary_least_squares(x, y);
    return c1;
}


//-------------------------------------------------------------------------

void ALGOTraderVolumeStats::push(const Trade& trade)
{
    push({.timestamp = trade.timestamp(), .volume = trade.volume(), .price = trade.price()});
}

//-------------------------------------------------------------------------

void ALGOTraderVolumeStats::push(TimestampedVolume timestampedVolume)
{
    auto acc = [&] {
        m_queue.push(timestampedVolume);
        m_rollingSum += timestampedVolume.volume;

        std::priority_queue<TimestampedVolume, std::vector<TimestampedVolume>, std::greater<TimestampedVolume>> temp = m_queue;
        std::vector<double> windowLogRets;
        if (!temp.empty()) {
            TimestampedVolume prev = temp.top(); temp.pop();
            while (!temp.empty()) {
                TimestampedVolume cur = temp.top(); temp.pop();
                if (cur.timestamp == prev.timestamp) {
                    decimal_t totalVol = cur.volume + prev.volume;
                    cur.price = (cur.price * cur.volume + prev.price * prev.volume)/ totalVol; 
                } else if (prev.price != decimal_t{0.0}) {  // avoid division by zero, just in case
                    double logret = std::log(util::decimal2double(cur.price) / util::decimal2double(prev.price));
                    windowLogRets.push_back(logret);
                } else {
                    windowLogRets.push_back(0.0);
                }

                Timestamp period_seqnum = cur.timestamp / m_period;
                m_priceHistory[period_seqnum] = util::decimal2double(cur.price);
                // Update the log returns
                if (period_seqnum == 0) {
                    m_logRets[period_seqnum] = std::log(m_initPrice / m_priceHistory[period_seqnum]);
                }
                else if (m_priceHistory.find(period_seqnum - 1) != m_priceHistory.end()) {
                    m_logRets[period_seqnum] = std::log(m_priceHistory[period_seqnum] / m_priceHistory[period_seqnum - 1]);
                }
                prev = cur;
            }
        }
        
        m_variance = [&] {
            namespace bacc = boost::accumulators;
            bacc::accumulator_set<double, bacc::stats<bacc::tag::lazy_variance>> accum;
            const auto n = windowLogRets.capacity();
            for (auto logRet : windowLogRets) {
                accum(logRet);
            }
            return bacc::variance(accum) * (n - 1) / n;
        }();
    };

    if (m_queue.empty()) [[unlikely]] {
        return acc();
    }

    if (timestampedVolume.timestamp < m_queue.top().timestamp) [[unlikely]] {
        throw std::runtime_error{fmt::format(
            "{}: Attempt adding volume {} with timestamp {} earlier than the top of the queue ({})",
            std::source_location::current().function_name(),
            timestampedVolume.volume, timestampedVolume.timestamp, m_queue.top().timestamp)};
    }

    const bool withinQueueWindow =
        timestampedVolume.timestamp - m_queue.top().timestamp < m_period;
    
    if (!withinQueueWindow) {
        const auto cutoff = timestampedVolume.timestamp - m_period;
        do {
            m_rollingSum -= m_queue.top().volume;
            m_queue.pop();
        } while (!m_queue.empty() && m_queue.top().timestamp <= cutoff);
    }
    
    acc();
}

//-------------------------------------------------------------------------

ALGOTraderVolumeStats ALGOTraderVolumeStats::fromXML(pugi::xml_node node, double initPrice)
{
    const auto period = node.attribute("volumeStatsPeriod").as_ullong();
    if (period == 0) {
        throw std::invalid_argument{fmt::format(
            "{}: attribute 'volumeStatsPeriod' should be > 0, was {}",
            std::source_location::current().file_name(), period)};
    }
    pugi::xml_attribute attr;
    attr = node.attribute("alpha");
    const double alpha = attr.as_double();
    attr = node.attribute("beta");
    const double beta = attr.as_double();
    attr = node.attribute("omega");
    const double omega = attr.as_double();
    attr = node.attribute("gammaX");
    const double gamma = attr.as_double();
    return ALGOTraderVolumeStats{period, alpha, beta, omega, gamma, initPrice};
}

//-------------------------------------------------------------------------

ALGOTraderAgent::ALGOTraderAgent(Simulation* simulation) noexcept
    : Agent{simulation}
{}

//-------------------------------------------------------------------------

void ALGOTraderAgent::configure(const pugi::xml_node& node)
{
    static constexpr auto ctx = std::source_location::current().function_name();

    if (simulation()->exchange() == nullptr) {
        throw std::runtime_error{fmt::format(
            "{}: exchange must be configured a priori", ctx)};
    }

    Agent::configure(node);

    m_rng = &simulation()->rng();
    
    pugi::xml_attribute attr;
    if (m_exchange = node.attribute("exchange").as_string(); m_exchange.empty()) {
        throw std::invalid_argument{fmt::format(
            "{}: attribute 'exchange' should be non-empty", ctx)};
    }

    m_bookCount = simulation()->exchange()->books().size();

    auto getProbAttr = [](pugi::xml_node node, const char* attrName) {
        const float val = node.attribute(attrName).as_float();
        if (!(0.0f <= val && val <= 1.0f)) {
            throw std::invalid_argument{fmt::format(
                "{}: attribute '{}' should be within [0,1], was {}",
                ctx, attrName, val)};
        }
        return val;
    };



    m_volumeDistribution =
        stats::DistributionFactory::createFromXML(node.child("VolumeDistribution"));

    double initPrice = simulation()->exchange()->process("fundamental", BookId{})->value();
    m_state = [&] {
        std::vector<ALGOTraderState> state;
        for (BookId bookId = 0; bookId < m_bookCount; ++bookId) {
            state.push_back(ALGOTraderState{
                .status = ALGOTraderStatus::ASLEEP,
                .volumeStats = ALGOTraderVolumeStats::fromXML(node, initPrice),
                .volumeToBeExecuted = 0_dec,
                .direction = OrderDirection::BUY
            });
        }
        return state;
    }();

    m_period = node.attribute("volumeStatsPeriod").as_ullong();

    m_marketFeedLatencyDistribution = std::normal_distribution<double>{
        [&] {
            static constexpr const char* name = "MFLmean";
            auto attr = node.attribute(name);
            return attr.empty() ? 1'000'000'000.0 : attr.as_double(); 
        }(),
        [&] {
            static constexpr const char* name = "MFLstd";
            auto attr = node.attribute(name);
            return attr.empty() ? 1'000'000'000.0 : attr.as_double(); 
        }()
    };

    attr = node.attribute("depth");
    m_depth = (attr.empty() || attr.as_uint() == 0) ? 21 : attr.as_uint();

    if (attr = node.attribute("minOPLatency"); attr.as_ullong() == 0) {
        throw std::invalid_argument(fmt::format(
            "{}: attribute 'minOPLatency' should have a value greater than 0", ctx));
    }
     m_opl.min = attr.as_ullong();
    if (attr = node.attribute("maxOPLatency"); attr.as_ullong() == 0) {
        throw std::invalid_argument(fmt::format(
            "{}: attribute 'maxOPLatency' should have a value greater than 0", ctx));
    }
    m_opl.max = attr.as_ullong();
    if (m_opl.min >= m_opl.max) {
        throw std::invalid_argument(fmt::format(
            "{}: minOP ({}) should be strictly less maxOP ({})", ctx, m_opl.min, m_opl.max));
    }
    attr = node.attribute("opLatencyScaleRay"); 
    const double scale = (attr.empty() || attr.as_double() == 0.0) ? 0.235 : attr.as_double();
    const double percentile = 1-std::exp(-1/(2*scale*scale));
    m_orderPlacementLatencyDistribution =  std::make_unique<taosim::stats::RayleighDistribution>(scale, percentile); 

    m_lastPrice =  std::vector<decimal_t>(m_bookCount, simulation()->exchange()->config2().initialPrice);

    attr = node.attribute("updateInterval");
    const double delayMean = (attr.empty() || attr.as_double() <= 0.0) ? 300'000'000'000.0 : attr.as_double();
    attr = node.attribute("updateSTD");
    const double delaySTD = (attr.empty() || attr.as_double() <= 0.0) ? 120'000'000'000.0 : attr.as_double();
    m_delay = std::normal_distribution<double>{delayMean, delaySTD};

    attr = node.attribute("volumeDrawRayleighScale");
    // Consider to change base to quote => simpler default
    const double scale2 = (attr.empty() || attr.as_double() == 0.0) ? 1'000'000'000.0/util::decimal2double(simulation()->exchange()->config2().initialPrice)
     : attr.as_double();
    m_volumeDrawDistribution =  std::make_unique<taosim::stats::RayleighDistribution>(scale2, 1.0); 

    attr = node.attribute("departure");
    const double deptSTD = (attr.empty() || attr.as_double() == 0.0) ? 0.025 : attr.as_double();
    m_departureThreshold = std::normal_distribution<double>{0,deptSTD};  
    
    attr = node.attribute("sensitivity");
    m_wakeupProb =  (attr.empty() || attr.as_float() == 0.0) ? 1 - 0.05 : attr.as_float();
    attr = node.attribute("volumeProb");
    m_volumeProb = (attr.empty() ||attr.as_double() <= 0.0) ? 0.25 : attr.as_double();

    attr = node.attribute("activationMidpoint");
    m_volatilityBounds.activationMidpoint = (attr.empty() || attr.as_double() <= 0.0) ? 0.025 : attr.as_double();
    attr = node.attribute("activationRate");
    m_volatilityBounds.activationRate = (attr.empty() || attr.as_double() <= 0.0) ? 100.0 : attr.as_double();
    attr = node.attribute("capacity");
    m_volatilityBounds.activationCapacity = (attr.empty() || attr.as_double() <= 0.0 || attr.as_double() > 1.0) ? 1.0 : attr.as_double();
    m_immediateBase = node.attribute("immediateBase").as_double(1000.0);
    m_topLevel = std::vector<TopLevel>(m_bookCount, TopLevel{});
}

//-------------------------------------------------------------------------

void ALGOTraderAgent::receiveMessage(Message::Ptr msg)
{
    if (msg->type == "EVENT_SIMULATION_START") {
        handleSimulationStart(msg);
    }
    else if (msg->type == "EVENT_TRADE") {
        handleTrade(msg);
    }
    else if (msg->type == "WAKEUP_ALGOTRADER") {
        handleWakeup(msg);
    }
    else if (msg->type == "RESPONSE_PLACE_ORDER_MARKET") {
        handleMarketOrderResponse(msg);
    }
    else if (msg->type == "RESPONSE_RETRIEVE_L2") {
        handleBookResponse(msg);
    } 
    else if (msg->type == "RESPONSE_RETRIEVE_L1") {
        handleL1Response(msg);
    }
}

//-------------------------------------------------------------------------

void ALGOTraderAgent::handleSimulationStart(Message::Ptr msg)
{
    simulation()->dispatchMessage(
        simulation()->currentTimestamp(),
        1,
        name(),
        m_exchange,
        "SUBSCRIBE_EVENT_TRADE");
    Timestamp initDelay = 600'000'000'000;
    if (simulation()->currentTimestamp() != static_cast<Timestamp>(0)) {
        fmt::println("Initial timestamp is not zero");
        initDelay -= simulation()->currentTimestamp();
    } 
    simulation()->dispatchMessage(
        simulation()->currentTimestamp(),
            initDelay,
            name(),
            name(),
            "WAKEUP_ALGOTRADER");

    for (BookId bookId = 0; bookId < m_bookCount; ++bookId) {

        auto& state = m_state.at(bookId);
        const auto& balances =  simulation()->account(name()).at(bookId);
        const decimal_t volumeToBeExecuted = drawNewVolume(balances.m_baseDecimals); 
        state.volumeToBeExecuted = std::min(volumeToBeExecuted, balances.base.getFree());
        simulation()->dispatchMessage(
            simulation()->currentTimestamp(),
            m_period,
            name(),
            m_exchange,
            "RETRIEVE_L2",
            MessagePayload::create<RetrieveL2Payload>(m_depth,bookId)
        );
    }
}


//-------------------------------------------------------------------------

void ALGOTraderAgent::handleTrade(Message::Ptr msg)
{
    const auto payload = std::dynamic_pointer_cast<EventTradePayload>(msg->payload);
    const BookId bookId = payload->bookId;
    m_lastPrice.at(bookId) = payload->trade.price();
    m_state.at(payload->bookId).volumeStats.push(payload->trade);
}


//-------------------------------------------------------------------------
void ALGOTraderAgent::handleBookResponse(Message::Ptr msg) 
{
    const auto payload = std::dynamic_pointer_cast<RetrieveL2ResponsePayload>(msg->payload);
    BookId bookId = payload->bookId;
    m_state.at(bookId).volumeStats.push_levels(static_cast<Timestamp>(payload->time / m_period), payload->bids,payload->asks);
    auto& topLevel = m_topLevel.at(bookId);
    topLevel.bid = taosim::util::decimal2double(payload->bids.front().quantity);
    topLevel.ask = taosim::util::decimal2double(payload->asks.front().quantity);

    const double fundamental = getProcessValue(bookId, "fundamental");
    const double lastPrice = util::decimal2double(m_lastPrice.at(bookId));
    auto& state = m_state.at(bookId);
    const auto& balances =  simulation()->account(name()).at(bookId);
    
    if (fundamental >= lastPrice) {
         if (state.status != ALGOTraderStatus::EXECUTING  && state.volumeStats.askVolume() >= m_immediateBase) {
            state.status = ALGOTraderStatus::EXECUTING;
            state.direction = OrderDirection::BUY;
            state.volumeToBeExecuted = taosim::util::double2decimal(topLevel.ask,balances.m_baseDecimals);
            execute(bookId,state);
         }
    }
    else {
        if (state.status != ALGOTraderStatus::EXECUTING  && state.volumeStats.bidVolume() >= m_immediateBase) {
            state.status = ALGOTraderStatus::EXECUTING;
            state.direction = OrderDirection::SELL;
            state.volumeToBeExecuted = taosim::util::double2decimal(topLevel.bid,balances.m_baseDecimals);
            execute(bookId,state);
         }
    }
    simulation()->dispatchMessage(
        simulation()->currentTimestamp(),
        m_period,
        name(),
        m_exchange,
        "RETRIEVE_L2",
        MessagePayload::create<RetrieveL2Payload>(m_depth,bookId)
    );

}

void ALGOTraderAgent::handleL1Response(Message::Ptr msg) {
        const auto payload = std::dynamic_pointer_cast<RetrieveL1ResponsePayload>(msg->payload);    
        const BookId bookId = payload->bookId;
        auto& topLevel = m_topLevel.at(bookId);
        topLevel.bid = taosim::util::decimal2double(payload->bestBidVolume);
        topLevel.ask = taosim::util::decimal2double(payload->bestAskVolume);
        auto& state = m_state.at(bookId);
        execute(bookId, state);
}




void ALGOTraderAgent::handleWakeup(Message::Ptr msg)
{
    for (BookId bookId = 0; bookId < m_bookCount; ++bookId) {

        auto& state = m_state.at(bookId);
        if (state.status == ALGOTraderStatus::EXECUTING) continue;
        const auto& balances =  simulation()->account(name()).at(bookId);
        const auto& baseBalance = balances.base;

        const double fundamental = getProcessValue(bookId, "fundamental");
        const double lastPrice = util::decimal2double(m_lastPrice.at(bookId));
        const double relativeDiff = std::abs(fundamental - lastPrice)/lastPrice;
        if (std::bernoulli_distribution{wakeupProb(state)}(*m_rng)) {
            if (fundamental >= lastPrice) {
                const decimal_t volumeToBeExecuted = drawNewVolume(balances.m_baseDecimals); 
                state.status = ALGOTraderStatus::EXECUTING;
                state.direction = OrderDirection::BUY;
                state.marketFeedLatency = static_cast<Timestamp>(0);
                state.volumeToBeExecuted = std::min(volumeToBeExecuted,
                balances.quote.getFree()/m_lastPrice.at(bookId));
            } else if (fundamental <= lastPrice) {
                const decimal_t volumeToBeExecuted = drawNewVolume(balances.m_baseDecimals); 
                state.status = ALGOTraderStatus::EXECUTING;
                state.direction = OrderDirection::SELL;
                state.marketFeedLatency = static_cast<Timestamp>(0);
                state.volumeToBeExecuted = std::min(volumeToBeExecuted,
                                            baseBalance.getFree());
            }
        }
       
        if (state.status == ALGOTraderStatus::EXECUTING) {
            execute(bookId, state);
        } 
    }
    const Timestamp delay = static_cast<Timestamp>(std::min(
            std::abs(m_delay(*m_rng)),
            m_delay.mean()
            + 3.0 * m_delay.stddev()));

    simulation()->dispatchMessage(
            simulation()->currentTimestamp(),
            delay,
            name(),
            name(),
            "WAKEUP_ALGOTRADER");  
}

//-------------------------------------------------------------------------

void ALGOTraderAgent::handleMarketOrderResponse(Message::Ptr msg)
{
    const auto payload = std::dynamic_pointer_cast<PlaceOrderMarketResponsePayload>(msg->payload);
    const auto requestPayload = payload->requestPayload;

    const decimal_t executedVolume = requestPayload->volume;
    const BookId bookId = requestPayload->bookId;
    auto& state = m_state.at(bookId);
    state.volumeToBeExecuted -= executedVolume;
    
    simulation()->logDebug("{} EXECUTED {}", name(), executedVolume);
    if (state.volumeToBeExecuted <= 1_dec) { 
        simulation()->logDebug("{} FALLING ASLEEP", name());
        state.status = ALGOTraderStatus::ASLEEP; 
        const auto& balances =  simulation()->account(name()).at(bookId);
        state.volumeToBeExecuted =  drawNewVolume(balances.m_baseDecimals); 
    } else {
        state.marketFeedLatency = static_cast<Timestamp>(std::min(
            std::abs(m_marketFeedLatencyDistribution(*m_rng)),
            m_marketFeedLatencyDistribution.mean()
            + 3.0 * m_marketFeedLatencyDistribution.stddev()));
        simulation()->dispatchMessage(
            simulation()->currentTimestamp(),
            state.marketFeedLatency,
            name(),
            m_exchange,
            "RETRIEVE_L1",
            MessagePayload::create<RetrieveL1Payload>(bookId));
    }
}
//-------------------------------------------------------------------------

void ALGOTraderAgent::execute(BookId bookId, ALGOTraderState& state)
{
    const auto& balances = simulation()->account(name()).at(bookId) ;
    const auto& baseBalance = balances.base;
    double levelVolume = state.direction == OrderDirection::BUY ? state.volumeStats.askVolume() : state.volumeStats.bidVolume();
    double topLevelVolume = state.direction == OrderDirection::BUY ? m_topLevel.at(bookId).ask : m_topLevel.at(bookId).bid;
    const decimal_t drawnQty = util::double2decimal(
                                        std::max(m_volumeDistribution->sample(*m_rng), topLevelVolume),
                                        balances.m_baseDecimals);
    const decimal_t volume = std::min(drawnQty,
                                         state.volumeToBeExecuted);
    const decimal_t volumeToExecute = state.direction == OrderDirection::BUY ? 
    std::min(volume, balances.quote.getFree()/m_lastPrice.at(bookId))
        : std::min(volume, baseBalance.getFree());


    simulation()->logDebug(
        "{} ATTEMPTING TO EXECUTE {} OF {}, | at {}", name(), state.direction, volumeToExecute, simulation()->currentTimestamp());

    simulation()->dispatchMessage( 
        simulation()->currentTimestamp(),
        orderPlacementLatency(),
        name(),
        m_exchange,
        "PLACE_ORDER_MARKET",
        MessagePayload::create<PlaceOrderMarketPayload>(
            state.direction, volumeToExecute, bookId));
}
//-------------------------------------------------------------------------
double ALGOTraderAgent::wakeupProb(ALGOTraderState& state) {
    double probability = m_volatilityBounds.activationCapacity/
        (1 + std::exp(m_volatilityBounds.activationRate*(state.volumeStats.estimatedVolatility() -  m_volatilityBounds.activationMidpoint)));
    // redundant sanity check
    return std::min(1.0,std::max(probability,0.0));
}

//-------------------------------------------------------------------------
decimal_t ALGOTraderAgent::drawNewVolume(uint32_t baseDecimals) {
        const double rayleighDraw = m_volumeDrawDistribution->sample(*m_rng);
        return  util::double2decimal(rayleighDraw,baseDecimals);
}
//-------------------------------------------------------------------------

Timestamp ALGOTraderAgent::orderPlacementLatency() {
    return static_cast<Timestamp>(std::lerp(m_opl.min, m_opl.max, m_orderPlacementLatencyDistribution->sample(*m_rng)));
}

//-------------------------------------------------------------------------
double ALGOTraderAgent::getProcessValue(BookId bookId, const std::string& name)
{
    return simulation()->exchange()->process(name, bookId)->value();
}


//-------------------------------------------------------------------------
uint64_t ALGOTraderAgent::getProcessCount(BookId bookId, const std::string& name)
{
    return simulation()->exchange()->process(name, bookId)->count();
}
//-------------------------------------------------------------------------
//-------------------------------------------------------------------------

}  // namespace taosim::agent

//-------------------------------------------------------------------------
