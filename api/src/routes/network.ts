import { Router } from "express";
import { z } from "zod";
import { redis } from "../redis/client";
import { requireAuth } from "../middleware/requireAuth";

export const networkRouter = Router();

const ratSchema = z.object({
    rat: z.enum(["4G_LTE", "5G_NR"]),
});

// The number comes from the login token, so nobody can switch someone else's network.
networkRouter.post("/toggle-rat", requireAuth, async (req, res) => {
    const parsed = ratSchema.safeParse(req.body);
    if (!parsed.success) {
        return res.status(422).json({
            error: { code: "VALIDATION_ERROR", message: "Invalid request body", details: parsed.error.issues },
        });
    }
    const { rat } = parsed.data;
    const { msisdn } = req.auth!;

    const hasPlan = await redis.exists(`user:${msisdn}:balance_bytes`);
    if (!hasPlan) {
        return res.status(404).json({
            error: { code: "NO_SUBSCRIPTION", message: "No active plan for this number" },
        });
    }

    await redis.set(`user:${msisdn}:rat_type`, rat);
    await redis.publish("telecom:telemetry", JSON.stringify({ type: "RAT_CHANGED", msisdn, rat, ts: Date.now() }));

    res.json({ msisdn, rat });
});
