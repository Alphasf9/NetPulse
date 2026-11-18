import fs from "fs";
import path from "path";
import { redis } from "../redis/client";



const creditLua = fs.readFileSync(
    path.resolve(__dirname, "../../../redis/credit.lua"),
    "utf-8"
);


const MARKER_TTL_SECONDS = 7 * 24 * 60 * 60; // 7 days in seconds


export async function creditBundle(p: {
    gatewayTxnId: string,
    msisdn: string,
    bytes: number,
    has5g: boolean,
}) {
    const [credited, balance] = (await redis.eval(
        creditLua,
        3,
        `idemp:${p.gatewayTxnId}`,
        `user:${p.msisdn}:balance_bytes`,
        `user:${p.msisdn}:unlimited_5g`,
        p.bytes,
        p.has5g ? 1 : 0,
        MARKER_TTL_SECONDS
    )) as [number, number];

    return { credited: credited === 1, balance };
}