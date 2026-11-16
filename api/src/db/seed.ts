import { db, pool } from "./client";
import { plans } from "./schema";

async function main() {
  await db.insert(plans).values([
    { name: "Data Booster", priceInr: 19, dataQuotaBytes: 25 * 1024 * 1024, is5gUnlimited: true, validityDays: 1 },
    { name: "4G Pack 50MB", priceInr: 10, dataQuotaBytes: 50 * 1024 * 1024, is5gUnlimited: false, validityDays: 1 },
    { name: "Daily 1.5GB", priceInr: 299, dataQuotaBytes: 1536 * 1024 * 1024, is5gUnlimited: true, validityDays: 28 },
  ]);
  console.log("Seeded plans");
  await pool.end();
}

main();