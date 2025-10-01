/*
 * SPDX-FileCopyrightText: 2025 Rayleigh Research <to@rayleigh.re>
 * SPDX-License-Identifier: MIT
 */
#include "taosim/book/OrderContainer.hpp"
#include "taosim/book/TickContainer.hpp"

#include "util.hpp"

//-------------------------------------------------------------------------

namespace taosim::book
{

//-------------------------------------------------------------------------

TickContainer::TickContainer(OrderContainer* orderContainer, taosim::decimal_t price) noexcept
    : list{}, m_orderContainer{orderContainer}, m_price{price}
{}

//-------------------------------------------------------------------------

void TickContainer::updateVolume(taosim::decimal_t deltaVolume) noexcept
{
    m_volume += deltaVolume;
    m_orderContainer->updateVolume(deltaVolume);
}

//-------------------------------------------------------------------------

void TickContainer::push_back(const TickContainer::value_type& order)
{
    ContainerType::push_back(order);
    m_volume += order->totalVolume();
    m_orderContainer->updateVolume(order->totalVolume());
}

//-------------------------------------------------------------------------

void TickContainer::pop_front()
{
    ContainerType::pop_front();
}

//-------------------------------------------------------------------------

void TickContainer::jsonSerialize(rapidjson::Document& json, const std::string& key) const
{
    auto serialize = [this](rapidjson::Document& json) {
        json.SetObject();
        auto& allocator = json.GetAllocator();
        json.AddMember("price", rapidjson::Value{taosim::util::decimal2double(m_price)}, allocator);
        rapidjson::Value ordersJson{rapidjson::kArrayType};
        for (const auto order : *this) {
            rapidjson::Document orderJson{&allocator};
            order->jsonSerialize(orderJson);
            orderJson.RemoveMember("price");
            ordersJson.PushBack(orderJson, allocator);
        }
        json.AddMember("orders", ordersJson, allocator);
        json.AddMember("volume", rapidjson::Value{taosim::util::decimal2double(m_volume)}, allocator);
    };
    taosim::json::serializeHelper(json, key, serialize);
}

//-------------------------------------------------------------------------

void TickContainer::checkpointSerialize(rapidjson::Document& json, const std::string& key) const
{
    auto serialize = [this](rapidjson::Document& json) {
        json.SetObject();
        auto& allocator = json.GetAllocator();
        json.AddMember("price", rapidjson::Value{taosim::util::packDecimal(m_price)}, allocator);
        rapidjson::Value ordersJson{rapidjson::kArrayType};
        for (const auto order : *this) {
            rapidjson::Document orderJson{&allocator};
            order->checkpointSerialize(orderJson);
            orderJson.RemoveMember("price");
            ordersJson.PushBack(orderJson, allocator);
        }
        json.AddMember("orders", ordersJson, allocator);
        json.AddMember(
            "volume", rapidjson::Value{taosim::util::packDecimal(m_volume)}, allocator);
    };
    taosim::json::serializeHelper(json, key, serialize);
}

//-------------------------------------------------------------------------

}  // namespace taosim::book

//-------------------------------------------------------------------------