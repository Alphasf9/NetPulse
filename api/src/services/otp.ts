import { createHmac, randomInt } from "crypto";
import { redis } from "../redis/client";
import { config } from "../config";

export const OTP_TTL_SECONDS = 5 * 60;
export const OTP_MAX_ATTEMPTS = 5;
const RESEND_COOLDOWN_SECONDS = 30;
const SENDS_PER_NUMBER_PER_HOUR = 5;
const SENDS_PER_IP_PER_HOUR = 20;
const HOUR = 3600;

const otpKey = (msisdn: string) => `otp:${msisdn}`;

// Keyed hash: reading Redis is not enough to recover a 6-digit code.
function hashCode(msisdn: string, code: string): string {
    return createHmac("sha256", config.jwtSecret).update(`${msisdn}:${code}`).digest("hex");
}

// Atomic inside Redis, so parallel guesses cannot beat the attempt limit.
// 1 = correct, 0 = wrong, -1 = none/expired, -2 = locked out
const VERIFY_LUA = `
local stored = redis.call("HGET", KEYS[1], "h")
if not stored then return -1 end
local attempts = tonumber(redis.call("HGET", KEYS[1], "a") or "0")
if attempts >= tonumber(ARGV[2]) then
  redis.call("DEL", KEYS[1])
  return -2
end
if stored == ARGV[1] then
  redis.call("DEL", KEYS[1])
  return 1
end
attempts = redis.call("HINCRBY", KEYS[1], "a", 1)
if attempts >= tonumber(ARGV[2]) then
  redis.call("DEL", KEYS[1])
  return -2
end
return 0
`;

async function bump(key: string, ttlSeconds: number): Promise<number> {
    const n = await redis.incr(key);
    if (n === 1 || (await redis.ttl(key)) === -1) await redis.expire(key, ttlSeconds);
    return n;
}

export type IssueResult =
    | { ok: true; code: string }
    | { ok: false; reason: "COOLDOWN" | "RATE_LIMITED"; retryAfterSeconds: number };

export async function issueOtp(msisdn: string, ip: string): Promise<IssueResult> {
    const cooled = await redis.set(`otp:cooldown:${msisdn}`, "1", "EX", RESEND_COOLDOWN_SECONDS, "NX");
    if (cooled !== "OK") {
        const ttl = await redis.ttl(`otp:cooldown:${msisdn}`);
        return { ok: false, reason: "COOLDOWN", retryAfterSeconds: Math.max(ttl, 1) };
    }

    const ipCount = await bump(`otp:sends:ip:${ip}`, HOUR);
    const numCount = await bump(`otp:sends:num:${msisdn}`, HOUR);
    if (ipCount > SENDS_PER_IP_PER_HOUR || numCount > SENDS_PER_NUMBER_PER_HOUR) {
        const key = numCount > SENDS_PER_NUMBER_PER_HOUR ? `otp:sends:num:${msisdn}` : `otp:sends:ip:${ip}`;
        return { ok: false, reason: "RATE_LIMITED", retryAfterSeconds: Math.max(await redis.ttl(key), 1) };
    }

    const code = randomInt(0, 1_000_000).toString().padStart(6, "0");
    await redis
        .multi()
        .del(otpKey(msisdn))
        .hset(otpKey(msisdn), "h", hashCode(msisdn, code), "a", 0)
        .expire(otpKey(msisdn), OTP_TTL_SECONDS)
        .exec();

    return { ok: true, code };
}

export type VerifyResult = "OK" | "INVALID" | "EXPIRED" | "LOCKED";

export async function verifyOtp(msisdn: string, code: string): Promise<VerifyResult> {
    const r = (await redis.eval(VERIFY_LUA, 1, otpKey(msisdn), hashCode(msisdn, code), OTP_MAX_ATTEMPTS)) as number;
    if (r === 1) return "OK";
    if (r === 0) return "INVALID";
    if (r === -2) return "LOCKED";
    return "EXPIRED";
}

// Demo mode returns the code to the screen. Real mode must send an SMS here.
export async function deliverOtp(msisdn: string, code: string): Promise<{ demoCode?: string }> {
    if (config.demoOtp) {
        console.log(`[DEMO OTP] ${msisdn} -> ${code}`);
        return { demoCode: code };
    }
    // TODO: call your SMS provider (MSG91 / Fast2SMS / Twilio) here.
    throw new Error("No SMS provider configured. Set ENABLE_DEMO_OTP=1 for local demos.");
}
