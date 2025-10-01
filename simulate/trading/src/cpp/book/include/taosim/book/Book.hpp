/*
 * SPDX-FileCopyrightText: 2025 Rayleigh Research <to@rayleigh.re>
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "BookSignals.hpp"
#include "CSVPrintable.hpp"
#include "IHumanPrintable.hpp"
#include "taosim/book/OrderContainer.hpp"
#include "OrderFactory.hpp"
#include "taosim/book/TickContainer.hpp"
#include "TradeFactory.hpp"
#include "common.hpp"

#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <queue>
#include <string>

//-------------------------------------------------------------------------

class Simulation;

//-------------------------------------------------------------------------

class Book
    : public CSVPrintable,
      public JsonSerializable
{
public:
    using Ptr = std::shared_ptr<Book>;

    Book(
        Simulation* simulation,
        BookId id,
        size_t maxDepth,
        size_t detailedDepth);

    virtual ~Book() noexcept = default;

    [[nodiscard]] Simulation* simulation() const noexcept { return m_simulation; }
    [[nodiscard]] BookId id() const noexcept { return m_id; }    
    [[nodiscard]] const OrderFactory& orderFactory() const noexcept { return m_orderFactory; }
    [[nodiscard]] const TradeFactory& tradeFactory() const noexcept { return m_tradeFactory; }
    [[nodiscard]] const taosim::book::OrderContainer& buyQueue() const { return m_buyQueue; }
    [[nodiscard]] const taosim::book::OrderContainer& sellQueue() const { return m_sellQueue; }
    [[nodiscard]] BookSignals& signals() noexcept { return m_signals; }
    [[nodiscard]] taosim::decimal_t midPrice() const noexcept;
    [[nodiscard]] taosim::decimal_t bestBid() const noexcept;
    [[nodiscard]] taosim::decimal_t bestAsk() const noexcept;
    [[nodiscard]] size_t detailedDepth() const noexcept { return m_detailedDepth; }

    [[nodiscard]] const OrderClientContext& orderClientContext(OrderID orderId) const
    {
        return m_order2clientCtx.at(orderId);
    }

    MarketOrder::Ptr placeMarketOrder(
        OrderDirection direction,
        Timestamp timestamp,
        taosim::decimal_t volume,
        taosim::decimal_t leverage,
        OrderClientContext ctx,
        STPFlag stpFlag = STPFlag::CO,
        SettleFlag settleFlag = SettleType::FIFO,
        Currency currency = Currency::BASE);

    LimitOrder::Ptr placeLimitOrder(
        OrderDirection direction,
        Timestamp timestamp,
        taosim::decimal_t volume,
        taosim::decimal_t price,
        taosim::decimal_t leverage,
        OrderClientContext ctx,
        STPFlag stpFlag = STPFlag::CO,
        SettleFlag settleFlag = SettleType::FIFO,
        bool postOnly = false,
        taosim::TimeInForce timeInForce = taosim::TimeInForce::GTC,
        std::optional<Timestamp> expiryPeriod = std::nullopt,
        Currency currency = Currency::BASE);
        
    bool cancelOrderOpt(OrderID orderId, std::optional<taosim::decimal_t> volumeToCancel = {});

    bool tryGetOrder(OrderID id, LimitOrder::Ptr& orderPtr) const;
    std::optional<LimitOrder::Ptr> getOrder(OrderID orderId) const;
    void L2Serialize(rapidjson::Document& json, const std::string& key = {}) const;

    virtual void printCSV() const override;
    virtual void jsonSerialize(
        rapidjson::Document& json, const std::string& key = {}) const override;

    void printCSV(uint32_t depth) const;

protected:
    void placeOrder(MarketOrder::Ptr order);
    void placeOrder(LimitOrder::Ptr order);
    void placeLimitBuy(LimitOrder::Ptr order);
    void placeLimitSell(LimitOrder::Ptr order);

    void registerLimitOrder(LimitOrder::Ptr order);
    void unregisterLimitOrder(LimitOrder::Ptr order);
    std::map<OrderID, LimitOrder::Ptr> m_orderIdMap;

    Simulation* m_simulation;
    BookId m_id;
    size_t m_maxDepth;
    size_t m_detailedDepth;
    OrderFactory m_orderFactory;
    TradeFactory m_tradeFactory;
    BookSignals m_signals;
    std::map<OrderID, OrderClientContext> m_order2clientCtx;
    taosim::book::OrderContainer m_buyQueue;
    LimitOrder::Ptr m_lastBetteringBuyOrder = nullptr;
    taosim::book::OrderContainer m_sellQueue;
    LimitOrder::Ptr m_lastBetteringSellOrder = nullptr;
    bool m_initMode = false;

    virtual taosim::decimal_t processAgainstTheBuyQueue(Order::Ptr order, taosim::decimal_t minPrice) = 0;
    virtual taosim::decimal_t processAgainstTheSellQueue(Order::Ptr order, taosim::decimal_t maxPrice) = 0;

    void logTrade(
        OrderDirection direction,
        OrderID aggressorId,
        OrderID restingId,
        taosim::decimal_t volume,
        taosim::decimal_t execPrice);

private:
    void setupL2Signal();

    template<typename... Args>
    void emitL2Signal([[maybe_unused]] Args&&... args) const;

    template<typename CIteratorType>
    void dumpCSVLOB(CIteratorType begin, CIteratorType end, unsigned int depth) const;

    friend class Simulation;
};

//-------------------------------------------------------------------------

template<typename... Args>
void Book::emitL2Signal(Args&&... args) const
{
    m_signals.L2(this);
}

//-------------------------------------------------------------------------

template<typename CIteratorType>
void Book::dumpCSVLOB(CIteratorType begin, CIteratorType end, unsigned int depth) const
{
    while (depth > 0 && begin != end) {
        const taosim::decimal_t totalVolume = [&] {
            taosim::decimal_t totalVolume{};
            for (auto it = begin->begin(); it != begin->end(); ++it) {
                LimitOrder::Ptr order = *it;
                totalVolume += order->volume() * (1_dec + order->leverage());
            }
            return totalVolume;
        }();

        
        if (totalVolume > 0_dec) {
            std::cout
                << ","
                << begin->price()
                << ","
                << totalVolume;
        }

        --depth;
        ++begin;
    }
}

//-------------------------------------------------------------------------
