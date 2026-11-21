import { config } from "./config";

// In development, also accept private-network origins so a tablet/phone on the
// same Wi-Fi can open the dashboard at http://192.168.x.x:5173.
const PRIVATE_LAN =
    /^https?:\/\/(localhost|127\.0\.0\.1|192\.168\.\d{1,3}\.\d{1,3}|10\.\d{1,3}\.\d{1,3}\.\d{1,3}|172\.(1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3})(:\d+)?$/;

export function isAllowedOrigin(origin: string | undefined): boolean {
    if (!origin) return true; // curl / Postman
    if (config.corsOrigins.includes(origin)) return true;
    return !config.isProduction && PRIVATE_LAN.test(origin);
}

export const corsOptions = {
    origin: (origin: string | undefined, cb: (err: Error | null, allow?: boolean) => void) =>
        cb(null, isAllowedOrigin(origin)),
    credentials: true,
};
