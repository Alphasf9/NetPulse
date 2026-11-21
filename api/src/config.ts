import dotenv from "dotenv";
import path from "path";

dotenv.config({
    path: path.resolve(__dirname, "../../.env"),
});

function required(name: string): string {
    const value = process.env[name];
    if (!value) throw new Error(`Missing required environment variable: ${name}`);
    return value;
}

const jwtSecret = required("JWT_SECRET");
if (jwtSecret.length < 32) {
    throw new Error("JWT_SECRET must be at least 32 characters long");
}

const isProduction = process.env.NODE_ENV === "production";

// Demo OTP shows the code on screen instead of sending an SMS. Never allowed in production.
const demoOtp = process.env.ENABLE_DEMO_OTP === "1";
if (demoOtp && isProduction) {
    throw new Error("ENABLE_DEMO_OTP=1 is not allowed when NODE_ENV=production");
}

export const config = {
    databaseUrl: required("DATABASE_URL_POOLED"),
    redisUrl: required("REDIS_URL"),
    apiPort: Number(process.env.API_PORT ?? 3000),
    // Comma-separated, e.g. "https://netpulse.example.com,http://localhost:5173"
    corsOrigins: (process.env.CORS_ORIGIN ?? "http://localhost:5173")
        .split(",")
        .map((s) => s.trim())
        .filter(Boolean),
    webhookSecret: required("WEBHOOK_SECRET"),
    jwtSecret,
    sessionTtlSeconds: 7 * 24 * 60 * 60,
    isProduction,
    demoOtp,
    // Set TRUST_PROXY=1 when the API is behind Render/nginx so req.ip is the real client.
    trustProxy: process.env.TRUST_PROXY === "1",
};
