/*
 * SPDX-FileCopyrightText: 2025 Rayleigh Research <to@rayleigh.re>
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "taosim/accounting/Balance.hpp"
#include "taosim/accounting/Balances.hpp"
#include "common.hpp"

#include <set>

//-------------------------------------------------------------------------

namespace taosim::accounting
{

//-------------------------------------------------------------------------

class Account : public JsonSerializable, public CheckpointSerializable
{
public:
    using Holdings = std::vector<Balances>;
    using ActiveOrders = std::vector<std::set<Order::Ptr>>;

    Account() noexcept = default;
    explicit Account(uint32_t bookCount, std::optional<Balances> balances) noexcept;

    [[nodiscard]] auto& at(this auto&& self, BookId bookId) { return self.m_holdings.at(bookId); }
    [[nodiscard]] auto& operator[](this auto&& self, BookId bookId) { return self.m_holdings[bookId]; }

    [[nodiscard]] decltype(auto) begin(this auto&& self) { return self.m_holdings.begin(); }
    [[nodiscard]] decltype(auto) end(this auto&& self) { return self.m_holdings.end(); }

    [[nodiscard]] auto&& holdings(this auto&& self) noexcept { return self.m_holdings; }
    [[nodiscard]] auto&& activeOrders(this auto&& self) noexcept { return self.m_activeOrders; }

    virtual void jsonSerialize(
        rapidjson::Document& json, const std::string& key = {}) const override;
    virtual void checkpointSerialize(
        rapidjson::Document& json, const std::string& key = {}) const override;

    friend std::ostream& operator<<(std::ostream& os, const Account& holdings);

    [[nodiscard]] static Account fromJson(const rapidjson::Value& json);

private:
    Account(Holdings holdings, ActiveOrders activeOrders) noexcept;

    Holdings m_holdings;
    ActiveOrders m_activeOrders;
};

//-------------------------------------------------------------------------

}  // namespace taosim::accounting

//-------------------------------------------------------------------------

template<>
struct fmt::formatter<taosim::accounting::Account>
{
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

    template<typename FormatContext>
    auto format(const taosim::accounting::Account& holdings, FormatContext& ctx) const
    {
        std::ostringstream oss;
        oss << holdings;
        return fmt::format_to(ctx.out(), "{}", oss.str());
    }
};

//-------------------------------------------------------------------------
