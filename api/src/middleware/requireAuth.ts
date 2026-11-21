import type { Request, Response, NextFunction } from "express";
import { verifySession, type SessionClaims } from "../services/session";

declare global {
    // eslint-disable-next-line @typescript-eslint/no-namespace
    namespace Express {
        interface Request {
            auth?: SessionClaims;
        }
    }
}

export function requireAuth(req: Request, res: Response, next: NextFunction) {
    const header = req.header("authorization") ?? "";
    const token = header.startsWith("Bearer ") ? header.slice(7) : "";
    const claims = token ? verifySession(token) : null;
    if (!claims) {
        return res.status(401).json({ error: { code: "UNAUTHORIZED", message: "Please log in again" } });
    }
    req.auth = claims;
    next();
}
