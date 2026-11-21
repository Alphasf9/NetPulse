import { Queue, Worker } from "bullmq";
import Redis from "ioredis";
import { redis } from "../redis/client";
import { db } from "../db/client";
import { usageCdrs } from "../db/schema";
import { config } from "../config";

// BullMQ requires its own Redis connection with maxRetriesPerRequest: null
// (it does blocking reads internally) — can't reuse the shared client.
const bullConnection = new Redis(config.redisUrl, { maxRetriesPerRequest: null });

const QUEUE_NAME = "cdr-sync-queue";
export const cdrQueue = new Queue(QUEUE_NAME, { connection: bullConnection });

async function drainRedisListIntoQueue() {
    while (true) {
        const raw = await redis.rpop("cdr:queue");
        if (!raw) break;
        await cdrQueue.add("cdr", JSON.parse(raw));
    }
}

setInterval(drainRedisListIntoQueue, 2000);

export const cdrSyncWorker = new Worker(
    QUEUE_NAME,
    async (job) => {
        const { msisdn, rat, up, down, host, end } = job.data;
        await db.insert(usageCdrs).values({
            msisdn,
            ratType: rat,
            bytesUp: up,
            bytesDown: down,
            destinationHost: host,
            endReason: end,
        });
    },
    { connection: bullConnection }
);

cdrSyncWorker.on("failed", (job, err) => {
    console.error("CDR job failed:", job?.id, err);
});