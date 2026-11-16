import express from "express";
import cors from "cors";
import { plansRouter } from "./routes/plans";
import { config } from "./config";

const app = express();

app.use(cors({ origin: config.corsOrigin }));
app.use(express.json());

app.get("/health", (_req, res) => {
    res.json({ status: "ok" });
});


app.use("/api/plans", plansRouter);

app.listen(config.apiPort, () => {
    console.log(`API listening on http://localhost:${config.apiPort}`);
});