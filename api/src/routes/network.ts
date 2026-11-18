import { Router } from "express";
import { z } from "zod";
import { redis } from "../redis/client";

export const networkRouter = Router();

const ratSchema = z.object({
    msisdn: z.string().regex(/^\d{10}$/),
    rat: z.enum(["4G_LTE", "5G_NR"]),
});

networkRouter.post("/toggle-rat", async (req, res) => {
    const parsed = ratSchema.safeParse(req.body);
    if (!parsed.success) {
        return res.status(422).json({
            error: { code: "VALIDATION_ERROR", message: "Invalid request body", details: parsed.error.issues },
        });
    }
    const { msisdn, rat } = parsed.data;

    const hasPlan = await redis.exists(`user:${msisdn}:balance_bytes`);
    if (!hasPlan) {
        return res.status(404).json({
            error: { code: "NO_SUBSCRIPTION", message: "No active plan for this number" },
        });
    }

    await redis.set(`user:${msisdn}:rat_type`, rat);
    await redis.publish(
        "telecom:telemetry",
        JSON.stringify({ type: "RAT_CHANGED", msisdn, rat, ts: Date.now() })
    );

    res.json({ msisdn, rat });
});