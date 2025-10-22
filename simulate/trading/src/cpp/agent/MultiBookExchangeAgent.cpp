/*
 * SPDX-FileCopyrightText: 2025 Rayleigh Research <to@rayleigh.re>
 * SPDX-License-Identifier: MIT
 */
#include "MultiBookExchangeAgent.hpp"

#include "BookFactory.hpp"
#include "Simulation.hpp"
#include "taosim/exchange/FeePolicy.hpp"
#include "taosim/book/FeeLogger.hpp"
#include "util.hpp"
#include "InstructionLogger.hpp"
#include "taosim/simulation/replay_helpers.hpp"

#include <boost/algorithm/string/erase.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <date/date.h>
#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <latch>
#include <memory>
#include <source_location>
#include <sstream>
#include <string_view>
#include <utility>

//-------------------------------------------------------------------------

MultiBookExchangeAgent::MultiBookExchangeAgent(Simulation* simulation) noexcept
    : Agent{simulation, "EXCHANGE"}
{}

//-------------------------------------------------------------------------

std::span<Book::Ptr> MultiBookExchangeAgent::books() noexcept
{
    return m_books;
}

//-------------------------------------------------------------------------

taosim::accounting::Account& MultiBookExchangeAgent::account(const LocalAgentId& agentId)
{
    return m_clearingManager->accounts()[agentId];
}

//-------------------------------------------------------------------------

ExchangeSignals* MultiBookExchangeAgent::signals(BookId bookId)
{
    return m_signals.at(bookId).get();
}

//-------------------------------------------------------------------------

Process* MultiBookExchangeAgent::process(const std::string& name, BookId bookId)
{
    return m_bookProcessManager->at(name).at(bookId).get();
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::retainRecord(bool flag) noexcept
{
    m_retainRecord = flag;
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::checkMarginCall() noexcept
{

    taosim::decimal_t bestAsk{}, bestBid{};
    for (const auto book : m_books) {
        const auto bookId = book->id();
        
        //-------------------------- Margin Buying orders ------------------------
        if (!book->buyQueue().empty()){
            
            auto bestBuyDeque = &book->buyQueue().back();
            bestBid = bestBuyDeque->price();
            auto& m_marginBuy = m_clearingManager->getMarginBuys();
            
            if (!m_marginBuy.empty()){
                auto marginIt = m_marginBuy.find(bookId);
                
                if (marginIt != m_marginBuy.end()) {
                    auto& marginBuyOrders = marginIt->second;

                    auto it = marginBuyOrders.upper_bound(bestBid);

                    for (auto tempIt = it; tempIt != marginBuyOrders.end(); ++tempIt) {
                        const auto& ids = tempIt->second;
                        for (auto idIt = ids.begin(); idIt != ids.end(); ++idIt) {

                            const auto& loan = accounts()[idIt->agentId][bookId].getLoan(idIt->orderId);
                            if (loan.has_value()){
                                taosim::decimal_t remainingVolume = loan->get().amount();

                                simulation()->logDebug("Margin Call for BUY order #{} of agent {} at price {} (marginCall:{}) in Book {} for volume {}x{}",
                                    idIt->orderId,
                                    idIt->agentId,
                                    bestBid,
                                    loan->get().marginCallPrice(),
                                    simulation()->bookIdCanon(bookId),
                                    taosim::util::dec1p(loan->get().leverage()),
                                    remainingVolume
                                );

                                if (idIt->agentId < 0){

                                    simulation()->dispatchMessageWithPriority(
                                        simulation()->currentTimestamp(),
                                        0,
                                        accounts().idBimap().right.at(idIt->agentId),
                                        name(),
                                        "PLACE_ORDER_MARKET_MC",
                                        MessagePayload::create<PlaceOrderMarketPayload>(
                                            OrderDirection::SELL,
                                            remainingVolume,
                                            bookId,
                                            Currency::QUOTE,
                                            std::nullopt,
                                            STPFlag::CO,
                                            idIt->orderId
                                        ),
                                        m_marginCallCounter++
                                    );

                                } else {

                                    simulation()->dispatchMessageWithPriority(
                                        simulation()->currentTimestamp(),
                                        0,
                                        "DISTRIBUTED_PROXY_AGENT",
                                        name(),
                                        "DISTRIBUTED_PLACE_ORDER_MARKET_MC",
                                        MessagePayload::create<DistributedAgentResponsePayload>(
                                            idIt->agentId,
                                            MessagePayload::create<PlaceOrderMarketPayload>(
                                                OrderDirection::SELL,
                                                remainingVolume,
                                                bookId,
                                                Currency::QUOTE,
                                                std::nullopt,
                                                STPFlag::CO,
                                                idIt->orderId
                                            )
                                        ),
                                        m_marginCallCounter++
                                    );

                                }

                                

                                // throw std::runtime_error("\n#################    CHECK THE RESULT     ##################\n");
                            }

                        }
                    }
                    ///##/ This is important to check whether the order successfully happens
                    // if it is not then it should be resend
                    // if it succeeds it should remove the id from m_marginBuy
                    // but if we don't remove it here it might happen several times?
                    // this should be checked and the same for m_marginSell ///##/
                    marginBuyOrders.erase(it, marginBuyOrders.end());

                    if (marginBuyOrders.empty()) {
                        m_marginBuy.erase(marginIt);
                    }
                }       
            }
        }

        //-------------------------- Short Selling orders ------------------------
        if (!book->sellQueue().empty()){
            auto bestSellDeque = &book->sellQueue().front();
            bestAsk = bestSellDeque->price();
            auto& m_marginSell = m_clearingManager->getMarginSells();
            
            if (!m_marginSell.empty()){
                auto marginIt = m_marginSell.find(bookId);
                if (marginIt != m_marginSell.end()) {
                    auto& marginSellOrders = marginIt->second;

                    auto it = marginSellOrders.lower_bound(bestAsk);

                    for (auto tempIt = marginSellOrders.begin(); tempIt != it; ++tempIt) {

                        const auto& ids = tempIt->second;
                        
                        for (auto idIt = ids.begin(); idIt != ids.end(); ++idIt) {

                            const auto& loan = accounts()[idIt->agentId][bookId].getLoan(idIt->orderId);
                            if (loan.has_value()){
                                taosim::decimal_t remainingVolume = loan->get().amount();
                                

                                simulation()->logDebug("Margin Call for SELL order #{} of agent {} at price {} (marginCall:{}) in Book {} for volume {}x{}",
                                    idIt->orderId,
                                    idIt->agentId,
                                    bestAsk,
                                    loan->get().marginCallPrice(),
                                    simulation()->bookIdCanon(bookId),
                                    taosim::util::dec1p(loan->get().leverage()),
                                    remainingVolume
                                );
                                
                                if (idIt->agentId < 0){
                                    
                                    simulation()->dispatchMessageWithPriority(
                                        simulation()->currentTimestamp(),
                                        0,
                                        accounts().idBimap().right.at(idIt->agentId),
                                        name(),
                                        "PLACE_ORDER_MARKET_MC",
                                        MessagePayload::create<PlaceOrderMarketPayload>(
                                            OrderDirection::BUY,
                                            remainingVolume,
                                            bookId,
                                            Currency::BASE,
                                            std::nullopt,
                                            STPFlag::CO,
                                            idIt->orderId
                                        ),
                                        m_marginCallCounter++
                                    );

                                } else {

                                    simulation()->dispatchMessageWithPriority(
                                        simulation()->currentTimestamp(),
                                        0,
                                        "DISTRIBUTED_PROXY_AGENT",
                                        name(),
                                        "DISTRIBUTED_PLACE_ORDER_MARKET_MC",
                                        MessagePayload::create<DistributedAgentResponsePayload>(
                                            idIt->agentId,
                                            MessagePayload::create<PlaceOrderMarketPayload>(
                                                OrderDirection::BUY,
                                                remainingVolume,
                                                bookId,
                                                Currency::BASE,
                                                std::nullopt,
                                                STPFlag::CO,
                                                idIt->orderId
                                            )
                                        ),
                                        m_marginCallCounter++
                                    );

                                }

                            }
                            
                        }
                        
                    }
                    
                    
                    marginSellOrders.erase(marginSellOrders.begin(), it);

                    if (marginSellOrders.empty()) {
                        m_marginSell.erase(marginIt);
                    }
                }
            }
            
        }
    }

}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::configure(const pugi::xml_node& node)
{
    static constexpr auto ctx = std::source_location::current().function_name();

    Agent::configure(node);

    m_config.configure(node);

    // TODO: This monstrosity should be split up somehow.
    try {
        m_config2 = taosim::exchange::makeExchangeConfig(node);

        m_eps = taosim::util::double2decimal(node.attribute("eps").as_double());

        const auto booksNode = node.child("Books");
        const uint32_t bookCount = booksNode.attribute("instanceCount").as_uint();
        const std::string bookAlgorithm = booksNode.attribute("algorithm").as_string();
        const size_t maxDepth = booksNode.attribute("maxDepth").as_ullong(21);
        const size_t detailedDepth = booksNode.attribute("detailedDepth").as_ullong(5);

        m_bookProcessManager = BookProcessManager::fromXML(
            booksNode, const_cast<Simulation*>(simulation()), &m_config2);
        m_clearingManager = std::make_unique<taosim::exchange::ClearingManager>(
            this,
            std::make_unique<taosim::exchange::FeePolicyWrapper>(
                taosim::exchange::FeePolicy::fromXML(
                    node.child("FeePolicy"), const_cast<Simulation*>(simulation())), &accounts()),
            taosim::exchange::OrderPlacementValidator::Parameters{
                .volumeIncrementDecimals = m_config.parameters().volumeIncrementDecimals,
                .priceIncrementDecimals = m_config.parameters().priceIncrementDecimals,
                .baseIncrementDecimals = m_config.parameters().baseIncrementDecimals,
                .quoteIncrementDecimals = m_config.parameters().quoteIncrementDecimals
            });

        simulation()->logDebug("TIERED FEE POLICY");
        int c = 0;
        for (taosim::exchange::Tier tier : m_clearingManager->feePolicy()->defaultPolicy()->tiers()) {
            simulation()->logDebug("TIER {} : VOL >= {} | MAKER {} TAKER {}", c, 
                tier.volumeRequired, 
                tier.makerFeeRate, tier.takerFeeRate
            );
            c++;
        }

        const auto balancesNode = node.child("Balances");
        const auto baseNode = balancesNode.child("Base");
        const auto quoteNode = balancesNode.child("Quote");

        std::chrono::system_clock::time_point startTimePoint;
        std::string L2LogTag;
        int L2Depth;
        std::string L3LogTag;
        std::string feeLogTag;
        pugi::xml_node loggingNode;
        pugi::xml_node L2Node;
        pugi::xml_node L3Node;
        pugi::xml_node feeLogNode;
        pugi::xml_node replayNode;
        if (loggingNode = node.child("Logging")) {
            std::istringstream in{loggingNode.attribute("startDate").as_string()};
            // TODO: Handle the timezone
            date::from_stream(in, "%Y-%m-%d %H:%M:%S", startTimePoint);
            if (L2Node = loggingNode.child("L2")) {
                L2LogTag = L2Node.attribute("tag").as_string();
                L2Depth = L2Node.attribute("depth").as_int(21);
            }
            if (L3Node = loggingNode.child("L3")) {
                L3LogTag = L3Node.attribute("tag").as_string();
            }
            if (feeLogNode = loggingNode.child("FeeLog")) {
                feeLogTag = feeLogNode.attribute("tag").as_string();
            }
            replayNode = loggingNode.child("Replay");
        }

        m_L3Record = taosim::event::L3RecordContainer{bookCount};

        for (BookId bookId{}; bookId < bookCount; ++bookId) {
            auto book = BookFactory::createBook(
                bookAlgorithm,
                simulation(),
                bookId,
                maxDepth,
                detailedDepth);
            book->signals().orderCreated.connect(
                [this](Order::Ptr order, OrderContext ctx) { orderCallback(order, ctx); });
            book->signals().orderLog.connect(
                [this](Order::Ptr order, OrderContext ctx) { orderLogCallback(order, ctx); });
            book->signals().trade.connect(
                [this](Trade::Ptr trade, BookId bookId) { tradeCallback(trade, bookId); });
            book->signals().unregister.connect(
                [this](LimitOrder::Ptr order, BookId bookId) {
                    unregisterLimitOrderCallback(order, bookId);
                });
            book->signals().cancelOrderDetails.connect(
                [this](LimitOrder::Ptr order, taosim::decimal_t volumeToCancel, BookId bookId) {
                    m_clearingManager->handleCancelOrder({
                        .bookId = bookId,
                        .order = order,
                        .volumeToCancel = volumeToCancel
                    });
                    m_L3Record.at(bookId).push(taosim::event::CancellationEvent(
                        taosim::event::Cancellation(order->id(), volumeToCancel),
                        simulation()->currentTimestamp(),
                        order->price()
                    ));
                });
            book->signals().marketOrderProcessed.connect(
                [this](MarketOrder::Ptr marketOrder, OrderContext ctx) {
                    marketOrderProcessedCallback(marketOrder, ctx);
                });
            m_books.push_back(book);
            m_signals[bookId] = std::make_unique<ExchangeSignals>();
            const BookId bookIdCanon = simulation()->m_blockIdx * bookCount + bookId;
            if (loggingNode) {
                if (L2Node) {
                    const fs::path logPath =
                        simulation()->logDir() / fmt::format(
                            "{}L2-{}.log",
                            !L2LogTag.empty() ? L2LogTag + "-" : "",
                            bookIdCanon);
                    m_L2Loggers[bookId] = std::make_unique<L2Logger>(
                        logPath,
                        L2Depth,
                        startTimePoint,
                        book->signals(),
                        simulation());
                }
                if (L3Node) {
                    const fs::path logPath =
                        simulation()->logDir() / fmt::format(
                            "{}L3-{}.log",
                            !L3LogTag.empty() ? L3LogTag + "-" : "",
                            bookIdCanon);
                    m_L3EventLoggers[bookId] = std::make_unique<L3EventLogger>(
                        logPath,
                        startTimePoint,
                        m_signals.at(bookId)->L3,
                        simulation());
                }
                if (feeLogNode) {
                    const fs::path logPath =
                        simulation()->logDir() / fmt::format(
                            "{}fees-{}.log",
                            !feeLogTag.empty() ? feeLogTag + "-" : "",
                            bookIdCanon);
                    m_feeLoggers[bookId] = std::make_unique<FeeLogger>(
                        logPath,
                        startTimePoint,
                        m_signals.at(bookId)->feeLog,
                        simulation());
                }
                if (replayNode) {
                    m_replayLog = true;
                    const fs::path path =
                        simulation()->logDir() / fmt::format("Replay-{}.log", bookIdCanon);
                    m_replayEventLoggers.push_back(
                        std::make_unique<taosim::exchange::ReplayEventLogger>(
                            path, startTimePoint, simulation()));
                }
            }
        }

        auto doc = std::make_shared<pugi::xml_document>();
        doc->append_copy(balancesNode);
        m_clearingManager->accounts().setAccountTemplate([this, doc, bookCount] {
            const pugi::xml_node balancesNode = doc->child("Balances");
            taosim::accounting::Account accountTemplate;
            for (auto bookId : views::iota(BookId{}, bookCount)) {
                accountTemplate.holdings().push_back(
                    taosim::accounting::Balances::fromXML(
                        balancesNode,
                        taosim::accounting::RoundParams{
                            .baseDecimals = m_config.parameters().baseIncrementDecimals,
                            .quoteDecimals = m_config.parameters().quoteIncrementDecimals}));
                accountTemplate.activeOrders().emplace_back();
            }
            return accountTemplate;
        });

        if (const uint32_t remoteAgentCount = node.attribute("remoteAgentCount").as_uint()) {
            for (AgentId agentId{}; agentId < remoteAgentCount; ++agentId) {
                m_clearingManager->accounts().registerRemote();
            }
        }

        simulation()->signals().agentsCreated.connect([=, this] {
            if (!balancesNode.attribute("log").as_bool()) return;
            for (BookId bookId = 0; bookId < bookCount; ++bookId) {
                auto balanceLogger = std::make_unique<taosim::accounting::BalanceLogger>(
                    simulation()->logDir() / fmt::format("bals-{}.log", bookId),
                    m_signals.at(bookId)->L3,
                    &accounts());
                m_balanceLoggers.push_back(std::move(balanceLogger));
            }
        });
        simulation()->signals().agentsCreated.connect([this] {
            if (!m_replayLog) return;
            rapidjson::Document json{rapidjson::kObjectType};
            auto& allocator = json.GetAllocator();
            for (const auto& [agentId, acct] : accounts()) {
                json.AddMember(
                    rapidjson::Value{std::to_string(agentId).c_str(), allocator},
                    [&] {
                        rapidjson::Document balancesJson{rapidjson::kArrayType, &allocator};
                        for (const auto& bals : acct) {
                            rapidjson::Document balanceJson{rapidjson::kObjectType, &allocator};
                            balanceJson.AddMember(
                                "base",
                                rapidjson::Value{taosim::util::packDecimal(bals.base.getTotal())},
                                allocator);
                            balanceJson.AddMember(
                                "quote",
                                rapidjson::Value{taosim::util::packDecimal(bals.quote.getTotal())},
                                allocator);
                            balancesJson.PushBack(balanceJson, allocator);
                        }
                        return balancesJson;
                    }().Move(),
                    allocator);
            }
            std::ofstream ofs{
                simulation()->logDir() /
                fmt::format(
                    "Replay-Balances-{}-{}.json",
                    simulation()->blockIdx() * m_books.size(),
                    (simulation()->blockIdx() + 1) * m_books.size() - 1)};
            taosim::json::dumpJson(
                json, ofs, taosim::json::FormatOptions{.indent = taosim::json::IndentOptions{}});
        });
    }
    catch (...) {
        handleException();
    }
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::receiveMessage(Message::Ptr msg)
{
    try {
        if (msg->type.starts_with("DISTRIBUTED")) {
            return handleDistributedMessage(msg);
        }
        handleLocalMessage(msg);
    }
    catch (...) {
        handleException();
    }
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::checkpointSerialize(
    rapidjson::Document& json, const std::string& key) const
{
    auto serialize = [this](rapidjson::Document& json) {
        auto& allocator = json.GetAllocator();
        m_clearingManager->accounts().checkpointSerialize(json);

        for (AgentId agentId : views::keys(m_clearingManager->accounts())) {
            const auto agentIdStr = std::to_string(agentId);
            const char* agentIdCStr = agentIdStr.c_str();
            rapidjson::Document ordersJson{rapidjson::kArrayType, &allocator};
            json[agentIdCStr].AddMember("orders", ordersJson, allocator);
        }

        for (const auto book : m_books) {
            const BookId bookId = book->id();
            for (const auto& [agentId, holdings] : m_clearingManager->accounts()) {
                const auto agentIdStr = std::to_string(agentId);
                const char* agentIdCStr = agentIdStr.c_str();
                json[agentIdCStr]["orders"].PushBack(
                    rapidjson::Document{rapidjson::kArrayType, &allocator},
                    allocator);
            }
            for (const taosim::book::TickContainer& bidLevel : book->buyQueue()) {
                for (const auto& bid : bidLevel) {
                    const auto [agentId, clientOrderId] = m_books[bookId]->orderClientContext(bid->id());
                    const auto agentIdStr = std::to_string(agentId);
                    const char* agentIdCStr = agentIdStr.c_str();
                    rapidjson::Document orderJson{&allocator};
                    bid->checkpointSerialize(orderJson);
                    taosim::json::setOptionalMember(orderJson, "clientOrderId", clientOrderId);
                    json[agentIdCStr]["orders"][bookId].PushBack(orderJson, allocator);
                }
            }
            for (const taosim::book::TickContainer& askLevel : book->sellQueue()) {
                for (const auto& ask : askLevel) {
                    const auto [agentId, clientOrderId] = m_books[bookId]->orderClientContext(ask->id());
                    const auto agentIdStr = std::to_string(agentId);
                    const char* agentIdCStr = agentIdStr.c_str();
                    rapidjson::Document orderJson{&allocator};
                    ask->checkpointSerialize(orderJson);
                    taosim::json::setOptionalMember(orderJson, "clientOrderId", clientOrderId);
                    json[agentIdCStr]["orders"][bookId].PushBack(orderJson, allocator);
                }
            }
        }
    };
    taosim::json::serializeHelper(json, key, serialize);
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::jsonSerialize(
    rapidjson::Document& json, const std::string& key) const
{
    auto serialize = [this](rapidjson::Document& json) {
        json.SetObject();
        auto& allocator = json.GetAllocator();
        json.AddMember("logDir", rapidjson::Value{simulation()->logDir().c_str(), allocator}, allocator);
        auto serializeBooks = [this](rapidjson::Document& json) {
            json.SetArray();
            auto& allocator = json.GetAllocator();
            for (const auto book : m_books) {
                const BookId bookId = book->id();
                rapidjson::Document bookJson{rapidjson::kObjectType, &allocator};
                bookJson.AddMember("bookId", rapidjson::Value{bookId}, allocator);
                m_L3Record.at(bookId).jsonSerialize(bookJson, "record");
                rapidjson::Document bidAskJson{&allocator};
                book->jsonSerialize(bidAskJson);
                bookJson.AddMember("bid", bidAskJson["bid"], allocator);
                bookJson.AddMember("ask", bidAskJson["ask"], allocator);
                json.PushBack(bookJson, allocator);
            }
        };
        taosim::json::serializeHelper(json, "books", serializeBooks);
        auto serializeAccounts = [this](rapidjson::Document& json) {
            auto& allocator = json.GetAllocator();
            m_clearingManager->accounts().jsonSerialize(json);
            for (AgentId agentId : views::keys(accounts())) {
                const auto agentIdStr = std::to_string(agentId);
                const char* agentIdCStr = agentIdStr.c_str();
                rapidjson::Document ordersJson{rapidjson::kArrayType, &allocator};
                json[agentIdCStr].AddMember("orders", ordersJson, allocator);
                const auto feePolicy = m_clearingManager->feePolicy();
                rapidjson::Document feesJson{rapidjson::kObjectType, &allocator};
                for (BookId bookId : views::iota(0u, m_books.size())) {
                    taosim::json::serializeHelper(
                        feesJson,
                        std::to_string(bookId).c_str(),
                        [&](rapidjson::Document& feeJson) {
                            feeJson.SetObject();
                            auto& allocator = feeJson.GetAllocator();
                            feeJson.AddMember(
                                "volume",
                                rapidjson::Value{taosim::util::decimal2double(
                                    feePolicy->agentVolume(bookId, agentId))},
                                allocator);
                            const auto rates = feePolicy->getRates(bookId, agentId);
                            feeJson.AddMember(
                                "makerFeeRate",
                                rapidjson::Value{taosim::util::decimal2double(rates.maker)},
                                allocator);
                            feeJson.AddMember(
                                "takerFeeRate",
                                rapidjson::Value{taosim::util::decimal2double(rates.taker)},
                                allocator);
                        });
                }
                json[agentIdCStr].AddMember("fees", feesJson, allocator);
            }
            for (const auto book : m_books) {
                const BookId bookId = book->id();
                const auto bookIdStr = std::to_string(bookId);
                for (const auto& [agentId, holdings] : accounts()) {
                    const auto agentIdStr = std::to_string(agentId);
                    const char* agentIdCStr = agentIdStr.c_str();
                    json[agentIdCStr]["orders"].PushBack(
                        rapidjson::Document{rapidjson::kArrayType, &allocator},
                        allocator);
                }
                for (const taosim::book::TickContainer& bidLevel : book->buyQueue()) {
                    for (const auto& bid : bidLevel) {
                        const auto [agentId, clientOrderId] = m_books[bookId]->orderClientContext(bid->id());
                        const auto agentIdStr = std::to_string(agentId);
                        const char* agentIdCStr = agentIdStr.c_str();
                        rapidjson::Document orderJson{&allocator};
                        bid->jsonSerialize(orderJson);
                        taosim::json::setOptionalMember(orderJson, "clientOrderId", clientOrderId);
                        json[agentIdCStr]["orders"][bookId].PushBack(orderJson, allocator);
                    }
                }
                for (const taosim::book::TickContainer& askLevel : book->sellQueue()) {
                    for (const auto& ask : askLevel) {
                        const auto [agentId, clientOrderId] = m_books[bookId]->orderClientContext(ask->id());
                        const auto agentIdStr = std::to_string(agentId);
                        const char* agentIdCStr = agentIdStr.c_str();
                        rapidjson::Document orderJson{&allocator};
                        ask->jsonSerialize(orderJson);
                        taosim::json::setOptionalMember(orderJson, "clientOrderId", clientOrderId);
                        json[agentIdCStr]["orders"][bookId].PushBack(orderJson, allocator);
                    }
                }
            }
        };
        taosim::json::serializeHelper(json, "accounts", serializeAccounts);
    };
    taosim::json::serializeHelper(json, key, serialize);
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleException()
{
    try {
        throw;
    }
    catch (const std::exception& e) {
        fmt::println("{}", e.what());
        throw;
    }
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleDistributedMessage(Message::Ptr msg)
{
    if (msg->type.contains("PLACE_ORDER_MARKET")) {
        handleDistributedPlaceMarketOrder(msg);
    }
    else if (msg->type.ends_with("PLACE_ORDER_LIMIT")) {
        handleDistributedPlaceLimitOrder(msg);
    }
    else if (msg->type.ends_with("RETRIEVE_ORDERS")) {
        handleDistributedRetrieveOrders(msg);
    }
    else if (msg->type.ends_with("CANCEL_ORDERS")) {
        handleDistributedCancelOrders(msg);
    }
    else if (msg->type.ends_with("CLOSE_POSITIONS")) {
        handleDistributedClosePositions(msg);
    }
    else if (msg->type.ends_with("RESET_AGENT")) {
        handleDistributedAgentReset(msg);
    }
    else {
        handleDistributedUnknownMessage(msg);
    }
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleDistributedAgentReset(Message::Ptr msg)
{
    const auto payload = std::dynamic_pointer_cast<DistributedAgentResponsePayload>(msg->payload);
    const auto subPayload = std::dynamic_pointer_cast<ResetAgentsPayload>(payload->payload);

    if (m_replayLog) {
        for (const auto& logger : m_replayEventLoggers) {
            logger->log(msg);
        }
    }

    std::vector<AgentId> valid = {};
    for (AgentId agentId : subPayload->agentIds) {
        if (accounts().contains(agentId)) {
            valid.push_back(agentId);
        } else {
            simulation()->logDebug("{} | RESET AGENTS : AGENT #{} NOT FOUND IN ACCOUNTS.", simulation()->currentTimestamp(), agentId);
        }
    }

    if (valid.empty()) {
        return;
    }

    if (m_replayMode) {
        for (auto agentId : valid) {
            auto& acct = accounts().at(agentId);
            for (auto book : m_books) {
                const auto& activeOrders = acct.activeOrders().at(book->id());
                for (Order::Ptr order : activeOrders) {
                    auto limitOrder = std::dynamic_pointer_cast<LimitOrder>(order);
                    if (limitOrder == nullptr) {
                        throw taosim::simulation::replay_helpers::ReplayError{};
                    }
                    auto cancelRes = book->cancelOrderOpt(limitOrder->id());
                    if (!cancelRes) {
                        throw taosim::simulation::replay_helpers::ReplayError{};
                    }
                    const taosim::event::Cancellation cancellation{limitOrder->id()};
                    m_signals.at(book->id())->cancelLog(CancellationWithLogContext(
                        cancellation,
                        std::make_shared<CancellationLogContext>(
                            agentId,
                            book->id(),
                            simulation()->currentTimestamp())));
                }
            }
        }
        rapidjson::Document json;
        const auto bookIdRange = std::pair{
            simulation()->blockIdx() * m_books.size(),
            (simulation()->blockIdx() + 1) * m_books.size() - 1
        };
        const auto balsPath =
            fs::path{boost::erase_last_copy(simulation()->logDir().string(), "-replay")}
                / fmt::format(
                    "Replay-Balances-{}-{}-{}.json",
                    bookIdRange.first,
                    bookIdRange.second,
                    simulation()->currentTimestamp());
        json = taosim::json::loadJson(balsPath);
        for (const auto& member : json.GetObject()) {
            const auto agentId = std::stoi(member.name.GetString());
            auto& acct = accounts().at(agentId);
            const auto& balsJson = member.value;
            for (const auto& balsMember : balsJson.GetObject()) {
                const auto bookIdCanon = std::stoi(balsMember.name.GetString());
                if (!(bookIdRange.first <= bookIdCanon && bookIdCanon <= bookIdRange.second)) {
                    continue;
                }
                const auto bookId = bookIdCanon % m_books.size();
                acct.at(bookId) = taosim::accounting::Balances(taosim::accounting::BalancesDesc{
                    .base = taosim::accounting::Balance{taosim::json::getDecimal(balsMember.value["base"])},
                    .quote = taosim::accounting::Balance{taosim::json::getDecimal(balsMember.value["quote"])},
                    .roundParams = acct.at(bookId).m_roundParams
                });
            }
        }
        const std::unordered_set<AgentId> resetAgentIds{valid.begin(), valid.end()};
        m_clearingManager->feePolicy()->resetHistory(resetAgentIds);
        return;
    }

    std::vector<std::vector<taosim::event::Cancellation>> cancellations;
    for (AgentId agentId : valid) {
        simulation()->logDebug("{} | AGENT #{} : RESET-CANCELS", simulation()->currentTimestamp(), agentId);
        for (BookId bookId = 0; bookId < m_books.size(); ++bookId) {
            simulation()->logDebug("{} | AGENT #{} BOOK {} : RESET-CANCELS", simulation()->currentTimestamp(), agentId, simulation()->bookIdCanon(bookId));
            std::vector<taosim::event::Cancellation> bookCancellations;
            const auto orders = accounts()[agentId].activeOrders()[bookId];
            const auto book = m_books.at(bookId);
            for (Order::Ptr order : orders) {
                if (auto limitOrder = std::dynamic_pointer_cast<LimitOrder>(order)) {
                    simulation()->logDebug("{} | AGENT #{} BOOK {} : START RESET-CANCEL OF ORDER {}", simulation()->currentTimestamp(), agentId, simulation()->bookIdCanon(bookId), limitOrder->id());
                    if (book->cancelOrderOpt(limitOrder->id())) {
                        const taosim::event::Cancellation cancellation{limitOrder->id()};
                        bookCancellations.push_back(cancellation);
                        m_signals.at(bookId)->cancelLog(CancellationWithLogContext(
                            cancellation,
                            std::make_shared<CancellationLogContext>(
                                agentId,
                                bookId,
                                simulation()->currentTimestamp())));
                        simulation()->logDebug("{} | AGENT #{} BOOK {} : END RESET-CANCEL OF ORDER {}", simulation()->currentTimestamp(), agentId, simulation()->bookIdCanon(bookId), limitOrder->id());
                    } else {
                        simulation()->logDebug("{} | AGENT #{} BOOK {} : RESET-CANCEL OF ORDER {} FAILED", simulation()->currentTimestamp(), agentId, simulation()->bookIdCanon(bookId), limitOrder->id());
                    }
                }
            }
            cancellations.push_back(std::move(bookCancellations));
        }
        accounts().reset(agentId);
        simulation()->logDebug("{} | AGENT #{} : RESET-CANCELS DONE", simulation()->currentTimestamp(), agentId);
    }
    simulation()->logDebug("{} | ALL RESET-CANCELS DONE", simulation()->currentTimestamp());

    const std::unordered_set<AgentId> resetAgentIds{valid.begin(), valid.end()};

    m_clearingManager->feePolicy()->resetHistory(resetAgentIds);

    if (m_replayMode) return;

    if (m_replayLog) {
        rapidjson::Document json{rapidjson::kObjectType};
        auto& allocator = json.GetAllocator();
        for (auto agentId : valid) {
            const auto& acct = accounts().at(agentId);
            json.AddMember(
                rapidjson::Value{std::to_string(agentId).c_str(), allocator},
                [&] {
                    rapidjson::Value balancesJson{rapidjson::kObjectType};
                    for (const auto& [bookId, bals] : views::enumerate(acct)) {
                        rapidjson::Value balanceJson{rapidjson::kObjectType};
                        balanceJson.AddMember(
                            "base",
                            rapidjson::Value{taosim::util::packDecimal(bals.base.getTotal())},
                            allocator);
                        balanceJson.AddMember(
                            "quote",
                            rapidjson::Value{taosim::util::packDecimal(bals.quote.getTotal())},
                            allocator);
                        balancesJson.AddMember(
                            rapidjson::Value{
                                std::to_string(simulation()->bookIdCanon(bookId)).c_str(), allocator},
                            balanceJson,
                            allocator);
                    }
                    return balancesJson;
                }().Move(),
                allocator);
        }
        std::ofstream ofs{
            simulation()->logDir() / fmt::format(
                "Replay-Balances-{}-{}-{}.json",
                simulation()->blockIdx() * m_books.size(),
                (simulation()->blockIdx() + 1) * m_books.size() - 1,
                simulation()->currentTimestamp())};
        taosim::json::dumpJson(
            json, ofs, taosim::json::FormatOptions{.indent = taosim::json::IndentOptions{}});
    }

    simulation()->m_messageQueue = MessageQueue{
        simulation()->m_messageQueue.m_queue.underlying()
            | views::filter([&](const auto& prioMsgWithId) {
                const auto distributedPayload =
                    std::dynamic_pointer_cast<DistributedAgentResponsePayload>(
                        prioMsgWithId.pmsg.msg->payload);
                return !(distributedPayload && resetAgentIds.contains(distributedPayload->agentId));
            })
            | ranges::to<std::vector>};
    simulation()->logDebug("{} | MESSAGE QUEUE CLEARED", simulation()->currentTimestamp());

    simulation()->fastRespondToMessage(
        msg,
        MessagePayload::create<DistributedAgentResponsePayload>(
            payload->agentId,
            MessagePayload::create<ResetAgentsResponsePayload>(valid, subPayload)));
    simulation()->logDebug("{} | RESET COMPLETE", simulation()->currentTimestamp());
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleDistributedPlaceMarketOrder(Message::Ptr msg)
{
    const auto payload = std::dynamic_pointer_cast<DistributedAgentResponsePayload>(msg->payload);
    const auto subPayload = std::dynamic_pointer_cast<PlaceOrderMarketPayload>(payload->payload);

    const bool isMarginCall = msg->type.ends_with("_MC");
    if (isMarginCall) {
        msg->type = "DISTRIBUTED_PLACE_MARKET_ORDER";
    }
    if (m_replayLog && !isMarginCall) {
        m_replayEventLoggers.at(subPayload->bookId)->log(msg);
    }

    if (simulation()->debug()) {
        const auto& balances = simulation()->exchange()->accounts()[payload->agentId][subPayload->bookId];
        simulation()->logDebug("{} | AGENT #{} BOOK {} : QUOTE : {}  BASE : {}", simulation()->currentTimestamp(), payload->agentId, simulation()->bookIdCanon(subPayload->bookId), balances.quote, balances.base);
    }
    const auto orderResult = m_clearingManager->handleOrder(
        taosim::exchange::MarketOrderDesc{
            .agentId = payload->agentId,
            .payload = subPayload
        });
    if (simulation()->debug()) {
        const auto& balances = simulation()->exchange()->accounts()[payload->agentId][subPayload->bookId];
        simulation()->logDebug("{} | AGENT #{} BOOK {} : QUOTE : {}  BASE : {}", simulation()->currentTimestamp(), payload->agentId, simulation()->bookIdCanon(subPayload->bookId), balances.quote, balances.base);
    }

    if (orderResult.ec != OrderErrorCode::VALID) {
        simulation()->logDebug(
            "Invalid Market Order Placement by Distributed Agent - {} : {}",
            orderResult.ec,
            taosim::json::jsonSerializable2str(payload));
        if (m_replayMode && !simulation()->isReplacedAgent(msg->source)) return;
        return fastRespondToMessage(
            msg,
            "ERROR",
            MessagePayload::create<DistributedAgentResponsePayload>(
                payload->agentId,
                MessagePayload::create<PlaceOrderMarketErrorResponsePayload>(
                    subPayload,
                    MessagePayload::create<ErrorResponsePayload>(
                        OrderErrorCode2StrView(orderResult.ec).data()))));
    }

    const auto order = m_books[subPayload->bookId]->placeMarketOrder(
        subPayload->direction,
        msg->arrival,
        orderResult.orderSize,
        subPayload->leverage,
        OrderClientContext{payload->agentId, subPayload->clientOrderId},
        subPayload->stpFlag,
        subPayload->settleFlag,
        subPayload->currency
    );

    if (m_replayMode && !simulation()->isReplacedAgent(msg->source)) return;

    const auto retSubPayload =
        MessagePayload::create<PlaceOrderMarketResponsePayload>(order->id(), subPayload);

    respondToMessage(
        msg,
        MessagePayload::create<DistributedAgentResponsePayload>(
            payload->agentId,
            MessagePayload::create<PlaceOrderMarketResponsePayload>(order->id(), subPayload)),
        0);
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleDistributedPlaceLimitOrder(Message::Ptr msg)
{
    const auto payload = std::dynamic_pointer_cast<DistributedAgentResponsePayload>(msg->payload);
    const auto subPayload = std::dynamic_pointer_cast<PlaceOrderLimitPayload>(payload->payload);

    if (m_replayLog) {
        m_replayEventLoggers.at(subPayload->bookId)->log(msg);
    }

    if (simulation()->debug()) {
        const auto& balances = simulation()->exchange()->accounts()[payload->agentId][subPayload->bookId];
        simulation()->logDebug("{} | AGENT #{} BOOK {} : QUOTE : {}  BASE : {}", simulation()->currentTimestamp(), payload->agentId, simulation()->bookIdCanon(subPayload->bookId), balances.quote, balances.base);
    }
    const auto orderResult = m_clearingManager->handleOrder(
        taosim::exchange::LimitOrderDesc{
            .agentId = payload->agentId,
            .payload = subPayload
        });
    if (simulation()->debug()) {
        const auto& balances = simulation()->exchange()->accounts()[payload->agentId][subPayload->bookId];
        simulation()->logDebug("{} | AGENT #{} BOOK {} : QUOTE : {}  BASE : {}", simulation()->currentTimestamp(), payload->agentId, simulation()->bookIdCanon(subPayload->bookId), balances.quote, balances.base);
    }

    if (orderResult.ec != OrderErrorCode::VALID) {
        simulation()->logDebug(
            "Invalid Limit Order Placement by Distributed Agent - {} : {}",
            orderResult.ec,
            taosim::json::jsonSerializable2str(payload));
        if (m_replayMode  && !simulation()->isReplacedAgent(msg->source)) return;
        return fastRespondToMessage(
            msg,
            "ERROR",
            MessagePayload::create<DistributedAgentResponsePayload>(
                payload->agentId,
                MessagePayload::create<PlaceOrderLimitErrorResponsePayload>(
                    subPayload,
                    MessagePayload::create<ErrorResponsePayload>(
                        OrderErrorCode2StrView(orderResult.ec).data()))));
    }

    const auto order = m_books[subPayload->bookId]->placeLimitOrder(
        subPayload->direction,
        msg->arrival,
        orderResult.orderSize,
        subPayload->price,
        subPayload->leverage,
        OrderClientContext{payload->agentId, subPayload->clientOrderId},
        subPayload->stpFlag,
        subPayload->settleFlag,
        subPayload->postOnly,
        subPayload->timeInForce,
        subPayload->expiryPeriod,
        subPayload->currency
    );

    if (m_replayMode && !simulation()->isReplacedAgent(msg->source)) return;

    const auto retSubPayload =
        MessagePayload::create<PlaceOrderLimitResponsePayload>(order->id(), subPayload);

    respondToMessage(
        msg,
        MessagePayload::create<DistributedAgentResponsePayload>(
            payload->agentId,
            MessagePayload::create<PlaceOrderLimitResponsePayload>(order->id(), subPayload)),
        0);

    if (subPayload->timeInForce == taosim::TimeInForce::GTT && subPayload->expiryPeriod.has_value()) {
        simulation()->dispatchMessage(
            simulation()->currentTimestamp(),
            subPayload->expiryPeriod.value(),
            msg->source,
            name(),
            "DISTRIBUTED_CANCEL_ORDERS",
            MessagePayload::create<DistributedAgentResponsePayload>(
                payload->agentId,
                MessagePayload::create<CancelOrdersPayload>(
                    std::vector{taosim::event::Cancellation{order->id()}}, subPayload->bookId)));
    }
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleDistributedRetrieveOrders(Message::Ptr msg)
{
    const auto payload = std::dynamic_pointer_cast<DistributedAgentResponsePayload>(msg->payload);
    const auto subPayload = std::dynamic_pointer_cast<RetrieveOrdersPayload>(payload->payload);

    const auto book = m_books[subPayload->bookId];

    auto retSubPayload = MessagePayload::create<RetrieveOrdersResponsePayload>();
    for (OrderID id : subPayload->ids) {
        if (LimitOrder::Ptr order; book->tryGetOrder(id, order)) {
            retSubPayload->orders.push_back(*order);
        }
    }

    respondToMessage(
        msg,
        MessagePayload::create<DistributedAgentResponsePayload>(payload->agentId, retSubPayload),
        0);
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleDistributedCancelOrders(Message::Ptr msg)
{
    const auto payload = std::dynamic_pointer_cast<DistributedAgentResponsePayload>(msg->payload);
    const auto subPayload = std::dynamic_pointer_cast<CancelOrdersPayload>(payload->payload);

    if (m_replayLog) {
        m_replayEventLoggers.at(subPayload->bookId)->log(msg);
    }

    const auto bookId = subPayload->bookId;
    const auto book = m_books[bookId];

    std::vector<taosim::event::Cancellation> cancellations;
    std::vector<taosim::event::Cancellation> failures;
    for (const auto& cancellation : subPayload->cancellations) {        
        if (book->cancelOrderOpt(cancellation.id, cancellation.volume)) {
            cancellations.push_back(cancellation);
            m_signals[bookId]->cancelLog(CancellationWithLogContext(
                cancellation,
                std::make_shared<CancellationLogContext>(
                    payload->agentId, bookId, simulation()->currentTimestamp())));
        }
        else {
            failures.push_back(cancellation);
        }
    }

    if (m_replayMode && !simulation()->isReplacedAgent(msg->source)) return;

    if (!cancellations.empty()) {        
        std::vector<OrderID> orderIds;
        for (const auto& canc : cancellations) {
            orderIds.push_back(canc.id);
        }
        respondToMessage(
            msg,
            MessagePayload::create<DistributedAgentResponsePayload>(
                payload->agentId,
                MessagePayload::create<CancelOrdersResponsePayload>(
                    std::move(orderIds),
                    MessagePayload::create<CancelOrdersPayload>(
                        std::move(cancellations), bookId)))
            );
    }

    if (!failures.empty()) {
        std::vector<OrderID> orderIds = failures
            | views::transform([](const auto& c) { return c.id; })
            | ranges::to<std::vector>();
        auto errorMsg = fmt::format("Order IDs {} do not exist.", fmt::join(orderIds, ", "));
        auto retSubPayload = MessagePayload::create<CancelOrdersErrorResponsePayload>(
            std::move(orderIds),
            MessagePayload::create<CancelOrdersPayload>(std::move(failures), bookId), 
            MessagePayload::create<ErrorResponsePayload>(std::move(errorMsg))
        );
        respondToMessage(
            msg,
            "ERROR",
            MessagePayload::create<DistributedAgentResponsePayload>(
                payload->agentId,
                retSubPayload),
            0);
    }
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleDistributedClosePositions(Message::Ptr msg)
{
    const auto payload = std::dynamic_pointer_cast<DistributedAgentResponsePayload>(msg->payload);
    const auto subPayload = std::dynamic_pointer_cast<ClosePositionsPayload>(payload->payload);

    if (m_replayLog) {
        m_replayEventLoggers.at(subPayload->bookId)->log(msg);
    }

    const auto bookId = subPayload->bookId;
    const auto book = m_books[bookId];

    std::vector<ClosePosition> closes;
    std::vector<ClosePosition> failures;
    for (const auto& close : subPayload->closePositions) {        
        if (m_clearingManager->handleClosePosition(ClosePositionDesc{
                .bookId = bookId,
                .agentId = payload->agentId,
                .orderId = close.id,
                .volumeToClose = close.volume
            })
        ) {
            closes.push_back(close);
        }
        else {
            failures.push_back(close);
        }
    }

    if (m_replayMode && !simulation()->isReplacedAgent(msg->source)) return;

    if (!closes.empty()) {        
        std::vector<OrderID> orderIds;
        for (const ClosePosition& close : closes) {
            orderIds.push_back(close.id);
        }
        respondToMessage(
            msg,
            MessagePayload::create<DistributedAgentResponsePayload>(
                payload->agentId,
                MessagePayload::create<ClosePositionsResponsePayload>(
                    std::move(orderIds),
                    MessagePayload::create<ClosePositionsPayload>(
                        std::move(closes), bookId)))
            );
    }

    if (!failures.empty()) {
        std::vector<OrderID> orderIds = failures
            | views::transform([](const ClosePosition& c) { return c.id; })
            | ranges::to<std::vector>();
        auto errorMsg = fmt::format("Order IDs {} do not exist.", fmt::join(orderIds, ", "));
        auto retSubPayload = MessagePayload::create<ClosePositionsErrorResponsePayload>(
            std::move(orderIds),
            MessagePayload::create<ClosePositionsPayload>(std::move(failures), bookId), 
            MessagePayload::create<ErrorResponsePayload>(std::move(errorMsg))
        );
        respondToMessage(
            msg,
            "ERROR",
            MessagePayload::create<DistributedAgentResponsePayload>(
                payload->agentId,
                retSubPayload),
            0);
    }
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleDistributedUnknownMessage(Message::Ptr msg)
{
    const auto payload = std::dynamic_pointer_cast<DistributedAgentResponsePayload>(msg->payload);

    auto retSubPayload =
        MessagePayload::create<ErrorResponsePayload>(
            fmt::format("Unknown message type: {}", msg->type));
    fastRespondToMessage(
        msg,
        "ERROR",
        MessagePayload::create<DistributedAgentResponsePayload>(
            payload->agentId,
            retSubPayload));
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleLocalMessage(Message::Ptr msg)
{
    if (msg->type.starts_with("PLACE_ORDER_MARKET")) {
        handleLocalPlaceMarketOrder(msg);
    }
    else if (msg->type == "PLACE_ORDER_LIMIT") {
        handleLocalPlaceLimitOrder(msg);
    }
    else if (msg->type == "RETRIEVE_ORDERS") {
        handleLocalRetrieveOrders(msg);
    }
    else if (msg->type == "CANCEL_ORDERS") {
        handleLocalCancelOrders(msg);
    }
    else if (msg->type == "CLOSE_POSITIONS") {
        handleLocalClosePositions(msg);
    }
    else if (msg->type == "RETRIEVE_L1") {
        handleLocalRetrieveL1(msg);
    }
    else if (msg->type == "RETRIEVE_L2") {
        handleLocalRetrieveL2(msg);
    }
    else if (msg->type == "SUBSCRIBE_EVENT_ORDER_MARKET") {
        handleLocalMarketOrderSubscription(msg);
    }
    else if (msg->type == "SUBSCRIBE_EVENT_ORDER_LIMIT") {
        handleLocalLimitOrderSubscription(msg);
    }
    else if (msg->type == "SUBSCRIBE_EVENT_TRADE") {
        handleLocalTradeSubscription(msg);
    }
    else if (msg->type == "SUBSCRIBE_EVENT_ORDER_TRADE") {
        handleLocalTradeByOrderSubscription(msg);
    }
    else {
        handleLocalUnknownMessage(msg);
    }
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleLocalPlaceMarketOrder(Message::Ptr msg)
{
    const auto& payload = std::dynamic_pointer_cast<PlaceOrderMarketPayload>(msg->payload);

    const bool isMarginCall = msg->type.ends_with("_MC");
    if (isMarginCall) {
        msg->type = "PLACE_MARKET_ORDER";
    }
    if (m_replayLog && !isMarginCall) {
        m_replayEventLoggers.at(payload->bookId)->log(msg);
    }

    if (simulation()->debug()) {
        auto agentId = accounts().idBimap().left.at(msg->source);
        const auto& balances = simulation()->exchange()->accounts()[agentId][payload->bookId];
        simulation()->logDebug("{} | AGENT #{} BOOK {} : QUOTE : {}  BASE : {}", simulation()->currentTimestamp(), agentId, simulation()->bookIdCanon(payload->bookId), balances.quote, balances.base);
    }
    const auto orderResult = m_clearingManager->handleOrder(
        taosim::exchange::MarketOrderDesc{
            .agentId = msg->source,
            .payload = payload
        });
    if (simulation()->debug()) {
        auto agentId = accounts().idBimap().left.at(msg->source);
        const auto& balances = simulation()->exchange()->accounts()[agentId][payload->bookId];
        simulation()->logDebug("{} | AGENT #{} BOOK {} : QUOTE : {}  BASE : {}", simulation()->currentTimestamp(), agentId, simulation()->bookIdCanon(payload->bookId), balances.quote, balances.base);
    }
    
    if (orderResult.ec != OrderErrorCode::VALID) {
        simulation()->logDebug(
            "Invalid Market Order Placement by Local Agent - {} : {}",
            orderResult.ec,
            taosim::json::jsonSerializable2str(payload));
        if (m_replayMode && !simulation()->isReplacedAgent(msg->source)) return;
        return fastRespondToMessage(
            msg,
            "ERROR",
            MessagePayload::create<PlaceOrderMarketErrorResponsePayload>(
                payload,
                MessagePayload::create<ErrorResponsePayload>(
                    OrderErrorCode2StrView(orderResult.ec).data())));
    }

    const auto order = m_books[payload->bookId]->placeMarketOrder(
        payload->direction,
        msg->arrival,
        orderResult.orderSize,
        payload->leverage,
        OrderClientContext{accounts().idBimap().left.at(msg->source), payload->clientOrderId},
        payload->stpFlag,
        payload->settleFlag,
        payload->currency
    );

    notifyMarketOrderSubscribers(order);

    if (m_replayMode && !simulation()->isReplacedAgent(msg->source)) return;

    respondToMessage(
        msg,
        MessagePayload::create<PlaceOrderMarketResponsePayload>(order->id(), payload),
        1);
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleLocalPlaceLimitOrder(Message::Ptr msg)
{
    if (msg->source == "STYLIZED_TRADER_AGENT_134") {
        fmt::println("{}", taosim::json::jsonSerializable2str(msg));
    }

    const auto& payload = std::dynamic_pointer_cast<PlaceOrderLimitPayload>(msg->payload);

    if (m_replayLog) {
        m_replayEventLoggers.at(payload->bookId)->log(msg);
    }

    if (simulation()->debug()) {
        auto agentId = accounts().idBimap().left.at(msg->source);
        const auto& balances = simulation()->exchange()->accounts()[agentId][payload->bookId];
        simulation()->logDebug("{} | AGENT #{} BOOK {} : QUOTE : {}  BASE : {}", simulation()->currentTimestamp(), agentId, simulation()->bookIdCanon(payload->bookId), balances.quote, balances.base);
    }
    const auto orderResult = m_clearingManager->handleOrder(
        taosim::exchange::LimitOrderDesc{
            .agentId = msg->source,
            .payload = payload
        });
    if (simulation()->debug()) {
        auto agentId = accounts().idBimap().left.at(msg->source);
        const auto& balances = simulation()->exchange()->accounts()[agentId][payload->bookId];
        simulation()->logDebug("{} | AGENT #{} BOOK {} : QUOTE : {}  BASE : {}", simulation()->currentTimestamp(), agentId, simulation()->bookIdCanon(payload->bookId), balances.quote, balances.base);
    }

    if (orderResult.ec != OrderErrorCode::VALID) {
        simulation()->logDebug(
            "Invalid Limit Order Placement by Local Agent - {} : {}",
            orderResult.ec,
            taosim::json::jsonSerializable2str(payload));
        if (msg->source == "STYLIZED_TRADER_AGENT_134") {
            fmt::println("Invalid Limit Order Placement by Local Agent - {}", orderResult.ec);
            exit(1);
        }
        if (m_replayMode && !simulation()->isReplacedAgent(msg->source)) return;
        return fastRespondToMessage(
            msg,
            "ERROR",
            MessagePayload::create<PlaceOrderLimitErrorResponsePayload>(
                payload,
                MessagePayload::create<ErrorResponsePayload>(
                    OrderErrorCode2StrView(orderResult.ec).data())));
    }

    const auto order = m_books[payload->bookId]->placeLimitOrder(
        payload->direction,
        msg->arrival,
        orderResult.orderSize,
        payload->price,
        payload->leverage,
        OrderClientContext{accounts().idBimap().left.at(msg->source), payload->clientOrderId},
        payload->stpFlag,
        payload->settleFlag,
        payload->postOnly,
        payload->timeInForce,
        payload->expiryPeriod,
        payload->currency
    );

    notifyLimitOrderSubscribers(order);

    if (m_replayMode && !simulation()->isReplacedAgent(msg->source)) return;

    respondToMessage(
        msg,
        MessagePayload::create<PlaceOrderLimitResponsePayload>(order->id(), payload),
        1);

    if (payload->timeInForce == taosim::TimeInForce::GTT && payload->expiryPeriod.has_value()) {
        simulation()->dispatchMessage(
            simulation()->currentTimestamp(),
            payload->expiryPeriod.value(),
            msg->source,
            name(),
            "CANCEL_ORDERS",
            MessagePayload::create<CancelOrdersPayload>(
                std::vector{taosim::event::Cancellation{order->id()}}, payload->bookId));
    }
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleLocalRetrieveOrders(Message::Ptr msg)
{
    const auto payload = std::dynamic_pointer_cast<RetrieveOrdersPayload>(msg->payload);

    const auto book = m_books[payload->bookId];

    respondToMessage(
        msg,
        MessagePayload::create<RetrieveOrdersResponsePayload>(
            payload->ids
                | views::transform([&](OrderID id) { return book->getOrder(id); })
                | views::filter([](std::optional<LimitOrder::Ptr> order) { return order.has_value(); })
                | views::transform([](std::optional<LimitOrder::Ptr> order) { return **order; })
                | ranges::to<std::vector>(),
            payload->bookId));
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleLocalCancelOrders(Message::Ptr msg)
{
    const auto& payload = std::dynamic_pointer_cast<CancelOrdersPayload>(msg->payload);

    if (m_replayLog) {
        m_replayEventLoggers.at(payload->bookId)->log(msg);
    }

    const auto bookId = payload->bookId;
    const auto book = m_books[bookId];

    std::vector<taosim::event::Cancellation> cancellations;
    std::vector<taosim::event::Cancellation> failures;
    for (auto& cancellation : payload->cancellations) {
        if (cancellation.volume) {
            cancellation.volume = taosim::util::round(
                cancellation.volume.value(), m_config.parameters().volumeIncrementDecimals);
        }
        if (book->cancelOrderOpt(cancellation.id, cancellation.volume)) {
            cancellations.push_back(cancellation);
            m_signals.at(bookId)->cancelLog(CancellationWithLogContext(
                cancellation,
                std::make_shared<CancellationLogContext>(
                    accounts().idBimap().left.at(msg->source),
                    bookId,
                    simulation()->currentTimestamp())));
        }
        else {
            failures.push_back(cancellation);
        }
    }

    if (m_replayMode && !simulation()->isReplacedAgent(msg->source)) return;

    if (!cancellations.empty()) {
        respondToMessage(
            msg,
            MessagePayload::create<CancelOrdersResponsePayload>(
                cancellations
                    | views::transform([](const auto& c) { return c.id; })
                    | ranges::to<std::vector>(),
                MessagePayload::create<CancelOrdersPayload>(
                    std::move(cancellations), payload->bookId)),
            0);
    }

    if (!failures.empty()) {
        std::vector<OrderID> orderIds = failures
            | views::transform([](const auto& c) { return c.id; })
            | ranges::to<std::vector>();
        auto errorMsg = fmt::format("Order IDs {} do not exist.", fmt::join(orderIds, ", "));
        auto retSubPayload = MessagePayload::create<CancelOrdersErrorResponsePayload>(
            std::move(orderIds),
            payload,
            MessagePayload::create<ErrorResponsePayload>(std::move(errorMsg))
        );
        respondToMessage(msg, "ERROR", retSubPayload);
    }
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleLocalClosePositions(Message::Ptr msg)
{
    const auto& payload = std::dynamic_pointer_cast<ClosePositionsPayload>(msg->payload);

    if (m_replayLog) {
        m_replayEventLoggers.at(payload->bookId)->log(msg);
    }

    const auto bookId = payload->bookId;
    const auto book = m_books[bookId];
    const auto agentId = accounts().idBimap().left.at(msg->source);

    std::vector<ClosePosition> closes;
    std::vector<ClosePosition> failures;
    for (const auto& close : payload->closePositions) {
        const auto res = m_clearingManager->handleClosePosition(ClosePositionDesc{
            .bookId = bookId,
            .agentId = agentId,
            .orderId = close.id,
            .volumeToClose = close.volume
        });
        if (res) {
            closes.push_back(close);
        } else {
            failures.push_back(close);
        }
    }

    if (m_replayMode && !simulation()->isReplacedAgent(msg->source)) return;

    if (!closes.empty()) {
        respondToMessage(
            msg,
            MessagePayload::create<ClosePositionsResponsePayload>(
                closes
                    | views::transform([](const ClosePosition& c) { return c.id; })
                    | ranges::to<std::vector>(),
                MessagePayload::create<ClosePositionsPayload>(
                    std::move(closes), bookId)),
            0);
    }

    if (!failures.empty()) {
        std::vector<OrderID> orderIds = failures
            | views::transform([](const ClosePosition& c) { return c.id; })
            | ranges::to<std::vector>();
        auto errorMsg = fmt::format("Order IDs {} do not exist.", fmt::join(orderIds, ", "));
        auto retSubPayload = MessagePayload::create<ClosePositionsErrorResponsePayload>(
            std::move(orderIds),
            payload,
            MessagePayload::create<ErrorResponsePayload>(std::move(errorMsg))
        );
        respondToMessage(msg, "ERROR", retSubPayload);
    }
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleLocalRetrieveL1(Message::Ptr msg)
{
    const auto payload = std::dynamic_pointer_cast<RetrieveL1Payload>(msg->payload);

    const auto book = m_books[payload->bookId];

    taosim::decimal_t bestAskPrice{};
    taosim::decimal_t bestAskVolume{}, askTotalVolume{};
    taosim::decimal_t bestBidPrice{};
    taosim::decimal_t bestBidVolume{}, bidTotalVolume{};

    if (!book->sellQueue().empty()) {
        const auto& bestSellLevel = book->sellQueue().front();
        bestAskPrice = bestSellLevel.price();
        bestAskVolume = bestSellLevel.volume();
        askTotalVolume = book->sellQueue().volume();
    }

    if (!book->buyQueue().empty()) {
        const auto& bestBuyLevel = book->buyQueue().back();
        bestBidPrice = bestBuyLevel.price();
        bestBidVolume = bestBuyLevel.volume();
        bidTotalVolume = book->buyQueue().volume();
    }

    simulation()->dispatchMessage(
        simulation()->currentTimestamp(),
        1,
        name(),
        msg->source,
        "RESPONSE_RETRIEVE_L1",
        MessagePayload::create<RetrieveL1ResponsePayload>(
            simulation()->currentTimestamp(),
            bestAskPrice,
            bestAskVolume,
            askTotalVolume,
            bestBidPrice,
            bestBidVolume,
            bidTotalVolume,
            payload->bookId));
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleLocalRetrieveL2(Message::Ptr msg)
{
    const auto payload = std::dynamic_pointer_cast<RetrieveL2Payload>(msg->payload);

    const auto book = m_books[payload->bookId];

    respondToMessage(
        msg,
        MessagePayload::create<RetrieveL2ResponsePayload>(
            simulation()->currentTimestamp(),
            book->buyQueue()
                | views::reverse
                | views::take(payload->depth)
                | views::transform([](const auto& level) -> BookLevel {
                    return {
                        .price = level.price(),
                        .quantity = level.volume()
                    };
                })
                | ranges::to<std::vector>,
            book->sellQueue()
                | views::take(payload->depth)
                | views::transform([](const auto& level) -> BookLevel {
                    return {
                        .price = level.price(),
                        .quantity = level.volume()
                    };
                })
                | ranges::to<std::vector>,
            book->id()));
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleLocalMarketOrderSubscription(Message::Ptr msg)
{
    const auto& sub = msg->source;

    if (!m_localMarketOrderSubscribers.add(sub)) {
        return fastRespondToMessage(
            msg,
            "ERROR",
            MessagePayload::create<ErrorResponsePayload>(
                fmt::format("Agent {} is already subscribed to market order events", sub)));
    }

    fastRespondToMessage(
        msg,
        MessagePayload::create<SuccessResponsePayload>(
            fmt::format("Agent {} subscribed successfully to market order events", sub)));
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleLocalLimitOrderSubscription(Message::Ptr msg)
{
    const auto& sub = msg->source;

    if (!m_localLimitOrderSubscribers.add(sub)) {
        return fastRespondToMessage(
            msg,
            "ERROR",
            MessagePayload::create<ErrorResponsePayload>(
                fmt::format("Agent {} is already subscribed to limit order events", sub)));
    }

    fastRespondToMessage(
        msg,
        MessagePayload::create<SuccessResponsePayload>(
            fmt::format("Agent {} subscribed successfully to limit order events", sub)));
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleLocalTradeSubscription(Message::Ptr msg)
{
    const auto& sub = msg->source;

    if (!m_localTradeSubscribers.add(sub)) {
        return fastRespondToMessage(
            msg,
            "ERROR",
            MessagePayload::create<ErrorResponsePayload>(
                fmt::format("Agent {} is already subscribed to trade events", sub)));
    }

    fastRespondToMessage(
        msg,
        MessagePayload::create<SuccessResponsePayload>(
            fmt::format("Agent {} subscribed successfully to trade events", sub)));
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleLocalTradeByOrderSubscription(Message::Ptr msg)
{
    const auto& sub = msg->source;
    auto pptr = std::dynamic_pointer_cast<SubscribeEventTradeByOrderPayload>(msg->payload);
    const auto orderId = pptr->id;

    if (!m_localTradeByOrderSubscribers[orderId].add(sub)) {
        return fastRespondToMessage(
            msg,
            "ERROR",
            MessagePayload::create<ErrorResponsePayload>(fmt::format(
                "Agent {} is already subscribed to trade events for order {}", sub, orderId)));
    }

    fastRespondToMessage(
        msg,
        MessagePayload::create<SuccessResponsePayload>(fmt::format(
            "Agent {} subscribed successfully to trade events for order {}", sub, orderId)));
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::handleLocalUnknownMessage(Message::Ptr msg)
{
    fastRespondToMessage(
        msg,
        "ERROR",
        MessagePayload::create<ErrorResponsePayload>(fmt::format(
            "Unknown message type: {}",
            msg->type)));
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::notifyMarketOrderSubscribers(MarketOrder::Ptr marketOrder)
{
    const Timestamp now = simulation()->currentTimestamp();

    auto subs = m_localMarketOrderSubscribers
        | views::filter([&](auto&& sub) {
            if (!m_replayMode) return true;
            return simulation()->isReplacedAgent(sub);
        });

    for (const auto& sub : subs) {
        simulation()->dispatchMessage(
            now,
            1,
            name(),
            sub,
            "EVENT_ORDER_MARKET",
            MessagePayload::create<EventOrderMarketPayload>(*marketOrder));
    }
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::notifyLimitOrderSubscribers(LimitOrder::Ptr limitOrder)
{
    const Timestamp now = simulation()->currentTimestamp();

    auto subs = m_localLimitOrderSubscribers
        | views::filter([&](auto&& sub) {
            if (!m_replayMode) return true;
            return simulation()->isReplacedAgent(sub);
        });

    for (const auto& sub : subs) {
        simulation()->dispatchMessage(
            now,
            1,
            name(),
            sub,
            "EVENT_ORDER_LIMIT",
            MessagePayload::create<EventOrderLimitPayload>(*limitOrder));
    }
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::notifyTradeSubscribers(TradeWithLogContext::Ptr tradeWithCtx)
{
    const Timestamp now = simulation()->currentTimestamp();
    // The trade happens exactly on the receipt of the aggressing order, no processing
    // delay there; the processing delay only kicks in sending out a response and events
    // related to the matching.
    tradeWithCtx->trade->setTimestamp(now);

    auto subs = m_localTradeSubscribers
        | views::filter([&](auto&& sub) {
            if (!m_replayMode) return true;
            return simulation()->isReplacedAgent(sub);
        });

    for (const auto& sub : subs) {
        simulation()->dispatchMessage(
            now,
            Timestamp{},
            name(),
            sub,
            "EVENT_TRADE",
            MessagePayload::create<EventTradePayload>(
                *(tradeWithCtx->trade),
                *(tradeWithCtx->logContext),
                tradeWithCtx->logContext->bookId));
    }

    notifyTradeSubscribersByOrderID(tradeWithCtx, tradeWithCtx->trade->aggressingOrderID());
    notifyTradeSubscribersByOrderID(tradeWithCtx, tradeWithCtx->trade->restingOrderID());
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::notifyTradeSubscribersByOrderID(
    TradeWithLogContext::Ptr tradeWithCtx, OrderID orderId)
{
    auto it = m_localTradeByOrderSubscribers.find(orderId);
    if (it == m_localTradeByOrderSubscribers.end()) return;

    auto subs = it->second
        | views::filter([&](auto&& sub) {
            if (!m_replayMode) return true;
            return simulation()->isReplacedAgent(sub);
        });

    const Timestamp now = simulation()->currentTimestamp();

    for (const auto& sub : subs) {
        simulation()->dispatchMessage(
            now,
            1,
            name(),
            sub,
            "EVENT_TRADE",
            MessagePayload::create<EventTradePayload>(
                *(tradeWithCtx->trade),
                *(tradeWithCtx->logContext),
                tradeWithCtx->logContext->bookId));
    }
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::orderCallback(Order::Ptr order, OrderContext ctx)
{
    accounts()[ctx.agentId].activeOrders()[ctx.bookId].insert(order);
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::orderLogCallback(Order::Ptr order, OrderContext ctx)
{
    if (order->totalVolume() == 0_dec) return;
    m_L3Record.at(ctx.bookId).push(taosim::event::OrderEvent(order, ctx));
    m_signals.at(ctx.bookId)->orderLog(OrderWithLogContext(
        order, std::make_shared<OrderLogContext>(ctx.agentId, ctx.bookId)));
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::instructionLogCallback(const taosim::exchange::OrderDesc& orderDesc, OrderID orderId)
{
    std::visit([&](auto&& desc) {
        const auto& agentId = desc.agentId;
        const auto& payload = desc.payload;

        m_signals[payload->bookId]->instructionLog(
            taosim::InstructionLogContext(
                std::visit([this](auto&& agId) -> AgentId {
                    if constexpr (std::same_as<std::remove_cvref_t<decltype(agId)>, LocalAgentId>) {
                        return accounts().idBimap().left.at(agId);
                    } else {
                        return agId;
                    }
                }, agentId),
                orderId,
                payload));

    }, orderDesc);
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::tradeCallback(Trade::Ptr trade, BookId bookId)
{
    const auto restingOrderId = trade->restingOrderID();
    const auto aggressingOrderId = trade->aggressingOrderID();

    const auto [restingAgentId, restingClientOrderId] =
        m_books[bookId]->orderClientContext(restingOrderId);
    const auto [aggressingAgentId, aggressingClientOrderId] =
        m_books[bookId]->orderClientContext(aggressingOrderId);

    const auto& fees = m_clearingManager->handleTrade(taosim::exchange::TradeDesc{
        .bookId = bookId,
        .restingAgentId = restingAgentId,
        .aggressingAgentId = aggressingAgentId,
        .trade = trade
    });

    m_L3Record.at(bookId).push(taosim::event::TradeEvent(
        trade, TradeContext(bookId, aggressingAgentId, restingAgentId, fees)));

    auto tradeWithCtx = std::make_shared<TradeWithLogContext>(
        trade,
        std::make_shared<TradeLogContext>(aggressingAgentId, restingAgentId, bookId, fees));

    if (!m_replayMode) {
        const Timestamp now = simulation()->currentTimestamp();
        const std::array<std::pair<AgentId, std::optional<ClientOrderID>>, 2> idPairs{
            std::pair{restingAgentId, restingClientOrderId},
            std::pair{aggressingAgentId, aggressingClientOrderId}};
        for (const auto [agentId, clientOrderId] : idPairs) {
            const bool isLocalAgent = agentId < AgentId{};
            if (isLocalAgent) continue;
            simulation()->dispatchMessage(
                now,
                Timestamp{},
                name(),
                "DISTRIBUTED_PROXY_AGENT",
                "EVENT_TRADE",
                MessagePayload::create<DistributedAgentResponsePayload>(
                    agentId,
                    MessagePayload::create<EventTradePayload>(
                        *trade, *tradeWithCtx->logContext, bookId, clientOrderId)));
        }
    }

    m_signals[bookId]->tradeLog(*tradeWithCtx);
    m_signals[bookId]->feeLog(
        m_clearingManager->feePolicy(), 
        taosim::FeeLogEvent{
            .bookId = bookId,
            .restingAgentId = restingAgentId,
            .aggressingAgentId = aggressingAgentId,
            .fees = fees,
            .price = trade->price(),
            .volume = trade->volume()
        }   
    );

    if (m_replayMode) return;

    notifyTradeSubscribers(tradeWithCtx);
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::unregisterLimitOrderCallback(LimitOrder::Ptr limitOrder, BookId bookId)
{
    const OrderID orderId = limitOrder->id();
    const AgentId agentId = m_books[bookId]->orderClientContext(orderId).agentId;

    taosim::accounting::Balances& balances = accounts().at(agentId).at(bookId);
    const auto freed = [&] -> taosim::accounting::ReservationAmounts {
        if (balances.canFree(orderId)){
            if (limitOrder->direction() == OrderDirection::BUY) {
                simulation()->logDebug("FREEING RESERVATION OF {} BASE + {} QUOTE for BUY order #{}", 
                    balances.base.getReservation(orderId).value_or(0_dec), balances.quote.getReservation(orderId).value_or(0_dec), orderId);
            } else {
                simulation()->logDebug("FREEING RESERVATION OF {} BASE + {} QUOTE for SELL order #{}", 
                    balances.base.getReservation(orderId).value_or(0_dec), balances.quote.getReservation(orderId).value_or(0_dec), orderId);
            }
            return balances.freeReservation(orderId, limitOrder->price(),
                    m_books[bookId]->bestBid(), m_books[bookId]->bestAsk(), limitOrder->direction(), 
                    simulation()->bookIdCanon(bookId));
        }
        return {};
    }();

    accounts()[agentId].activeOrders()[bookId].erase(limitOrder);

    if (limitOrder->volume() > 0_dec) {
        simulation()->logDebug(
            "{} | AGENT #{} BOOK {} : UNREGISTERED {} ORDER #{} ({}@{}) (FREED {} BASE + {} QUOTE) | RESERVED_QUOTE={} | RESERVED_BASE={}",
            simulation()->currentTimestamp(),
            agentId,
            simulation()->bookIdCanon(bookId),
            limitOrder->direction() == OrderDirection::BUY ? "BUY" : "SELL",
            orderId,
            limitOrder->leverage() > 0_dec ? fmt::format("{}x{}",1_dec + limitOrder->leverage(),limitOrder->volume()) : fmt::format("{}",limitOrder->volume()),
            limitOrder->price(),
            freed.base, freed.quote,
            balances.quote.getReserved(),
            balances.base.getReserved());
    }

    if (balances.quote.getReserved() < 0_dec) {
        throw std::runtime_error(fmt::format(
            "{} | AGENT #{} BOOK {} | {}: Reserved quote balance {} < 0 after unregistering order #{}", 
            simulation()->currentTimestamp(),
            agentId,
            simulation()->bookIdCanon(bookId), std::source_location::current().function_name(),
            balances.quote.getReserved(), agentId, orderId));
    }
    if (accounts()[agentId].activeOrders()[bookId].empty()) {

        if (balances.quote.getReserved() > 0_dec){
            for (const auto& res : balances.quote.getReservations()){
                fmt::println("unregisterLimitOrderCallback | Releasing Quote residual reservation {} with no corresponding active order #{} in book #{}", 
                    res.second, res.first, simulation()->bookIdCanon(bookId));
                // balances.releaseReservation(res.first, simulation()->bookIdCanon(bookId));
            }
        }

        if (balances.base.getReserved() > 0_dec){
            for (const auto& res : balances.base.getReservations()){
                fmt::println("unregisterLimitOrderCallback | Releasing Base residual reservation {} with no corresponding active order #{} in book #{}", 
                    res.second, res.first, simulation()->bookIdCanon(bookId));
                // balances.releaseReservation(res.first, simulation()->bookIdCanon(bookId));
            }
        }
    }
}

//-------------------------------------------------------------------------

void MultiBookExchangeAgent::marketOrderProcessedCallback(
    MarketOrder::Ptr marketOrder, OrderContext ctx)
{
    accounts()[ctx.agentId].activeOrders()[ctx.bookId].erase(marketOrder);
    taosim::accounting::Balances& balances = accounts()[ctx.agentId][ctx.bookId];

    if (balances.canFree(marketOrder->id())){
        balances.freeReservation(marketOrder->id(), m_books[ctx.bookId]->bestAsk(),
            m_books[ctx.bookId]->bestBid(), m_books[ctx.bookId]->bestAsk(), marketOrder->direction(), 
            simulation()->bookIdCanon(ctx.bookId));
    }
}

//-------------------------------------------------------------------------
