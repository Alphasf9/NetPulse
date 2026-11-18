import { Router } from "express";
import { z } from "zod";
import { randomUUID } from "crypto";
import { eq } from "drizzle-orm";
import { db } from "../db/client";
import { users, plans, rechargeTransactions } from "../db/schema";



export const rechargeRouter = Router();


const initiateSchema = z.object({
    msisdn: z.string().regex(/^\d{10}$/),
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