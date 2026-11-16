import { Router } from "express";
import { db } from "../db/client";
import { plans } from "../db/schema";


export const plansRouter = Router();


plansRouter.get("/", async (_req, res) => {
    try {
        const rows = await db.select().from(plans);
        res.json(rows);

    } catch (error) {
        console.log("Error fetching plans:", error);
        res.status(500).json({ error: "Failed to fetch plans" });
    }
})