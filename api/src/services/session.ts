import jwt from "jsonwebtoken";
import { config } from "../config";

export type SessionClaims = { msisdn: string; uid: number };

export function signSession(claims: SessionClaims): string {
    return jwt.sign(claims, config.jwtSecret, { algorithm: "HS256", expiresIn: config.sessionTtlSeconds });
}

export function verifySession(token: string): SessionClaims | null {
    try {
        const p = jwt.verify(token, config.jwtSecret, { algorithms: ["HS256"] }) as jwt.JwtPayload;
        if (typeof p.msisdn === "string" && typeof p.uid === "number") return { msisdn: p.msisdn, uid: p.uid };
        return null;
    } catch {
        return null;
    }
}
