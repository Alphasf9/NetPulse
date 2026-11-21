import { Router } from "express";
import { z } from "zod";
import { eq } from "drizzle-orm";
import { db } from "../db/client";
import { users } from "../db/schema";
import { redis } from "../redis/client";
import { requireAuth } from "../middleware/requireAuth";
import { issueOtp, verifyOtp, deliverOtp, OTP_TTL_SECONDS } from "../services/otp";
import { signSession } from "../services/session";

export const authRouter = Router();

const BINDING_TTL_SECONDS = 24 * 60 * 60;
const msisdnSchema = z.string().regex(/^\d{10}$/, "Enter a 10-digit mobile number");

function normalizeIp(raw: string | undefined): string {
    if (!raw) return "unknown";
    if (raw === "::1") return "127.0.0.1";
    return raw.replace(/^::ffff:/, "");
}

function validationError(res: import("express").Response, issues: unknown) {
    return res.status(422).json({
        error: { code: "VALIDATION_ERROR", message: "Invalid request body", details: issues },
    });
}

// ---------- Step 1: ask for a code ----------
const requestSchema = z.object({ msisdn: msisdnSchema });

authRouter.post("/request-otp", async (req, res) => {
    const parsed = requestSchema.safeParse(req.body);
    if (!parsed.success) return validationError(res, parsed.error.issues);

    const { msisdn } = parsed.data;
    const issued = await issueOtp(msisdn, normalizeIp(req.ip));

    if (!issued.ok) {
        res.setHeader("Retry-After", String(issued.retryAfterSeconds));
        return res.status(429).json({
            error: {
                code: issued.reason,
                message: `Too many requests. Try again in ${issued.retryAfterSeconds}s.`,
                retryAfterSeconds: issued.retryAfterSeconds,
            },
        });
    }

    // Same response whether or not the number is already registered.
    const delivery = await deliverOtp(msisdn, issued.code);
    res.json({ sent: true, expiresInSeconds: OTP_TTL_SECONDS, ...delivery });
});

// ---------- Step 2: prove ownership, get a session ----------
const verifySchema = z.object({
    msisdn: msisdnSchema,
    code: z.string().regex(/^\d{6}$/, "Code must be 6 digits"),
    name: z.string().trim().min(1).max(100).optional(),
});

authRouter.post("/verify-otp", async (req, res) => {
    const parsed = verifySchema.safeParse(req.body);
    if (!parsed.success) return validationError(res, parsed.error.issues);

    const { msisdn, code, name } = parsed.data;
    const result = await verifyOtp(msisdn, code);

    if (result !== "OK") {
        const map = {
            INVALID: [400, "That code is incorrect."],
            EXPIRED: [400, "That code has expired. Request a new one."],
            LOCKED: [429, "Too many wrong attempts. Request a new code."],
        } as const;
        const [status, message] = map[result];
        return res.status(status).json({ error: { code: `OTP_${result}`, message } });
    }

    // Number is proven. Create the account if this is a first login.
    await db
        .insert(users)
        .values({ msisdn, name: name ?? `Subscriber ${msisdn.slice(-4)}` })
        .onConflictDoNothing();

    const [user] = await db.select().from(users).where(eq(users.msisdn, msisdn));
    const token = signSession({ msisdn: user.msisdn, uid: user.id });

    res.json({ token, user: { msisdn: user.msisdn, name: user.name } });
});

// ---------- Who am I + current state ----------
authRouter.get("/me", requireAuth, async (req, res) => {
    const { msisdn } = req.auth!;
    const [user] = await db.select().from(users).where(eq(users.msisdn, msisdn));
    if (!user) {
        return res.status(401).json({ error: { code: "UNAUTHORIZED", message: "Please log in again" } });
    }

    const [balance, rat, boundIp] = await Promise.all([
        redis.get(`user:${msisdn}:balance_bytes`),
        redis.get(`user:${msisdn}:rat_type`),
        redis.get(`user:${msisdn}:bound_ip`),
    ]);

    res.json({
        msisdn: user.msisdn,
        name: user.name,
        hasPlan: balance !== null,
        balanceBytes: balance === null ? null : Number(balance),
        on5g: rat === "5G_NR",
        boundIp,
    });
});

// ---------- Connect THIS device to the logged-in number ----------
// The number comes from the login token, never from the request body.
authRouter.post("/bind-device", requireAuth, async (req, res) => {
    const { msisdn } = req.auth!;
    const ip = normalizeIp(req.ip);

    const [ipOwner, previousIp] = await Promise.all([
        redis.get(`ip:${ip}:msisdn`),
        redis.get(`user:${msisdn}:bound_ip`),
    ]);

    const tx = redis.multi();
    // One device per account: drop this account's old device binding
    if (previousIp && previousIp !== ip) tx.del(`ip:${previousIp}:msisdn`);
    // This device was bound to someone else: unlink them
    if (ipOwner && ipOwner !== msisdn) tx.del(`user:${ipOwner}:bound_ip`);
    tx.set(`ip:${ip}:msisdn`, msisdn, "EX", BINDING_TTL_SECONDS);
    tx.set(`user:${msisdn}:bound_ip`, ip, "EX", BINDING_TTL_SECONDS);
    await tx.exec();

    res.json({ msisdn, ip, expiresInSeconds: BINDING_TTL_SECONDS });
});
