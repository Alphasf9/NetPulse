import { Router } from "express";
import { z } from "zod";
import { eq } from "drizzle-orm";
import { db } from "../db/client";
import { users } from "../db/schema";
import { redis } from "../redis/client";

export const authRouter = Router();

const bindSchema = z.object({ msisdn: z.string().regex(/^\d{10}$/) });
const BINDING_TTL_SECONDS = 24 * 60 * 60;



function normalizeIp(raw: string | undefined): string {
    if (!raw) return "unknown";
    if (raw === "::1") return "127.0.0.1";
    return raw.replace(/^::ffff:/, "");
}


authRouter.post("/bind-device", async (req, res) => {
    const parsed = bindSchema.safeParse(req.body);
    if (!parsed.success) {
        return res.status(422).json({
            error: { code: "VALIDATION_ERROR", message: "Invalid request body", details: parsed.error.issues },
        });
    }


    const { msisdn } = parsed.data;

    const [user] = await db
        .select().from(users).where(eq(users.msisdn, msisdn));

    if (!user) {
        return res.status(404).json({
            error: { code: "USER_NOT_FOUND", message: "No such user" },
        });
    }

    const ip = normalizeIp(req.ip);

    await redis.set(`ip:${ip}:msisdn`, msisdn, "EX", BINDING_TTL_SECONDS);
    res.json({ msisdn, ip, expiresInSeconds: BINDING_TTL_SECONDS });

})