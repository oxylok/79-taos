# Agent Development Guide

This document aims to provide some clarification and guidelines to assist miners in development of trading strategies for deployment in the subnet.  This is not intended as a comprehensive set of instructions, but rather provides an overview of the basic understanding and tools needed to begin the design and implementation of agent logic in the context of τaos simulations.

---

**Table of Contents**
- [Data](#data)
  - [Processing State Updates](#processing-state-updates)
    - [Book States](#book-states)
    - [Notices](#notices)
- [Response](#response)
  - [The `FinanceAgentResponse` Class](#the-financeagentresponse-class)
    - [`market_order(...)`](#market_order)
    - [`limit_order(...)`](#limit_order)
    - [`cancel_order(...)`](#cancel_order)
    - [`cancel_orders(...)`](#cancel_orders)
  - [Timeout](#timeout)
  - [Latency](#latency)
    - [Response Time](#response-time)
    - [The `delay` Parameter](#the-delay-parameter)
  - [Trading Volume](#trading-volume)
    - [Contribution to Reward](#contribution-to-reward)
    - [Trading Limitation](#trading-limitation)
- [Agent Testing](#agent-testing)
  - [Local](#local)
  - [Testnet (Netuid 366)](#testnet-netuid-366)
  - [Mainnet (Netuid 79)](#mainnet-netuid-79)
- [Appendix](#appendix)
  - [The `MarketSimulationStateUpdate` Class](#the-marketsimulationstateupdate-class)

---

## Data

The first piece of the puzzle is understanding the format and content of the data published by validators.  Each request sent by a validator includes the latest (partial L3 + L2) state of all orderbooks in the simulation, a record of all events occurring in the simulation since the last update, and miner-specific information relating to the state of the agent's accounts and those events which involve the agent's orders.  The protocol class which defines how this data is represented practically can be found [here](/taos/im/protocol/__init__.py); the structure is expanded and documented for convenient reference in [The Appendix](#the-marketsimulationstateupdate-class).

### Processing State Updates

There are two main types of data that may need to be processed by an agent in deciding on their next trading actions: the first is the actual state data communicating the latest state of each of the simulated orderbooks, and the other is the notices which indicate when an event has occurred in relation to one of the agent's orders.

#### Book States

The latest state of the simulated orderbooks is contained in the `state.books` dictionary.  This dictionary maps the integer identifier for the book to a class structure which represents a snapshot of the top 21 levels of the book state together with a record of all events having occurred since the previous state was published.  In most cases, agents will need to iterate over the books, and apply the same logic to each of them in order to execute their strategy on all realizations of the simulated market.  As demonstrated in the examples, this is most straightforwardly achieved by a simple for loop:

```python
for book_id, book in state.books.items():
    # Analyze state data and generate instructions
```

For more complex and advanced strategies, it may be necessary to parallelize the processing of books in order to ensure to generate a response within the timeout.  Details of how to achieve this are for the miner to design in a manner suitable for their implementation and hosting configuration.

Each book object contains `bids` and `asks` arrays representing the top 21 levels on either side of the order book.  The arrays are ordered with higher indices corresponding to levels further from the midquote, meaning that the best bid (highest buy offer) and best ask (lowest sell offer) are at index 0.  Each level object has a `price` and `quantity` field indicating the price level and the total quantity of all resting orders composing the level.  The topmost levels include also an `orders` field which reveals the composition of the level in terms of the individual orders existing at that price.

```python
# Topmost LevelInfo objects
best_bid_level = book.bids[0]
best_ask_level = book.bids[0]
# Best bid price and volume
bid = best_bid_level.price
bid_vol = best_bid_level.quantity
# Best bid price and volume
ask = best_ask_level.price
ask_vol = best_ask_level.quantity
# Calculate spread
spread = ask - bid
```

The book object further contains an `events` field, which is populated with a complete listing of all the events which have occurred since the last state update.  This allows to reconstruct the complete, high-frequency history of the state of the orderbook throughout the previous publishing interval.  Full details of this procedure and usage are beyond the scope of this document, but the [ImbalanceAgent](/agents/ImbalanceAgent.py) sample agent demonstrates how to use this field and the associated tools to make use of a more complete, high-resolution record of the book state evolution.  An example of a simple use of this record is to obtain the price at which the last trade occurred; the `book` class includes a property method which allows to easily retrieve the latest `TradeInfo` object in the `events` record:

```python
last_trade : TradeInfo = book.last_trade
last_trade_price = last_trade.price
```

It may also be useful for certain strategies to obtain the trade price history over the preceding publishing interval:

```python
trade_price_history = {event.timestamp : event.price for event in book.events if event.type == 't'}
```

#### Notices

The `notices` field of the state update contains a dictionary mapping UIDs to a list of notifications corresponding to events that have occurred specifically in relation to that UID's previously submitted orders.  When received by a miner, this field contains only the notices corresponding to the receiving agent's actions.  If your agent does not override the default `FinanceSimulationAgent.update` method, specific event types can be handled by defining the appropriate functions in your agent code:

- `onStart(self, event : SimulationStartEvent)` : Triggered on start of a new simulation.
- `onOrderAccepted(self, event : OrderPlacementEvent)` : Triggered when an agent's order is accepted by the simulator.
- `onOrderRejected(self, event : OrderPlacementEvent)` : Triggered when an agent's order is rejected by the simulator (due to e.g. insufficient balance, invalid parameters etc).
- `onOrderCancelled(self, event : OrderCancellationEvent)` : Triggered when an agent's order is successfully cancelled in the simulator.
- `onOrderCancellationFailed(self, event : OrderCancellationEvent)` : Triggered when an agent's order fails to be cancelled in the simulator.
- `onTrade(self, event : TradeEvent)` : Triggered when an agent's order is involved in the trade.
- `onEnd(self, event : SimulationEndEvent)` : Triggered when a simulation ends.

To specify logic to be executed when a particular type of notice is received, simply define the handler function in your agent class:

```python
class MyTradingAgent(FinanceSimulationAgent):
    def initialize(self):
        ...

    def respond(self, state : MarketSimulationStateUpdate) -> FinanceAgentResponse:
        ...

    def onTrade(self, event : TradeEvent) -> None:
        print("{event}")
        # Do something - update internal records, trigger placement of a new order, recalculate statistics etc.
```

Note of course that these notices are part of the state update, and so all events occurring in the previous interval will be processed in sequence when the state is received (i.e. before your `respond` method is called).

## Response

Once a miner has received and analyzed the data, they must make a decision about what instructions they wish to submit to the simulation.  Any trading strategy is possible to implement, but note that the state is only published once every `config.publish_interval` simulation nanoseconds, and miners are only able to submit instructions in response to the state.  This implies that strategies must all operate at >=5 second timescale (we aim to lift/reduce this limitation in future).  Some simple example agent implementations can be found in this directory; **it is not expected that running any of the example agents without modification would lead to successful mining in the subnet**.

Miners are expected to develop their own custom agent logic and compete to improve their risk-adjusted performance.  This section documents and explains the usage of the tools involved in agent logic implementation; it does not intend to provide any guidance as to how to design a successful strategy.  However, our simulated markets aim to accurately approximate real markets, so that the same considerations should be applied when formulating strategies as in any trading scenario.  There are further some important limitations and restrictions imposed on miner agents which must be considered when designing a trading strategy, these are also reviewed and explained in the following sections.

### The `FinanceAgentResponse` Class

In order to submit instructions to the validator, a miner must respond to the validator request with an instance of the [`FinanceAgentResponse` class](/taos/im/protocol/response.py).  This class contains one property, `instructions`, which holds an array of `FinanceInstruction`, defined to encapsulate the three main instruction types which miners are able to execute : `PlaceMarketOrderInstruction`, `PlaceLimitOrderInstruction` and `CancelOrdersInstruction` (defined [here](/taos/im/protocol/instructions.py)).  The class additionally exposes convenience methods which allow to easily attach these instruction types to the `FinanceAgentResponse` instance:

---

#### `market_order(...)`

Place a **market order** to immediately buy or sell at the best available price.

##### **Signature**
```python
response.market_order(
    book_id: int,
    direction: OrderDirection,
    quantity: float,
    delay: int = 0,
    clientOrderId: int | None = None,
    stp: STP = STP.CANCEL_OLDEST,
    currency: OrderCurrency = OrderCurrency.BASE
)
```

##### **Arguments**

| Parameter         | Type                       | Description                                                                                                                               |
|--------------------|----------------------------|-------------------------------------------------------------------------------------------------------------------------------------------|
| `book_id`          | `int`                      | ID of the order book where the order will be placed.                                                                                     |
| `direction`        | `OrderDirection`           | `OrderDirection.BUY` or `OrderDirection.SELL`.                                                                                            |
| `quantity`         | `float`                    | Amount to buy/sell in `currency`.                                                                                                         |
| `delay`            | `int`, optional            | Delay in simulation nanoseconds before the order reaches the market.                                                                      |
| `clientOrderId`    | `int` or `None`, optional  | Optional client-specified order ID for tracking.                                                                                          |
| `stp`              | `STP`, optional            | Self-trade prevention strategy (`STP.CANCEL_OLDEST`, `STP.CANCEL_NEWEST`, `STP.CANCEL_BOTH`, `STP.DECREASE_CANCEL`). Defaults to `CANCEL_OLDEST`.                             |
| `currency`         | `OrderCurrency`, optional  | Currency to use for the order quantity (`OrderCurrency.BASE` or `OrderCurrency.QUOTE`). Defaults to BASE.                                 |

##### **Example**
```python
response.market_order(
    book_id=1,
    direction=OrderDirection.BUY,
    quantity=100.0,
    delay=50_000_000  # 50ms delay
)
```

---

#### `limit_order(...)`

Place a **limit order** at a specific price level.

##### **Signature**
```python
response.limit_order(
    book_id: int,
    direction: OrderDirection,
    quantity: float,
    price: float,
    delay: int = 0,
    clientOrderId: int | None = None,
    stp: STP = STP.CANCEL_OLDEST,
    postOnly: bool = False,
    timeInForce: TimeInForce = TimeInForce.GTC,
    expiryPeriod: int | None = None
)
```

##### **Arguments**

| Parameter         | Type                        | Description                                                                                                                                                                        |
|--------------------|-----------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `book_id`          | `int`                       | ID of the order book where the order will be placed.                                                                                                                              |
| `direction`        | `OrderDirection`            | `OrderDirection.BUY` or `OrderDirection.SELL`.                                                                                                                                     |
| `quantity`         | `float`                     | Quantity of the asset to trade.                                                                                                                                                    |
| `price`            | `float`                     | Price at which to place the limit order.                                                                                                                                           |
| `delay`            | `int`, optional             | Delay in simulation nanoseconds before the order reaches the market. Defaults to `0`.                                                                                              |
| `clientOrderId`    | `int` or `None`, optional   | Optional client-specified order ID for tracking.                                                                                                                                   |
| `stp`              | `STP`, optional             | Self-trade prevention strategy (`STP.CANCEL_OLDEST`, `STP.CANCEL_NEWEST`, `STP.CANCEL_BOTH`, `STP.DECREASE_CANCEL`). Defaults to `STP.CANCEL_OLDEST`.                                                                                                                      |
| `postOnly`         | `bool`, optional            | If True, prevents the order from matching immediately. Defaults to `False`.                                                                                                       |
| `timeInForce`      | `TimeInForce`, optional     | Time-in-force option (`TimeInForce.GTC`, `TimeInForce.GTT`, `TimeInForce.IOC`, `TimeInForce.FOK`). Defaults to `TimeInForce.GTC`.                                                                                                              |
| `expiryPeriod`     | `int` or `None`, optional   | Expiry period for `GTT` orders, in simulation nanoseconds.                                                                                                                         |

##### **Example**
```python
response.limit_order(
    book_id=1,
    direction=OrderDirection.SELL,
    quantity=50,
    price=101.25,
    timeInForce=TimeInForce.GTT,
    expiryPeriod=10_000_000_000  # 10 seconds
)
```

---

#### `cancel_order(...)`

Cancel a single order.

##### **Signature**
```python
response.cancel_order(
    book_id: int,
    order_id: int,
    quantity: float | None = None,
    delay: int = 0
)
```

##### **Arguments**

| Parameter     | Type                | Description                                                                                             |
|---------------|---------------------|---------------------------------------------------------------------------------------------------------|
| `book_id`      | `int`               | ID of the order book where the order exists.                                                            |
| `order_id`     | `int`               | ID of the order to cancel.                                                                              |
| `quantity`     | `float` or `None`      | Amount to cancel (if None, cancels the entire order).                                                   |
| `delay`        | `int`, optional     | Delay before the cancellation is processed. Defaults to `0`.                                            |

##### **Example**
```python
response.cancel_order(book_id=1, order_id=42)
```

---

#### `cancel_orders(...)`

Cancel multiple orders at once.

##### **Signature**
```python
response.cancel_orders(
    book_id: int,
    order_ids: list[int],
    delay: int = 0
)
```

##### **Arguments**

| Parameter     | Type           | Description                                                                                                |
|---------------|----------------|------------------------------------------------------------------------------------------------------------|
| `book_id`      | `int`          | ID of the order book where the orders exist.                                                               |
| `order_ids`    | `list[int]`    | List of order IDs to cancel.                                                                               |
| `delay`        | `int`, optional| Delay before the cancellations are processed. Defaults to `0`.                                             |

##### **Example**
```python
response.cancel_orders(book_id=1, order_ids=[42, 43, 44])
```

---

### Timeout

The query logic of validators enforces a timeout which specifies how long miners may take at maximum to respond to state updates.  The exact value of the timeout is subject to change, and is set in the [validator config](/taos/im/config/__init__.py) as `neuron.timeout` (see the `default` value for the current active setting).  If a response is not received within the timeout, no instructions will be submitted to the simulator for that agent.  It is the miner agent's responsibility to ensure that they receive, decompress and process the state update, as well as generate and return instructions, before the timeout expires.  This requires to allocate sufficient resources (CPU and network bandwidth) and optimize data analysis and other processes involved in trading decision making, and it will also benefit the miner to locate nearby to key validators.

---

### Latency

In real-world trading, there’s always some delay between submitting an order and it reaching the exchange. The simulator models this **latency** to make the simulation more realistic.

#### Response Time

When you submit instructions as a miner agent, they’re not executed immediately. Instead:  

- The simulator applies a **processing delay** to your instructions.  
- This delay is based on how quickly your agent responds to the validator’s state update.  
- **Faster responses mean your instructions are executed sooner** than those of slower agents.

The logic for calculating these delays is defined in the [`set_delays`](/taos/im/validator/reward.py) function.  

#### The `delay` Parameter

You can also manually add a **custom delay** to your instructions. This lets you schedule actions to happen later within the current simulation interval, rather than all at once:

```python
# Place an order immediately when the simulator receives your response
# This will be processed 0 simulation nanoseconds after the response time latency 
# determined by `set_delays` has elapsed
response.limit_order(
    book_id=0,
    direction=OrderDirection.BUY,
    quantity=1.0,
    price=301.25,
    delay=0
)

# Place another order 500ms after the instruction is received
response.limit_order(
    book_id=0,
    direction=OrderDirection.BUY,
    quantity=1.0,
    price=301.00,
    delay=500_000_000_000  # nanoseconds
)

# Place a third order 1.5 after the instruction is received
response.limit_order(
    book_id=0,
    direction=OrderDirection.BUY,
    quantity=1.0,
    price=300.75,
    delay=1_500_000_000_000
)

# Place a fourth order 3 after the instruction is received
response.limit_order(
    book_id=0,
    direction=OrderDirection.BUY,
    quantity=1.0,
    price=300.50,
    delay=3_000_000_000_000
)
```
This allows to attempt to take advantage of movements in price during the period between state updates, where otherwise miner agents are not able to participate.  The delay you specify is added on top of your agent’s response time latency. To maximize your advantage, you should aim to respond quickly as well as carefully scheduling your actions to take advantage of price movements within the publishing interval.

---

### Trading Volume

Trading volume plays an important role in determining the rewards assigned to a miner agent.  The trading volume of an agent is defined as the total value in QUOTE of the quantity of orders submitted by the agent which are matched by the simulator.  For example, if a miner submits an instruction to place an order with a quantity of `2.0`, and this order is matched at price `300.00`, their trading volume is increased by `600.00`.  This is true regardless of the role of the agent's order in the trade as either maker or taker.

#### Contribution to Reward

The primary component of incentive mechanism of the subnet is the risk-adjusted performance of the strategy, which is calculated using an intra-day Sharpe ratio where the returns are obtained as the difference in total inventory value held by the agent between subsequent state updates (see [rewarding logic](/taos/im/validator/reward.py)).  

In order to avoid rewarding miners who do not participate in active trading, as well as to incentivize the creation of volume in the simulated market, the calculated Sharpe values are then scaled by a factor derived using the agent's total traded volume over a [configured period](/taos/im/config/__init__.py) (see `scoring.activity.trade_volume_assessment_period`) of simulation time.  It should further be noted that, although any amount of traded volume within each `scoring.activity.trade_volume_sampling_interval` will trigger the volume factor to be assigned a value based on the traded volume during the assessment period, if no trades occur in the previous sampling interval then the volume factor will start to decay.  This is designed to prevent miners from producing a burst of trading activity once within the assessment period, and then stopping trading activity to benefit from the volume factor without maintaining consistent active trading. 

The inclusion of this factor in the scoring has the effect of magnifying the Sharpe ratios for miners which have executed more volume during the assessment period.  If the agent achieves a good Sharpe ratio while also trading significant volume, they will be rewarded more highly than a miner with the same performance at a lower traded volume.  Similarly, if a miner has high volume and poor Sharpe ratio, they will receive a worse score than a miner with the same performance and lower volume.  This discourages maximization of volume without sufficient regard for the performance, while also incentivizing the deployment of strategies which are both optimally risk-managed and highly active.

#### Trading Limitation

The validator logic also implements a cap on the maximum amount that can be traded within the assessment window.  This is intended to limit attempts to attain high volume-weighted scores during periods of good performance by recklessly trading purely for the sake of volume creation.  The limit is configured as a multiplier on the value of the initial capital allocated to miner agents; the multiplier is configured by validators using the `scoring.activity.capital_turnover_cap` parameter, where the value of initial capital allocated is configured in the simulator and can be read from the state update `config` field `miner_wealth`.  

Explicitly, if a miner has traded more than `scoring.activity.capital_turnover_cap * state.config.miner_wealth` in volume on a particular book over the `scoring.activity.trade_volume_assessment_period`, they will not be able to submit any more instructions to that book (other than cancellations) until their total volume over the preceding assessment period drops below this limit.

---

## Agent Testing

### Local

You can debug and test your agent logic offline before deploying to testnet or mainnet by making use of the facilities documented [here](/agents/proxy/README.md).  This setup allows to launch the simulator on your machine, and receive messages to the agent via a proxy which fulfils the role of the validator in a local setting.

### Testnet (Netuid 366)

Once you are satisfied that your agent logic works as intended, we recommend to register a UID on testnet (netuid 366) and deploy your miner as you intend to host it in mainnet environment.  This allows to confirm that all is properly configured for communication with validators, and the resources allocated to the miner are sufficient.  You can request testnet TAO at the [Bittensor Discord](https://discord.com/channels/799672011265015819/1389370202327748629).

### Mainnet (Netuid 79)

If all looks to be functioning well in testnet, register a UID on mainnet netuid 79 and restart your miner using the mainnet endpoint and your registered hotkey.  If you encounter issues, our team monitors the [τaos channel](https://discord.com/channels/799672011265015819/1353733356470276096) at BT Discord server.

Good luck!

---
---

## Appendix

### The `MarketSimulationStateUpdate` Class

---

- `version`

  This field is included to identify which version of the **taos** package the validator who sent the request is running.
  Miners do not need to worry about this generally; it is mainly to ensure backward compatibility during subnet development.

  **Type:** `int | None`

---

- `timestamp`

  The simulation timestamp at which the state was generated.
  This is represented as the number of nanoseconds since the start of the simulation.

  **Type:** `int`

---

- `config`

  Contains details of the simulation configuration ([`MarketSimulationConfig`](/taos/im/protocol/models.py)) used by the sending validator.  Includes simulation parameters, fee settings, and agent configurations.

  The fields which are important for miners are:

  - `config.baseDecimals` : Decimal precision of BASE currency values
  - `config.quoteDecimals` : Decimal precision of QUOTE currency values
  - `config.priceDecimals` : Decimal precision of prices (important when setting limit order price - input value will be rounded to this many decimals if specified to higher precision)
  - `config.volumeDecimals` : Decimal precision of volumes (important when setting order quantities - input value will be rounded to this many decimals if specified to higher precision)
  - `config.fee_policy` : The fee policy applied in the simulation
  - `config.max_open_orders` : The maximum number of orders that any agent in the simulation can simultaneously have open on the book.
  - `config.miner_wealth` : The total QUOTE value of the initial capital allocated to miners at start of simulation; this is used in determining the trading volume cap (see [Volume Limit](#volume-limit)).


  **Type:** `MarketSimulationConfig | str | None`

---

- `books`

  A dictionary mapping order book IDs to `Book` objects, which represent the state of each simulated order book at the time of the state publish event.

  **Type:** `dict[int, Book] | None`

  ---
  * **`Book`**

    Represents an order book at a specific point in time, including price levels and recent events.

    - `id`

      The unique identifier for this order book.

      **Type:** `int`

    - `bids`

      A list of bid price levels (`LevelInfo`) in descending order.

      **Type:** `list[LevelInfo]`

    - `asks`

      A list of ask price levels (`LevelInfo`) in descending order.

      **Type:** `list[LevelInfo]`

      ---

      **`LevelInfo`**
        - **`price`**
      
          The price of the bid level.

          **Type:** `float`

        - **`quantity`**
      
          The total quantity available at this bid price level.

          **Type:** `float`

        - **`orders`**
      
          A list of orders at this bid level (only present for top `config.detailedDepth` levels).

          **Type:** `list[Order] | None`

          ---
      
          **`Order`**
          - **`id`**

            The simulator-assigned ID of the order.

            **Type:** `int`

          - **`client_id`**

            The user-assigned client ID of the order.

            **Type:** `int | None`

          - **`timestamp`**

            The simulation timestamp at which the order was placed.

            **Type:** `int` 

          - **`quantity`**
        
            The remaining size of the order (in BASE).

            **Type:** `float` 

          - **`side`**

            The direction of the order, either `OrderDirection.BUY=0` or `OrderDirection.SELL=1`.

            **Type:** `int` 

          - **`price`**
            The price at which the order was placed.

            **Type:** `float` 

        ---

    ---

    - **`events`**

      A list of events that have occurred in the order book since the last snapshot.
      These may include `Order`, `TradeInfo`, and `Cancellation` entries.

      **Type:** `list[Order | TradeInfo | Cancellation] | None`

      ---

        **`Order`**

        - **`id`**

          The simulator-assigned ID of the order.

          **Type:** `int`

        - **`client_id`**
          The user-assigned client ID of the order.

          **Type:** `int | None`

        - **`timestamp`**
          The simulation timestamp at which the order was placed.

          **Type:** `int` 

        - **`quantity`**
          The remaining size of the order (in BASE).

          **Type:** `float` 

        - **`side`**
          The direction of the order, either `OrderDirection.BUY=0` or `OrderDirection.SELL=1`.

          **Type:** `int` 

        - **`price`**
          The price at which the order was placed.

          **Type:** `float` 

      ---
  
        **`TradeInfo`**

        - **`id`**  
          The simulator-assigned ID of the trade.  

          **Type:** `int`

        - **`side`**  
          Direction in which the trade was initiated.  
          `0` means BUY initiated, `1` means SELL initiated.  

          **Type:** `int`

        - **`timestamp`**  
          The simulation timestamp at which the trade occurred.  

          **Type:** `int`

        - **`taker_id`**  
          The ID of the aggressing order (the order initiating the trade).  

          **Type:** `int`

        - **`taker_agent_id`**  
          The ID of the agent who placed the aggressing order.  

          **Type:** `int`

        - **`taker_fee`**  
          The fee paid by the taker on this trade.

          **Type:** `float | None`

        - **`maker_id`**  
          The ID of the resting order (the order providing liquidity).  

          **Type:** `int`

        - **`maker_agent_id`**  
          The ID of the agent who placed the resting order.  

          **Type:** `int`

        - **`maker_fee`**  
          The fee paid by the maker on this trade.

          **Type:** `float | None`

        - **`quantity`**  
          The quantity traded (in base currency units).  

          **Type:** `float`

        - **`price`**  
          The price at which the trade occurred.  

          **Type:** `float`

      ---

      **`Cancellation`**

        - **`orderId`**  
          The ID of the cancelled order.  

          **Type:** `int`

        - **`timestamp`**  
          The simulation timestamp when the cancellation occurred.

          **Type:** `int`

        - **`price`**  
          The price of the cancelled order.

          **Type:** `float`

        - **`quantity`**  
          The quantity that was cancelled.

          **Type:** `float`
    ---

---

- `accounts`

  A dictionary mapping agent IDs to their trading accounts.
  Each agent maps to a dictionary of book IDs to `Account` objects.

  **Type:** `dict[int, dict[int, Account]] | None`

  ---
  * **`Account`**

    Represents an agent’s trading account on a specific order book.

    - **`agent_id`**

      The ID of the agent that owns this account.

      **Type:** `int`

    - **`book_id`**

      The ID of the order book where this account is active.

      **Type:** `int`

    - **`base_balance`**

      Represents the agent’s balance in the base currency.

      **Type:** `Balance`

    - **`quote_balance`**

      Represents the agent’s balance in the quote currency.

      **Type:** `Balance`

      ---

      **`Balance`**

      - **`currency`**

        The currency symbol (e.g., BTC).

        **Type:** `str`

      - **`total`**

        Total balance in this currency.

        **Type:** `float`

      - **`free`**

        Free balance available for trading.

        **Type:** `float`

      - **`reserved`**

        Reserved balance tied up in open orders.

        **Type:** `float`
    
      ---

    - **`orders`**

      A list of the agent’s currently open orders.

      **Type:** `list[Order]`

    - **`fees`**

      The fee rates applicable to this account.

      **Type:** `Fees | None`

      ---

      **`Fees`**

        - **`volume_traded`**
        
          Total volume traded by this agent for tiered fee assignment.

          **Type:** `float`

        - **`maker_fee_rate`**
        
          Current maker fee rate for the agent.

          **Type:** `float`

        - **`taker_fee_rate`**
        
          Current taker fee rate for the agent.

          **Type:** `float`

      ---

  ---

- `notices`

  A dictionary mapping agent IDs to a list of market events that occurred since the last update.

  **Type:** `dict[int, list[SimulationStartEvent | LimitOrderPlacementEvent | MarketOrderPlacementEvent | OrderCancellationsEvent | TradeEvent | ResetAgentsEvent | SimulationEndEvent]] | None`

  ---
  * **`SimulationStartEvent`**

    Represents the event generated on simulation start.

  ---

  * **`SimulationEndEvent`**

    Represents the event generated on simulation end.

  ---

  * **`OrderPlacementEvent`**

    Base class for events representing placement of an order in the simulation.

    - **`bookId`**

      The ID of the order book on which the order was attempted to be placed.

      **Type:** `int`

    - **`orderId`**

      The ID assigned to the order by the simulator.

      **Type:** `int`

    - **`clientOrderId`**

      Optional agent-assigned identifier for the order.

      **Type:** `int | None`

    - **`side`**

      The side of the book on which the order was attempted to be placed (`0=BID`, `1=ASK`).

      **Type:** `int`

    - **`quantity`**

      The size of the order in base currency.

      **Type:** `float`

    - **`success`**

      Flag indicating whether the order was successfully placed.

      **Type:** `bool`

    - **`message`**

      A message describing the result of the placement attempt (e.g., error reason).

      **Type:** `str`

  ---

  * **`LimitOrderPlacementEvent`**

    Represents the event generated on placement of a Limit Order.
    Inherits all fields from `OrderPlacementEvent`, plus the below:

    - **`price`**

      The price level at which the order was attempted to be placed.

      **Type:** `float`

  ---

  * **`MarketOrderPlacementEvent`**

    Represents the event generated on placement of a Market Order.
    Inherits all fields from `OrderPlacementEvent`.


  ---

  * **`OrderCancellationsEvent`**

    Represents cancellation of multiple orders.

    - **`bookId`**

      The ID of the order book where cancellations were attempted.

      **Type:** `int | None`

    - **`cancellations`**

      A list of events for each individual order cancellation.

      **Type:** `list[OrderCancellationEvent]`

      ---  
    
        **OrderCancellationEvent**

        Represents cancellation of a single order.

        - **`timestamp`**

          The simulation timestamp at which cancellation was attempted.

          **Type:** `int`

        - **`bookId`**

          The ID of the order book where the cancellation was attempted.

          **Type:** `int`

        - **`orderId`**

          The ID of the order being cancelled.

          **Type:** `int`

        - **`quantity`**

          The quantity to be cancelled in base currency.  
          If `None`, the entire remaining size of the order is cancelled.

          **Type:** `float | None`

        - **`success`**

          Flag indicating whether the cancellation was successful.

          **Type:** `bool`

        - **`message`**

          A message describing the result of the cancellation attempt.

          **Type:** `str`

        ---

  ---

  * **`TradeEvent`**

    Represents a trade that occurred in the simulation.

    - **`bookId`**

      The ID of the order book where the trade occurred.

      **Type:** `int | None`

    - **`tradeId`**

      The simulator-assigned ID of the trade.

      **Type:** `int`

    - **`clientOrderId`**

      Optional client-assigned ID of the resting order that was traded.

      **Type:** `int | None`

    - **`takerAgentId`**

      The ID of the agent that placed the aggressing order.

      **Type:** `int`

    - **`takerOrderId`**

      The ID of the aggressing order.

      **Type:** `int`

    - **`takerFee`**

      Fee paid by the taker.

      **Type:** `float`

    - **`makerAgentId`**

      The ID of the agent that placed the resting order.

      **Type:** `int`

    - **`makerOrderId`**

      The ID of the resting order.

      **Type:** `int`

    - **`makerFee`**

      Fee paid by the maker.

      **Type:** `float`

    - **`side`**

      Direction of the trade (`0=BUY initiated`, `1=SELL initiated`).

      **Type:** `int`

    - **`price`**

      The price at which the trade occurred.

      **Type:** `float`

    - **`quantity`**

      The quantity traded in base currency.

      **Type:** `float`

  ---

  * **`ResetAgentsEvent`**

    Represents a batch reset of multiple agent accounts.

    - **`resets`**

      A list of `ResetAgentEvent` objects, each describing an individual agent reset.

      **Type:** `list[ResetAgentEvent]`

      ---

      **`ResetAgentEvent`**

        Represents the event generated when a single agent account is reset.

        - **`success`**

          Flag indicating whether the agent’s account was successfully reset.

          **Type:** `bool`

        - **`message`**

          Message associated with the reset operation.

          **Type:** `str`

      ---

  ---

- `response`

  A mutable field populated by the miner agent with a response containing instructions for the simulation.

  **Type:** `FinanceAgentResponse | None`

---

- `compressed`

  A compressed representation of the state data, used to reduce transmission size.

  **Type:** `str | dict | None`

---

- `compression_engine`

  The library used for compressing the state data.
  Valid values: `"zlib"`, `"lz4"` (default=`lz4`).

  **Type:** `str`

---