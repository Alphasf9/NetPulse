import { Server as SocketIOServer } from "socket.io";
import type { Server as HTTPServer } from "http";
import Redis from "ioredis";
import { config } from "../config";
import { isAllowedOrigin } from "../cors";
import { verifySession } from "../services/session";

export function attachRealtimeBridge(httpServer: HTTPServer) {
    const io = new SocketIOServer(httpServer, {
        cors: {
            origin: (origin, cb) => cb(null, isAllowedOrigin(origin)),
            methods: ["GET", "POST"],
            credentials: true,
        },
    });

    // Only logged-in users may connect, and each one only receives their own events.
    io.use((socket, next) => {
        const token = socket.handshake.auth?.token;
        const claims = typeof token === "string" ? verifySession(token) : null;
        if (!claims) return next(new Error("UNAUTHORIZED"));
        socket.data.msisdn = claims.msisdn;
        socket.join(`sub:${claims.msisdn}`);
        next();
    });

    // A Redis connection in subscribe mode can't run other commands: dedicated connection.
    const subscriber = new Redis(config.redisUrl);

    subscriber.subscribe("telecom:telemetry", (err) => {
        if (err) console.error("Redis Pub/Sub subscription failed:", err);
        else console.log("Subscribed to Redis channel: telecom:telemetry");
    });

    subscriber.on("message", (_channel, message) => {
        try {
            const { msisdn } = JSON.parse(message);
            if (typeof msisdn === "string") io.to(`sub:${msisdn}`).emit("telemetry", message);
        } catch {
            // ignore malformed events
        }
    });

    subscriber.on("error", (err) => console.error("Redis subscriber error:", err));

    return io;
}
