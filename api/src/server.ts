import express from "express";
import cors from "cors";
import { plansRouter } from "./routes/plans";
import { rechargeRouter } from "./routes/recharge";
import { webhookRouter } from "./routes/webhook";
import { authRouter } from "./routes/auth";
import { networkRouter } from "./routes/network";
import { config } from "./config";

const app = express();

app.use(cors({ origin: config.corsOrigin }));
app.use(express.json());

app.get("/health", (_req, res) => {
    res.json({ status: "ok" });
});


app.use("/api/plans", plansRouter);
app.use("/api/recharge", rechargeRouter);
app.use("/api/webhook", webhookRouter);
app.use("/api/auth", authRouter);
app.use("/api/network", networkRouter);

app.use(
    (err: unknown, _req: express.Request, res: express.Response, _next: express.NextFunction) => {
        console.error(err);
        res.status(500).json({
            error: { code: "INTERNAL_ERROR", message: "Something went wrong" },
        });
    }
);


app.listen(config.apiPort, () => {
    console.log(`API listening on http://localhost:${config.apiPort}`);
});