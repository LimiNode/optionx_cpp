#include <optionx_cpp/trading_system.hpp>

int main() {
    using namespace optionx::trading_system;
    RiskAdmissionConfig config;
    config.scope = {RiskScopeKind::ACCOUNT, 42};
    RiskAdmission owner(config);
    RiskAccountSnapshot account{
        RiskAvailability::SUPPORTED,
        MoneyAmount{1000, 2, optionx::CurrencyType::USD},
        1000,
        1};
    if (owner.update_snapshot(account).kind != RiskAdmissionKind::APPLIED) return 1;

    TradeIntent intent;
    intent.intent_id = 7;
    intent.idempotency_key = "consumer-key";
    intent.status = IntentStatus::ADMITTED;
    const auto request = RiskRequest::from_intent(
        intent,
        MoneyAmount{10, 2, optionx::CurrencyType::USD},
        MoneyAmount{25, 2, optionx::CurrencyType::USD});
    const auto reserved = owner.reserve(request, 1000);
    if (reserved.kind != RiskAdmissionKind::ADMITTED || !reserved.reservation) return 2;
    if (owner.commit(*reserved.reservation).kind != RiskAdmissionKind::APPLIED) return 3;
    if (owner.mark_unknown(*reserved.reservation).kind != RiskAdmissionKind::APPLIED) return 4;
    RiskSettlementEvidence evidence;
    evidence.reservation_id = reserved.reservation->reservation_id;
    evidence.intent_id = reserved.reservation->intent_id;
    evidence.execution_group = reserved.reservation->execution_group;
    evidence.outcome.final = true;
    evidence.outcome.all_members_terminal = true;
    evidence.outcome.completed_members = 1;
    evidence.outcome.aggregation_status = AggregationStatus::READY;
    evidence.outcome.kind = OutcomeKind::DRAW;
    evidence.outcome.net_profit = MoneyAmount{0, 2, optionx::CurrencyType::USD};
    TradeSettlement settlement;
    settlement.settlement_id = 100;
    settlement.intent_id = intent.intent_id;
    settlement.trade_id = 1;
    settlement.kind = OutcomeKind::DRAW;
    settlement.evidence = ExecutionEvidence::EXECUTED;
    settlement.net_profit = MoneyAmount{0, 2, optionx::CurrencyType::USD};
    evidence.settlements.push_back(settlement);
    if (owner.settle(*reserved.reservation, evidence).kind != RiskAdmissionKind::APPLIED) return 5;
    return owner.reserved_risk_units() == 0 ? 0 : 6;
}
