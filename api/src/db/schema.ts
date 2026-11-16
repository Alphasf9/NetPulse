import {
    pgTable, serial, varchar, timestamp,
    integer, bigint, boolean,
    pgEnum
} from "drizzle-orm/pg-core";

export const users = pgTable("users", {
    id: serial("id").primaryKey(),
    msisdn: varchar("msisdn", { length: 15 }).notNull().unique(),
    name: varchar("name", { length: 100 }).notNull(),
    createdAt: timestamp("created_at").defaultNow().notNull(),
});


export const plans = pgTable("plans", {
    id: serial("id").primaryKey(),
    name: varchar("name", { length: 100 }).notNull(),
    priceInr: integer("price_inr").notNull(),
    dataQuotaBytes: bigint("data_quota_bytes", { mode: "number" }).notNull(),
    is5gUnlimited: boolean("is_5g_unlimited").default(false).notNull(),
    validityDays: integer("validity_days").notNull(),
});


export const txnStatus = pgEnum("txn_status", ["PENDING", "SUCCESS", "FAILED"]);


export const rechargeTransactions = pgTable("recharge_transactions", {
    id: serial("id").primaryKey(),
    userId: integer("user_id").notNull().references(() => users.id),
    planId: integer("plan_id").notNull().references(() => plans.id),
    gatewayTxnId: varchar("gateway_txn_id", { length: 64 }).notNull().unique(),
    amountInr: integer("amount_inr").notNull(),
    status: txnStatus("status").default("PENDING").notNull(),
    createdAt: timestamp("created_at").defaultNow().notNull(),
});