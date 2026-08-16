#pragma once
#ifndef OPTIONX_HEADER_DATA_TRADING_TRADE_SIGNAL_HPP_INCLUDED
#define OPTIONX_HEADER_DATA_TRADING_TRADE_SIGNAL_HPP_INCLUDED

/// \file TradeSignal.hpp
/// \brief Defines the TradeSignal class with signal intent and money-management data.

namespace optionx {

    /// \class TradeSignal
    /// \brief Represents a strategy signal that may later produce one or more trade requests.
    class TradeSignal {
    public:
        // Signal identity
        SignalId signal_id = 0;              ///< Persistent signal ID; 0 means "not assigned".
        BridgeId bridge_id = 0;              ///< Source bridge ID; 0 means "not assigned".
        std::int64_t unique_id = 0;           ///< External/runtime signal identifier.
        std::string unique_hash;              ///< External/runtime signal hash.

        // Context
        PlatformType platform_type = PlatformType::UNKNOWN; ///< Target platform, if known.
        AccountType account_type = AccountType::UNKNOWN;    ///< Target account type, if known.
        CurrencyType currency = CurrencyType::UNKNOWN;      ///< Target account currency, if known.
        std::int64_t account_id = 0;                        ///< Target account ID, if known.
        std::string symbol;                                 ///< Trading symbol.
        std::string signal_name;                            ///< Signal or strategy name.
        std::string user_data;                              ///< User-defined signal data.
        std::string comment;                                ///< Optional signal comment.
        std::int64_t source_time_ms = 0;                    ///< Source publication timestamp in Unix milliseconds.

        // Suggested trade parameters
        OptionType option_type = OptionType::UNKNOWN;       ///< Suggested option type.
        OrderType order_type = OrderType::UNKNOWN;          ///< Suggested direction.
        double amount = 0.0;                                ///< Suggested initial trade amount.
        double refund = 0.0;                                ///< Suggested refund ratio.
        double min_payout = 0.0;                            ///< Minimum acceptable payout.
        std::uint32_t duration = 0;                         ///< Suggested duration in seconds; 0 means not specified.
        std::int64_t expiry_time = 0;                       ///< Suggested classic expiry timestamp in seconds.

        // Money-management context
        MmSystemType mm_type = MmSystemType::NONE;          ///< Money management system type.
        std::int32_t mm_step = 0;                           ///< Generic money management step.
        std::int64_t mm_group_id = 0;                       ///< Numeric money management group.
        std::string mm_group_hash;                          ///< Hash of the money management group.
        std::string mm_group_name;                          ///< Human-readable group name.
        std::unique_ptr<IMoneyManagementParams> mm_params;  ///< Money management parameters.
        std::unique_ptr<ITradeDecisionParams> decision_params; ///< Decision-making parameters.

        /// \brief Assigns signal fields to an executable trade request.
        /// \details The request gets signal identity and the suggested trade
        /// parameters, but runtime fields such as callbacks are untouched.
        void assign_to_request(TradeRequest& request) const {
            request.signal_id = signal_id;
            request.bridge_id = bridge_id;
            request.unique_id = unique_id;
            request.unique_hash = unique_hash;
            request.account_id = account_id;
            request.account_type = account_type;
            request.currency = currency;
            request.symbol = symbol;
            request.signal_name = signal_name;
            request.user_data = user_data;
            request.comment = comment;
            request.source_time_ms = source_time_ms;
            request.option_type = option_type;
            request.order_type = order_type;
            request.amount = amount;
            request.refund = refund;
            request.min_payout = min_payout;
            request.duration = duration;
            request.expiry_time = expiry_time;
        }

        /// \brief Builds an executable trade request from this signal.
        TradeRequest to_trade_request() const {
            TradeRequest request;
            assign_to_request(request);
            return request;
        }

        /// \brief Sets the money management parameters for the trade signal.
        ///
        /// If `params` is valid, it is stored and the `mm_type` is updated accordingly.
        /// If `params` is null, the money management type is reset to NONE.
        ///
        /// \param params A unique pointer to money management parameters.
        void set_money_management(std::unique_ptr<IMoneyManagementParams> params) {
            if (params) {
                mm_type = params->get_type();
                mm_params = std::move(params);
            } else {
                mm_type = MmSystemType::NONE;
                mm_params.reset();
            }
        }

        /// \brief Creates a deep copy of the trade signal.
        ///
        /// This method clones parameter objects when they exist.
        ///
        /// \return A unique pointer to the cloned `TradeSignal`.
        std::unique_ptr<TradeSignal> clone() const {
            auto cloned = std::make_unique<TradeSignal>();
            cloned->signal_id = signal_id;
            cloned->bridge_id = bridge_id;
            cloned->unique_id = unique_id;
            cloned->unique_hash = unique_hash;
            cloned->platform_type = platform_type;
            cloned->account_type = account_type;
            cloned->currency = currency;
            cloned->account_id = account_id;
            cloned->symbol = symbol;
            cloned->signal_name = signal_name;
            cloned->user_data = user_data;
            cloned->comment = comment;
            cloned->source_time_ms = source_time_ms;
            cloned->option_type = option_type;
            cloned->order_type = order_type;
            cloned->amount = amount;
            cloned->refund = refund;
            cloned->min_payout = min_payout;
            cloned->duration = duration;
            cloned->expiry_time = expiry_time;
            cloned->mm_type = mm_type;
            cloned->mm_step = mm_step;
            cloned->mm_group_id = mm_group_id;
            cloned->mm_group_hash = mm_group_hash;
            cloned->mm_group_name = mm_group_name;
            if (mm_params) {
                cloned->mm_params = mm_params->clone();
            }
            if (decision_params) {
                cloned->decision_params = decision_params->clone();
            }
            return cloned;
        }
    };

} // namespace optionx

namespace nlohmann {

    /// \brief Custom JSON serializer for TradeSignal.
    template <>
    struct adl_serializer<optionx::TradeSignal> {
        /// \brief Serializes a TradeSignal object to JSON.
        /// \param j The JSON object to populate.
        /// \param signal The TradeSignal object to convert.
        static void to_json(json& j, const optionx::TradeSignal& signal) {
            j = json{
                {"signal_id", signal.signal_id},
                {"bridge_id", signal.bridge_id},
                {"unique_id", signal.unique_id},
                {"unique_hash", signal.unique_hash},
                {"platform_type", signal.platform_type},
                {"account_type", signal.account_type},
                {"currency", signal.currency},
                {"account_id", signal.account_id},
                {"symbol", signal.symbol},
                {"signal_name", signal.signal_name},
                {"user_data", signal.user_data},
                {"comment", signal.comment},
                {"source_time_ms", signal.source_time_ms},
                {"option_type", signal.option_type},
                {"order_type", signal.order_type},
                {"amount", signal.amount},
                {"refund", signal.refund},
                {"min_payout", signal.min_payout},
                {"duration", signal.duration},
                {"expiry_time", signal.expiry_time},
                {"mm_type", signal.mm_type},
                {"mm_step", signal.mm_step},
                {"mm_group_id", signal.mm_group_id},
                {"mm_group_hash", signal.mm_group_hash},
                {"mm_group_name", signal.mm_group_name},
                {"mm_params", signal.mm_params ? signal.mm_params->to_json() : nullptr},
                {"decision_params", signal.decision_params ? signal.decision_params->to_json() : nullptr}
            };
        }

        /// \brief Deserializes a TradeSignal object from JSON.
        /// \param j The JSON object to read.
        /// \param signal The TradeSignal object to populate.
        static void from_json(const json& j, optionx::TradeSignal& signal) {
            signal.signal_id = j.value("signal_id", optionx::SignalId{0});
            signal.bridge_id = j.value("bridge_id", optionx::BridgeId{0});
            signal.unique_id = j.value("unique_id", std::int64_t{0});
            signal.unique_hash = j.value("unique_hash", std::string());
            signal.platform_type = j.value("platform_type", optionx::PlatformType::UNKNOWN);
            signal.account_type = j.value("account_type", optionx::AccountType::UNKNOWN);
            signal.currency = j.value("currency", optionx::CurrencyType::UNKNOWN);
            signal.account_id = j.value("account_id", std::int64_t{0});
            signal.symbol = j.value("symbol", std::string());
            signal.signal_name = j.value("signal_name", std::string());
            signal.user_data = j.value("user_data", std::string());
            signal.comment = j.value("comment", std::string());
            signal.source_time_ms = j.value("source_time_ms", std::int64_t{0});
            signal.option_type = j.value("option_type", optionx::OptionType::UNKNOWN);
            signal.order_type = j.value("order_type", optionx::OrderType::UNKNOWN);
            signal.amount = j.value("amount", 0.0);
            signal.refund = j.value("refund", 0.0);
            signal.min_payout = j.value("min_payout", 0.0);
            signal.duration = j.value("duration", std::uint32_t{0});
            signal.expiry_time = j.value("expiry_time", std::int64_t{0});
            signal.mm_type = j.value("mm_type", optionx::MmSystemType::NONE);
            signal.mm_step = j.value("mm_step", std::int32_t{0});
            signal.mm_group_id = j.value("mm_group_id", std::int64_t{0});
            signal.mm_group_hash = j.value("mm_group_hash", std::string());
            signal.mm_group_name = j.value("mm_group_name", std::string());

            if (j.contains("mm_params") && !j["mm_params"].is_null()) {
                // Typed money-management params are restored by higher-level strategy code.
            }
            if (j.contains("decision_params") && !j["decision_params"].is_null()) {
                // Typed decision params are restored by higher-level strategy code.
            }
        }
    };

} // namespace nlohmann

#endif // OPTIONX_HEADER_DATA_TRADING_TRADE_SIGNAL_HPP_INCLUDED
