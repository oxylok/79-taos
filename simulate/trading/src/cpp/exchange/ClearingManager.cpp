/*
 * SPDX-FileCopyrightText: 2025 Rayleigh Research <to@rayleigh.re>
 * SPDX-License-Identifier: MIT
 */
#include "ClearingManager.hpp"

#include "MultiBookExchangeAgent.hpp"
#include "Simulation.hpp"
#include "taosim/accounting/margin_utils.hpp"
#include "taosim/exchange/FeePolicy.hpp"

#include <bit>

//-------------------------------------------------------------------------

namespace taosim::exchange
{

//-------------------------------------------------------------------------

ClearingManager::ClearingManager(
    MultiBookExchangeAgent* exchange,
    std::unique_ptr<FeePolicyWrapper> feePolicy,
    OrderPlacementValidator::Parameters validatorParams) noexcept
    : m_exchange{exchange},
      m_feePolicy{std::move(feePolicy)},
      m_orderPlacementValidator{std::move(validatorParams), exchange}
{}

//-------------------------------------------------------------------------

MultiBookExchangeAgent* ClearingManager::exchange() noexcept
{
    return m_exchange;
}

//-------------------------------------------------------------------------

accounting::AccountRegistry& ClearingManager::accounts() noexcept
{
    return m_exchange->accounts();
}

//-------------------------------------------------------------------------

OrderResult ClearingManager::handleOrder(const OrderDesc& orderDesc)
{

    const auto [agentId, bookId, quantity, price, expectedValidationResult] =
        std::visit(
            [&](auto&& desc) {
                using T = std::remove_cvref_t<decltype(desc)>;
                if constexpr (std::same_as<T, MarketOrderDesc>) {
                    return std::make_tuple(
                        desc.agentId,
                        desc.payload->bookId,
                        desc.payload->volume,
                        0_dec,
                        m_orderPlacementValidator.validateMarketOrderPlacement(
                            accounts()[desc.agentId],
                            m_exchange->books()[desc.payload->bookId],
                            desc.payload,
                            *m_feePolicy,
                            m_exchange->getMaxLeverage(),
                            m_exchange->getMaxLoan(),
                            accounts().getAgentId(desc.agentId)));
                }
                else if constexpr (std::same_as<T, LimitOrderDesc>) {
                    return std::make_tuple(
                        desc.agentId,
                        desc.payload->bookId,
                        desc.payload->volume,
                        desc.payload->price,
                        m_orderPlacementValidator.validateLimitOrderPlacement(
                            accounts()[desc.agentId],
                            m_exchange->books()[desc.payload->bookId],
                            desc.payload,
                            *m_feePolicy,
                            m_exchange->getMaxLeverage(),
                            m_exchange->getMaxLoan(),
                            accounts().getAgentId(desc.agentId)));
                }
                else {
                    static_assert(false, "Unrecognized Order descriptor");
                }
            },
            orderDesc);

    if (!expectedValidationResult.has_value()) {
        return {
            .ec =  expectedValidationResult.error(),
            .orderSize = 0_dec
        };
    }
    const auto& validationResult = expectedValidationResult.value();

    auto& balances = accounts().at(agentId).at(bookId);
    const OrderID orderId = m_exchange->books()[bookId]->orderFactory().getCounterState();

    if (validationResult.instantTrade){
        m_exchange->instructionLogCallback(orderDesc, orderId);
    }

    const decimal_t curPrice = validationResult.direction == OrderDirection::BUY ?
        m_exchange->books()[bookId]->bestAsk() : m_exchange->books()[bookId]->bestBid();

    if (validationResult.amount <= 0_dec) {
        fmt::println("Incorrect validation results {} with lev {} at {} price for incoming order #{} in book #{}", 
            validationResult.amount,
            validationResult.leverage,
            curPrice,
            orderId,
            m_exchange->simulation()->bookIdCanon(bookId)
        );
    }

    m_exchange->simulation()->logDebug("{} | AGENT #{} BOOK {} : MAKING RESERVATION {} {} WITH LEV {} FOR {} ORDER #{}", 
        m_exchange->simulation()->currentTimestamp(), std::holds_alternative<LocalAgentId>(agentId) ? m_exchange->accounts().idBimap().left.at(std::get<LocalAgentId>(agentId)) : std::get<AgentId>(agentId), m_exchange->simulation()->bookIdCanon(bookId),
        validationResult.amount, validationResult.direction == OrderDirection::BUY ? "QUOTE" : "BASE", validationResult.leverage, validationResult.direction == OrderDirection::BUY ? "BUY" : "SELL", orderId
    );
    auto reserved = balances.makeReservation(orderId, price > 0_dec ? price : curPrice,
        m_exchange->books()[bookId]->bestBid(), m_exchange->books()[bookId]->bestAsk(),
        validationResult.amount, validationResult.leverage, validationResult.direction, m_exchange->simulation()->bookIdCanon(bookId));
    
    m_exchange->simulation()->logDebug(
        "{} | AGENT #{} BOOK {} : RESERVATION OF {} BASE + {} QUOTE (={} {}) CREATED FOR {} ORDER #{} ({}x{}@{}) | BEST {} : {} | MAX LEV : {}", 
        m_exchange->simulation()->currentTimestamp(), std::holds_alternative<LocalAgentId>(agentId) ? m_exchange->accounts().idBimap().left.at(std::get<LocalAgentId>(agentId)) : std::get<AgentId>(agentId), m_exchange->simulation()->bookIdCanon(bookId),
        reserved.base, reserved.quote, validationResult.amount, validationResult.direction == OrderDirection::BUY ? "QUOTE" : "BASE",
        validationResult.direction == OrderDirection::BUY ? "BUY" : "SELL", orderId, 
        1_dec + validationResult.leverage, quantity, price > 0_dec ? fmt::format("{}", price) : "MARKET",
        validationResult.direction == OrderDirection::BUY ? "ASK" : "BID", curPrice, m_exchange->getMaxLeverage()
    );    

    return {
        .ec = OrderErrorCode::VALID,
        .orderSize = validationResult.orderSize
    };
}

//-------------------------------------------------------------------------

bool ClearingManager::handleClosePosition(const ClosePositionDesc &closeDesc)
{
    const auto& [bookId, agentId, orderId, volumeToClose] = closeDesc;

    accounting::Account& account = accounts()[agentId];
    accounting::Balances& balances = account[bookId];
    const auto& loan = balances.getLoan(orderId);

    if (loan.has_value()){
        decimal_t loanAmount = loan->get().amount();
        decimal_t settleAmount = volumeToClose.value_or(loanAmount);

        // if (settleAmount > loanAmount){
        //     m_exchange->simulation()->logDebug("Settle Amount {} bigger than the remining loan {}",
        //         settleAmount, loanAmount
        //     );
        //     return false;
        // }

        if (loan->get().direction() == OrderDirection::BUY){
            const auto bestBid = m_exchange->books()[bookId]->bestBid();
            settleAmount = util::roundUp(
                bestBid * util::roundUp(settleAmount / bestBid, 
                    m_exchange->config().parameters().baseIncrementDecimals),
                m_exchange->config().parameters().quoteIncrementDecimals);

            const auto takerFee = m_feePolicy->getRates(bookId, agentId).taker;
            settleAmount = util::roundUp(settleAmount /util::dec1m(takerFee),
                m_exchange->config().parameters().quoteIncrementDecimals);
        }

        auto orderPayload = MessagePayload::create<PlaceOrderMarketPayload>(
                loan->get().direction() == OrderDirection::BUY ? OrderDirection::SELL : OrderDirection::BUY,
                settleAmount,
                bookId,
                loan->get().direction() == OrderDirection::BUY ? Currency::QUOTE : Currency::BASE,
                std::nullopt,
                STPFlag::CO,
                orderId
            );
        if (agentId < 0) {
            m_exchange->simulation()->dispatchMessage(
                m_exchange->simulation()->currentTimestamp(),
                0,
                accounts().idBimap().right.at(agentId),
                m_exchange->name(),
                "PLACE_ORDER_MARKET",
                orderPayload
            );
        } else {
            m_exchange->simulation()->dispatchMessage(
                m_exchange->simulation()->currentTimestamp(),
                0,
                "DISTRIBUTED_PROXY_AGENT",
                m_exchange->name(),
                "DISTRIBUTED_PLACE_ORDER_MARKET",
                MessagePayload::create<DistributedAgentResponsePayload>(
                    agentId,
                    orderPayload
                )
            );
        }
        
    } else {
        return false;
    }

    return true;
}

//-------------------------------------------------------------------------

void ClearingManager::handleCancelOrder(const CancelOrderDesc& cancelDesc)
{
    const auto& [bookId, order, volumeToCancel] = cancelDesc;

    const OrderID orderId = order->id();
    const AgentId agentId = m_exchange->books()[bookId]->orderClientContext(orderId).agentId;

    accounting::Account& account = accounts()[agentId];
    accounting::Balances& balances = account[bookId];

    const auto freed = [&] {
        if (order->direction() == OrderDirection::BUY) {
            return balances.freeReservation(
                orderId,
                order->price(),
                m_exchange->books()[bookId]->bestBid(), 
                m_exchange->books()[bookId]->bestAsk(), 
                order->direction(),
                m_exchange->simulation()->bookIdCanon(bookId),
                // volumeToCancel < order->totalVolume()
                volumeToCancel < order->volume()
                    ? std::make_optional(
                        util::round(
                            util::round(order->price(), m_exchange->config().parameters().priceIncrementDecimals) *
                            util::round(volumeToCancel, m_exchange->config().parameters().volumeIncrementDecimals) *
                            util::dec1p(m_feePolicy->getRates(bookId, agentId).maker),
                            m_exchange->config().parameters().quoteIncrementDecimals))
                    : std::nullopt);

        } else {
            return balances.freeReservation(
                orderId,
                order->price(),
                m_exchange->books()[bookId]->bestBid(), 
                m_exchange->books()[bookId]->bestAsk(), 
                order->direction(),
                m_exchange->simulation()->bookIdCanon(bookId),
                // volumeToCancel < order->totalVolume()
                volumeToCancel < order->volume()
                    ? std::make_optional(
                        util::round(volumeToCancel, m_exchange->config().parameters().baseIncrementDecimals))
                    : std::nullopt);
        }
    }();

    // if (volumeToCancel == order->totalVolume()) {
    if (volumeToCancel >= order->volume()) {
        account.activeOrders()[bookId].erase(order);
        // balances.releaseReservation(order->id());
    }

    m_exchange->simulation()->logDebug(
        "{} | AGENT #{} BOOK {} : CANCELLED {} ORDER #{} ({}@{}) for {} (FREED {} BASE + {} QUOTE)",
        m_exchange->simulation()->currentTimestamp(),
        agentId,
        m_exchange->simulation()->bookIdCanon(bookId),
        order->direction(),
        orderId,
        order->leverage() > 0_dec ? fmt::format("{}x{}",1_dec + order->leverage(),order->volume()) : fmt::format("{}",order->volume()),
        order->price(),
        volumeToCancel,
        freed.base, freed.quote,
        balances.quote.getReserved(),
        balances.base.getReserved());

    if (balances.quote.getReserved() < 0_dec) {
        throw std::runtime_error(fmt::format(
            "{} | AGENT #{} BOOK {} | {}: Reserved quote balance {} < 0 after cancelling order #{}", 
            m_exchange->simulation()->currentTimestamp(),
            agentId,
            m_exchange->simulation()->bookIdCanon(bookId), std::source_location::current().function_name(),
            balances.quote.getReserved(), agentId, orderId));
    }
    if (account.activeOrders()[bookId].empty()) {

        if (balances.quote.getReserved() > 0_dec){
            for (const auto& res : balances.quote.getReservations()){
                fmt::println("handleCancelOrder | Releasing Quote residual reservation {} with no corresponding active order #{} in book #{}", 
                    res.second, res.first, m_exchange->simulation()->bookIdCanon(bookId));
                // balances.releaseReservation(res.first, m_exchange->simulation()->bookIdCanon(bookId));
            }
        }

        if (balances.base.getReserved() > 0_dec){
            for (const auto& res : balances.base.getReservations()){
                fmt::println("handleCancelOrder | Releasing Base residual reservation {} with no corresponding active order #{} in book #{}", 
                    res.second, res.first, m_exchange->simulation()->bookIdCanon(bookId));
                // balances.releaseReservation(res.first, m_exchange->simulation()->bookIdCanon(bookId));
            }
        }
    }

}

//-------------------------------------------------------------------------

Fees ClearingManager::handleTrade(const TradeDesc& tradeDesc)
{
    const auto [bookId, restingAgentId, aggressingAgentId, trade] = tradeDesc;

    const OrderID restingOrderId = trade->restingOrderID();
    const OrderID aggressingOrderId = trade->aggressingOrderID();

    const auto& restingAgentActiveOrders =
        accounts()[restingAgentId].activeOrders()[bookId];
    const auto& aggressingAgentActiveOrders =
        accounts()[aggressingAgentId].activeOrders()[bookId];

    auto restingOrderIt = std::find_if(
        restingAgentActiveOrders.begin(),
        restingAgentActiveOrders.end(),
        [restingOrderId](const auto o) { return o->id() == restingOrderId; });
    if (restingOrderIt == restingAgentActiveOrders.end()) {
        throw std::runtime_error{fmt::format(
            "{} | AGENT #{} BOOK {} : Resting order #{} not found in active orders while processing {} trade #{} against order #{} from {} for {}@{}.",
            m_exchange->simulation()->currentTimestamp(),
            restingAgentId,
            m_exchange->simulation()->bookIdCanon(bookId),
            restingOrderId,
            trade->direction() == OrderDirection::BUY ? "BUY" : "SELL",
            trade->id(),
            trade->aggressingOrderID(),
            aggressingAgentId,
            trade->volume(),
            trade->price()
        )};
    }
    LimitOrder::Ptr restingOrder = std::dynamic_pointer_cast<LimitOrder>(*restingOrderIt);

    auto aggressingOrderIt = std::find_if(
        aggressingAgentActiveOrders.begin(),
        aggressingAgentActiveOrders.end(),
        [aggressingOrderId](const auto o) { return o->id() == aggressingOrderId; });
    if (aggressingOrderIt == aggressingAgentActiveOrders.end()) {        
        throw std::runtime_error{fmt::format(
            "{} | AGENT #{} BOOK {} : Aggressing order #{} not found in active orders.",
            m_exchange->simulation()->currentTimestamp(),
            aggressingAgentId,
            m_exchange->simulation()->bookIdCanon(bookId),
            aggressingOrderId)};
    }
    Order::Ptr aggressingOrder = *aggressingOrderIt;

    auto fees = m_feePolicy->calculateFees(tradeDesc);

    // TODO: Rounding should be done inside fees calculation
    fees.taker = util::round(fees.taker, m_exchange->config().parameters().quoteIncrementDecimals);
    fees.maker = util::round(fees.maker, m_exchange->config().parameters().quoteIncrementDecimals);

    accounting::Balances& restingBalance = accounts()[restingAgentId][bookId];
    accounting::Balances& aggressingBalance = accounts()[aggressingAgentId][bookId];

    const decimal_t bestBid = m_exchange->books()[bookId]->bestBid();
    const decimal_t bestAsk = m_exchange->books()[bookId]->bestAsk();

    // Policy: The direction of the trade is that of the aggressing order.
    if (trade->direction() == OrderDirection::BUY) {
        // Aggressing is BUY, quote reserved; Resting is SELL, base reserved.
        taosim::decimal_t reservation = util::round(aggressingBalance.getReservationInQuote(aggressingOrderId, bestAsk) *
            util::dec1p(aggressingBalance.getLeverage(aggressingOrderId, aggressingOrder->direction())),
            m_exchange->config().parameters().quoteIncrementDecimals
        );

        const auto totalPrice = [&] {
            if (auto limitOrder = std::dynamic_pointer_cast<LimitOrder>(aggressingOrder)) {
                // if (!reservation.has_value()) {
                if (reservation == 0_dec) {
                    throw std::runtime_error{fmt::format(
                        "{} | AGENT #{} BOOK {} : No reservation for aggressing {} order #{}.",
                        m_exchange->simulation()->currentTimestamp(), 
                        aggressingAgentId, m_exchange->simulation()->bookIdCanon(bookId),
                        aggressingOrder->direction(), aggressingOrderId)};
                }
                if (aggressingOrder->totalVolume() == trade->volume()) {
                    m_exchange->simulation()->logDebug(
                        "{} | AGENT #{} BOOK {} : Committing reservation amount {} for trade volume {} in {} order #{}.",
                        m_exchange->simulation()->currentTimestamp(),
                        aggressingAgentId, m_exchange->simulation()->bookIdCanon(bookId),
                        util::round(reservation / util::dec1p(aggressingBalance.getLeverage(aggressingOrderId, aggressingOrder->direction())), m_exchange->config().parameters().quoteIncrementDecimals),
                        trade->volume(), aggressingOrder->direction(), aggressingOrderId);
                    return reservation - fees.taker;
                }
            }
            return util::round(trade->price(), m_exchange->config().parameters().priceIncrementDecimals) * 
                    util::round(trade->volume(), m_exchange->config().parameters().volumeIncrementDecimals);
        }();

        decimal_t aggressingMarginCall = {}, restingMarginCall = {};
        // margin call price for margin buying
        if (aggressingOrder->leverage() > 0_dec){
            aggressingMarginCall = accounting::calculateMarginCallPrice(trade->price(), aggressingOrder->leverage(),
                OrderDirection::BUY, m_exchange->getMaintenanceMargin());
            m_marginBuy[bookId][aggressingMarginCall].push_back({
                .orderId = aggressingOrderId, .agentId = aggressingAgentId
            });
        }
        // margin call price for short selling
        if (restingOrder->leverage() > 0_dec){
            restingMarginCall = accounting::calculateMarginCallPrice(trade->price(), restingOrder->leverage(),
                OrderDirection::SELL, m_exchange->getMaintenanceMargin());
            m_marginSell[bookId][restingMarginCall].push_back({
                .orderId = restingOrderId, .agentId = restingAgentId
            });
        }

        const auto aggressingVolume = util::round(totalPrice, m_exchange->config().parameters().quoteIncrementDecimals);
        const auto restingVolume = util::round(trade->volume(), m_exchange->config().parameters().baseIncrementDecimals);
        const auto tradeQuote = util::round(trade->volume() * trade->price(), m_exchange->config().parameters().quoteIncrementDecimals);

        m_feePolicy->updateHistory(bookId, restingAgentId, tradeQuote);
        m_feePolicy->updateHistory(bookId, aggressingAgentId, aggressingVolume);

        m_exchange->simulation()->logDebug(
            "{} | AGENT #{} BOOK {} : COMMIT {} WITH FEE {} FOR AGG BUY ORDER #{} AGAINST {} FOR RESTING SELL ORDER #{} (BEST ASK {} | MARGIN={})",
            m_exchange->simulation()->currentTimestamp(), aggressingAgentId, m_exchange->simulation()->bookIdCanon(bookId),
            aggressingVolume, fees.taker, aggressingOrderId, restingVolume, restingOrderId, bestAsk, aggressingMarginCall);
        auto removedIdsShortSell = aggressingBalance.commit(
            aggressingOrderId,
            OrderDirection::BUY,
            aggressingVolume,
            restingVolume,
            fees.taker,
            bestBid,
            bestAsk,
            aggressingMarginCall,
            m_exchange->simulation()->bookIdCanon(bookId),
            aggressingOrder->settleFlag());

        m_exchange->simulation()->logDebug(
            "{} | AGENT #{} BOOK {} : COMMIT {} WITH FEE {} FOR RESTING SELL ORDER #{} AGAINST {} FOR AGG BUY ORDER #{} (BEST BID {} | MARGIN={})",
            m_exchange->simulation()->currentTimestamp(), restingAgentId, m_exchange->simulation()->bookIdCanon(bookId),
            restingVolume, fees.maker, restingOrderId, aggressingVolume, aggressingOrderId, bestBid, restingMarginCall);
        auto removedIdsMarginBuy = restingBalance.commit(
            restingOrderId, 
            OrderDirection::SELL,
            restingVolume,
            aggressingVolume,
            fees.maker,
            bestBid,
            bestAsk,
            restingMarginCall,
            m_exchange->simulation()->bookIdCanon(bookId),
            restingOrder->settleFlag());

        removeMarginOrders(bookId, OrderDirection::BUY, removedIdsMarginBuy);
        removeMarginOrders(bookId, OrderDirection::SELL, removedIdsShortSell);

        exchange()->simulation()->logDebug("{} | AGENT #{} BOOK {} : AGG BUY ORDER #{} FROM AGENT #{} FOR {} TRADED AGAINST RESTING #{} {}@{} FROM AGENT #{} FOR {} (MAKER {} QUOTE | TAKER {} QUOTE)", 
            exchange()->simulation()->currentTimestamp(), aggressingAgentId, m_exchange->simulation()->bookIdCanon(bookId), 
            aggressingOrder->id(), aggressingAgentId, aggressingOrder->leverage() > 0_dec ? fmt::format("{}x{}",1_dec + aggressingOrder->leverage(),aggressingOrder->volume()) : fmt::format("{}",aggressingOrder->volume()), 
            restingOrder->id(), restingOrder->leverage() > 0_dec ? fmt::format("{}x{}",1_dec + restingOrder->leverage(),restingOrder->volume()) : fmt::format("{}",restingOrder->volume()), restingOrder->price(), restingAgentId, trade->volume(), 
            fees.maker, fees.taker);

    }
    else {
        // Aggressing is SELL, base reserved; Resting is BUY, quote reserved.
        taosim::decimal_t reservation = util::round(restingBalance.getReservationInQuote(restingOrderId, bestBid) *
            util::dec1p(restingBalance.getLeverage(restingOrderId, restingOrder->direction())),
            m_exchange->config().parameters().quoteIncrementDecimals
        );

        if (reservation == 0_dec) {
            throw std::runtime_error{fmt::format(
                "{} | AGENT #{} BOOK {} : Trade volume {}, No reservation for resting {} order #{}.",
                m_exchange->simulation()->currentTimestamp(),
                restingAgentId, m_exchange->simulation()->bookIdCanon(bookId), trade->volume(),
                restingOrder->direction(), restingOrderId)};
        } else if (restingOrder->totalVolume() == trade->volume()) {
            m_exchange->simulation()->logDebug(
                "{} | AGENT #{} BOOK {} : Committing reservation amount {} for trade volume {} in {} order #{}.",
                m_exchange->simulation()->currentTimestamp(),
                restingAgentId, m_exchange->simulation()->bookIdCanon(bookId),
                util::round(reservation / util::dec1p(restingBalance.getLeverage(restingOrderId, restingOrder->direction())), m_exchange->config().parameters().quoteIncrementDecimals), 
                trade->volume(), restingOrder->direction(), restingOrderId);
        }

        taosim::decimal_t aggressingMarginCall = {}, restingMarginCall = {};
        // margin call price for short selling
        if (aggressingOrder->leverage() > 0_dec){
            aggressingMarginCall = accounting::calculateMarginCallPrice(trade->price(), aggressingOrder->leverage(),
                OrderDirection::SELL, m_exchange->getMaintenanceMargin());
            m_marginSell[bookId][aggressingMarginCall].push_back({.orderId = aggressingOrderId, .agentId = aggressingAgentId});
        }
        // margin call price for margin buying
        if (restingOrder->leverage() > 0_dec){
            restingMarginCall = accounting::calculateMarginCallPrice(trade->price(), restingOrder->leverage(),
                OrderDirection::BUY, m_exchange->getMaintenanceMargin());
            (restingOrder->leverage() * trade->price()) / ((1_dec + restingOrder->leverage()) * 
                (1 - m_exchange->getMaintenanceMargin()));
            m_marginBuy[bookId][restingMarginCall].push_back({.orderId = restingOrderId, .agentId = restingAgentId});
        }

        const auto aggressingVolume = util::round(trade->volume(), m_exchange->config().parameters().baseIncrementDecimals);
        const auto restingVolume = restingOrder->totalVolume() == trade->volume() && reservation > 0_dec ?
            reservation - fees.maker : util::round(trade->price() * trade->volume(), m_exchange->config().parameters().quoteIncrementDecimals);
        const auto tradeQuote = util::round(trade->volume() * trade->price(), m_exchange->config().parameters().quoteIncrementDecimals);

        m_feePolicy->updateHistory(bookId, restingAgentId, tradeQuote);
        m_feePolicy->updateHistory(bookId, aggressingAgentId, restingVolume);
        
        m_exchange->simulation()->logDebug(
            "{} | AGENT #{} BOOK {} : COMMIT {} WITH FEE {} FOR AGG SELL ORDER #{} AGAINST {} FOR RESTING BUY ORDER #{} (BEST ASK {} | MARGIN={})",
            m_exchange->simulation()->currentTimestamp(), aggressingAgentId, m_exchange->simulation()->bookIdCanon(bookId),
            aggressingVolume, fees.taker, aggressingOrderId, restingVolume, restingOrderId, bestAsk, aggressingMarginCall);

        auto removedIdsMarginBuy = aggressingBalance.commit(
            aggressingOrderId,
            OrderDirection::SELL,
            aggressingVolume,
            restingVolume,
            fees.taker,
            bestBid,
            bestAsk,
            aggressingMarginCall,
            m_exchange->simulation()->bookIdCanon(bookId),
            aggressingOrder->settleFlag());

        m_exchange->simulation()->logDebug(
            "{} | AGENT #{} BOOK {} : COMMIT {} WITH FEE {} FOR RESTING BUY ORDER #{} AGAINST {} FOR AGG SELL ORDER #{} (BEST BID {} | MARGIN={})",
            m_exchange->simulation()->currentTimestamp(), restingAgentId, m_exchange->simulation()->bookIdCanon(bookId),
            restingVolume, fees.maker, restingOrderId, aggressingVolume, aggressingOrderId, bestBid, restingMarginCall);
            
        auto removedIdsShortSell = restingBalance.commit(
            restingOrderId, 
            OrderDirection::BUY,
            restingVolume,
            aggressingVolume,
            fees.maker,
            bestBid,
            bestAsk,
            restingMarginCall,
            m_exchange->simulation()->bookIdCanon(bookId),
            restingOrder->settleFlag());

        removeMarginOrders(bookId, OrderDirection::SELL, removedIdsShortSell);
        removeMarginOrders(bookId, OrderDirection::BUY, removedIdsMarginBuy);

        exchange()->simulation()->logDebug("{} | AGENT #{} BOOK {} : AGG SELL ORDER #{} FROM AGENT #{} FOR {} TRADED AGAINST RESTING #{} {}@{} FROM AGENT #{} FOR {} (MAKER {} QUOTE | TAKER {} QUOTE)", 
            exchange()->simulation()->currentTimestamp(), aggressingAgentId, m_exchange->simulation()->bookIdCanon(bookId), 
            aggressingOrder->id(), aggressingAgentId, aggressingOrder->leverage() > 0_dec ? fmt::format("{}x{}",1_dec + aggressingOrder->leverage(),aggressingOrder->volume()) : fmt::format("{}",aggressingOrder->volume()), 
            restingOrder->id(), restingOrder->leverage() > 0_dec ? fmt::format("{}x{}",1_dec + restingOrder->leverage(),restingOrder->volume()) : fmt::format("{}",restingOrder->volume()), restingOrder->price(), restingAgentId, trade->volume(), 
            fees.maker, fees.taker);
    }

    return fees;
}

//-------------------------------------------------------------------------

void ClearingManager::removeMarginOrders(
    BookId bookId, OrderDirection direction, std::span<std::pair<OrderID, decimal_t>> ids)
{   
    auto& cont = direction == OrderDirection::BUY ? m_marginBuy : m_marginSell;

    auto marginIt = cont.find(bookId);
    if (marginIt == cont.end()) return;

    auto& marginOrders = marginIt->second;

    for (const auto& [orderId, amount] : ids) {
        auto ordersIt = marginOrders.find(amount);
        if (ordersIt == marginOrders.end()) continue;
        auto& orders = ordersIt->second;
        std::erase_if(
            orders, [orderId](const auto& order) { return order.orderId == orderId; });
        if (orders.empty()) {
            marginOrders.erase(ordersIt);
        }
    }

    if (marginOrders.empty()) {
        cont.erase(marginIt);
    }
}

//-------------------------------------------------------------------------

void ClearingManager::updateFeeTiers(Timestamp time) noexcept
{
    m_feePolicy->updateAgentsTiers(time);
}

//-------------------------------------------------------------------------

}  // namespace taosim::exchange

//-------------------------------------------------------------------------