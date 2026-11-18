import { db, pool } from "./client";
import { users } from "./schema";

async function main() {
  await db
    .insert(users)
    .values({ msisdn: "9999999999", name: "Demo User" })
    .onConflictDoNothing();
  console.log("Demo user ready");
  await pool.end();
}

main();