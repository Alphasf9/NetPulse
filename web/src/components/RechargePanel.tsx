import { useEffect, useState } from "react";
import { api, type Plan } from "../api";

type Order = { gatewayTxnId: string; amountInr: number; planName: string };
type Message = { text: string; ok: boolean };

export function RechargePanel({
    msisdn,
    onCredited,
}: {
    msisdn: string;
    onCredited: (balanceBytes: number) => void;
}) {
    const [plans, setPlans] = useState<Plan[]>([]);
    const [order, setOrder] = useState<Order | null>(null);
    const [busy, setBusy] = useState(false);
    const [message, setMessage] = useState<Message | null>(null);

    useEffect(() => {
        api.getPlans().then(setPlans).catch((e) => setMessage({ text: e.message, ok: false }));
    }, []);

    async function startOrder(plan: Plan) {
        setBusy(true);
        setMessage(null);
        try {
            const r = await api.initiateRecharge(msisdn, plan.id);
            setOrder({ gatewayTxnId: r.gatewayTxnId, amountInr: r.amountInr, planName: plan.name });
        } catch (e) {
            setMessage({ text: (e as Error).message, ok: false });
        } finally {
            setBusy(false);
        }
    }

    async function settle(outcome: "SUCCESS" | "FAILED") {
        if (!order) return;
        setBusy(true);
        try {
            const r = await api.simulatePayment(order.gatewayTxnId, outcome);
            if (r.status === "SUCCESS") {
                const balance = Number(r.balanceBytes);
                if (Number.isFinite(balance)) onCredited(balance);
                setMessage({
                    text: r.duplicate
                        ? "This payment was already credited. Balance unchanged."
                        : `Payment received. ${order.planName} added to your balance.`,
                    ok: true,
                });
            } else {
                setMessage({ text: "Payment failed. No data was added.", ok: false });
            }
            setOrder(null);
        } catch (e) {
            setMessage({ text: (e as Error).message, ok: false });
        } finally {
            setBusy(false);
        }
    }

    // Step 2: mock UPI screen
    if (order) {
        return (
            <div className="bg-gray-900 rounded-lg p-4 border border-gray-800">
                <div className="text-gray-400 text-sm mb-3">Complete payment</div>
                <div className="text-2xl font-semibold">₹{order.amountInr}</div>
                <div className="text-sm text-gray-300 mt-1">{order.planName}</div>
                <div className="text-xs text-gray-500 mt-1 break-all">Order {order.gatewayTxnId}</div>

                <div className="mt-4 flex gap-2">
                    <button
                        disabled={busy}
                        onClick={() => settle("SUCCESS")}
                        className="flex-1 rounded px-3 py-2 bg-emerald-600 hover:bg-emerald-500 disabled:opacity-50"
                    >
                        {busy ? "Processing..." : `Pay ₹${order.amountInr}`}
                    </button>
                    <button
                        disabled={busy}
                        onClick={() => settle("FAILED")}
                        className="rounded px-3 py-2 bg-gray-800 hover:bg-gray-700 text-rose-300 disabled:opacity-50"
                    >
                        Fail payment
                    </button>
                </div>
                <button
                    disabled={busy}
                    onClick={() => setOrder(null)}
                    className="mt-2 text-xs text-gray-500 hover:text-gray-300"
                >
                    Choose a different plan
                </button>
            </div>
        );
    }

    // Step 1: pick a plan
    return (
        <div className="bg-gray-900 rounded-lg p-4 border border-gray-800">
            <div className="text-gray-400 text-sm mb-2">Recharge Plans</div>
            <div className="space-y-2">
                {plans.map((p) => (
                    <button
                        key={p.id}
                        disabled={busy}
                        onClick={() => startOrder(p)}
                        className="w-full text-left bg-gray-800 hover:bg-gray-700 rounded px-3 py-2 disabled:opacity-50"
                    >
                        <div className="font-semibold">{p.name}</div>
                        <div className="text-xs text-gray-400">
                            ₹{p.priceInr} · {(p.dataQuotaBytes / (1024 * 1024)).toFixed(0)}MB
                            {p.is5gUnlimited ? " · 5G Unlimited" : ""}
                        </div>
                    </button>
                ))}
            </div>
            {message && (
                <div className={`mt-3 text-sm ${message.ok ? "text-emerald-400" : "text-rose-400"}`}>
                    {message.text}
                </div>
            )}
        </div>
    );
}