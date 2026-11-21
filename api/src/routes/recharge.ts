import { Router } from "express";
import { z } from "zod";
import { randomUUID } from "crypto";
import { eq } from "drizzle-orm";
import { db } from "../db/client";
import { users, plans, rechargeTransactions } from "../db/schema";
import { config } from "../config";



export const rechargeRouter = Router();


const initiateSchema = z.object({
    msisdn: z.string(),
    planId: z.number().int().positive(),
});



rechargeRouter.post("/initiate", async (req, res) => {
    const parsed = initiateSchema.safeParse(req.body);

    if (!parsed.success) {
        return res.status(422).json({
            error: {
                code: "VALIDATION_ERROR",
                message: "Invalid request body",
                details: parsed.error.issues,
            }
        })
    }


    const { msisdn, planId } = parsed.data;

    const [plan] = await db.select().from(plans).where(eq(plans.id, planId));

    if (!plan) {
        return res.status(404).json({
            error: { code: "PLAN_NOT_FOUND", message: "No such plan" },
        });
    }


    const [user] = await db.select().from(users).where(eq(users.msisdn, msisdn));

    if (!user) {
        return res.status(404).json({
            error: { code: "USER_NOT_FOUND", message: "No such user" },
        });
    }


    const gatewayTxnId = `txn_${randomUUID()}`;

    const [txn] = await db
        .insert(rechargeTransactions)
        .values({
            userId: user.id,
            planId: plan.id,
            gatewayTxnId,
            amountInr: plan.priceInr,
        })

        .returning()


    res.status(201).json({
        gatewayTxnId: txn.gatewayTxnId,
        amountInr: txn.amountInr,
        status: txn.status,
        upiUri: `upi://pay?pa=netpulse@mock&am=${plan.priceInr}&tn=${txn.gatewayTxnId}`,
    })
})


// Mock payment gateway for development only. Off unless ENABLE_MOCK_PAYMENT=1 is set in .env.
// The browser must never hold the webhook secret, so this route calls the real webhook
// server-side, exactly like a payment gateway would.
if (process.env.ENABLE_MOCK_PAYMENT === "1") {
    const simulateSchema = z.object({
        gatewayTxnId: z.string().min(1).max(64),
        outcome: z.enum(["SUCCESS", "FAILED"]).default("SUCCESS"),
    });

    rechargeRouter.post("/simulate-payment", async (req, res) => {
        const parsed = simulateSchema.safeParse(req.body);
        if (!parsed.success) {
            return res.status(422).json({
                error: { code: "VALIDATION_ERROR", message: "Invalid request body", details: parsed.error.issues },
            });
        }
        const { gatewayTxnId, outcome } = parsed.data;

        const [txn] = await db
            .select()
            .from(rechargeTransactions)
            .where(eq(rechargeTransactions.gatewayTxnId, gatewayTxnId));
        if (!txn) {
            return res.status(404).json({
                error: { code: "TXN_NOT_FOUND", message: "Unknown transaction" },
            });
        }

        try {
            const r = await fetch(`http://localhost:${config.apiPort}/api/webhook/payment`, {
                method: "POST",
                headers: {
                    "Content-Type": "application/json",
                    "x-webhook-secret": config.webhookSecret,
                },
                body: JSON.stringify({ gatewayTxnId, status: outcome, amountInr: txn.amountInr }),
            });
            res.status(r.status).json(await r.json());
        } catch {
            res.status(502).json({
                error: { code: "WEBHOOK_UNREACHABLE", message: "Could not reach the payment webhook" },
            });
        }
    });
}