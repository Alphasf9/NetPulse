import { Router } from "express";
import { z } from "zod";
import { timingSafeEqual } from "crypto";
import { and, eq } from "drizzle-orm";
import { db } from "../db/client";
import { users, plans, rechargeTransactions } from "../db/schema";
import { config } from "../config";
import { redis } from "../redis/client";
import { creditBundle } from "../services/billing";


export const webhookRouter = Router();


const webHookSchema = z.object({
    gatewayTxnId: z.string().min(1).max(64),
    status: z.enum(['SUCCESS', 'FAILED']),
    amountInr: z.number().int().positive()
});


function secretMatches(provided: string | undefined): boolean {
    if (!provided) return false;

    const a = Buffer.from(provided);
    const b = Buffer.from(config.webhookSecret);
    return a.length === b.length && timingSafeEqual(a, b);
}


webhookRouter.post("/payment", async (req, res) => {
    if (!secretMatches(req.header("x-webhook-secret"))) {
        return res.status(401).json({ error: "Invalid webhook secret", code: "UNAUTHORIZED" });
    }


    const parsed = webHookSchema.safeParse(req.body);

    if (!parsed.success) {
        return res.status(422).json({
            error: {
                code: "VALIDATION_ERROR",
                message: "Invalid webhook body",
                details: parsed.error.issues,
            },
        });
    }

    const { gatewayTxnId, status, amountInr } = parsed.data;

    const [row] = await db
        .select({ txn: rechargeTransactions, plan: plans, msisdn: users.msisdn })
        .from(rechargeTransactions)
        .innerJoin(plans, eq(plans.id, rechargeTransactions.planId))
        .innerJoin(users, eq(users.id, rechargeTransactions.userId))
        .where(eq(rechargeTransactions.gatewayTxnId, gatewayTxnId));

    if (!row) {
        return res.status(404).json({
            error: { code: "TXN_NOT_FOUND", message: "Unknown transaction" },
        });
    }

    if (row.txn.amountInr !== amountInr) {
        return res.status(422).json({
            error: { code: "AMOUNT_MISMATCH", message: "Amount does not match order" },
        });
    }


    if (status === "FAILED") {
        await db
            .update(rechargeTransactions)
            .set({ status: "FAILED" })
            .where(and(
                eq(rechargeTransactions.gatewayTxnId, gatewayTxnId),
                eq(rechargeTransactions.status, "PENDING")
            ))
        return res.json({ ok: true, status: "FAILED" });
    }

    if (row.txn.status === "FAILED") {
        return res.status(409).json({
            error: { code: "TXN_ALREADY_FAILED", message: "Transaction already failed" },
        })
    }


    const result = await creditBundle({
        gatewayTxnId,
        msisdn: row.msisdn,
        bytes: row.plan.dataQuotaBytes,
        has5g: row.plan.is5gUnlimited,
    })


    await redis.setnx(`user:${row.msisdn}:rat_type`, "4G_LTE");

    await db
        .update(rechargeTransactions)
        .set({ status: "SUCCESS" })
        .where(and(
            eq(rechargeTransactions.gatewayTxnId, gatewayTxnId),
            eq(rechargeTransactions.status, "PENDING")
        ))


    res.json({
        ok: true,
        status: "SUCCESS",
        duplicate: !result.credited,
        balanceBytes: result.balance,
    });

})


