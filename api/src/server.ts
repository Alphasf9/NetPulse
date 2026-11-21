import express from "express";
import cors from "cors";
import { plansRouter } from "./routes/plans";
import { rechargeRouter } from "./routes/recharge";
import { webhookRouter } from "./routes/webhook";
import { authRouter } from "./routes/auth";
import { networkRouter } from "./routes/network";
import http from "http";
import { attachRealtimeBridge } from "./realtime/bridge";
import "./workers/cdrWorker";
import { config } from "./config";
import { corsOptions } from "./cors";

const app = express();

if (config.trustProxy) app.set("trust proxy", 1);

app.use(cors(corsOptions));

app.use(express.json());

const httpServer = http.createServer(app);

// Attach real-time Socket.IO bridge
attachRealtimeBridge(httpServer);

app.get("/health", (_req, res) => {
    res.json({ status: "ok" });
});

app.use("/api/plans", plansRouter);
app.use("/api/recharge", rechargeRouter);
app.use("/api/webhook", webhookRouter);
app.use("/api/auth", authRouter);
app.use("/api/network", networkRouter);

app.use(
    (
        err: unknown,
        _req: express.Request,
        res: express.Response,
        _next: express.NextFunction
    ) => {
        console.error(err);
        res.status(500).json({
            error: {
                code: "INTERNAL_ERROR",
                message: "Something went wrong",
            },
        });
    }
);

httpServer.listen(config.apiPort, () => {
    console.log(`API listening on http://localhost:${config.apiPort}`);
});
