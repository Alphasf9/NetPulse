// Default works from a tablet too: it uses whatever host the page was opened from.
const BASE =
    import.meta.env.VITE_API_URL ?? `${window.location.protocol}//${window.location.hostname}:4000`;

export const API_BASE = BASE;

export type Plan = {
    id: number;
    name: string;
    priceInr: number;
    dataQuotaBytes: number;
    is5gUnlimited: boolean;
    validityDays: number;
};

export type Session = { token: string; msisdn: string; name: string };

export type Me = {
    msisdn: string;
    name: string;
    hasPlan: boolean;
    balanceBytes: number | null;
    on5g: boolean;
    boundIp: string | null;
};

const STORAGE_KEY = "np_session";

export function getSession(): Session | null {
    try {
        const raw = localStorage.getItem(STORAGE_KEY);
        return raw ? (JSON.parse(raw) as Session) : null;
    } catch {
        return null;
    }
}

export function saveSession(s: Session) {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(s));
}

export function clearSession() {
    localStorage.removeItem(STORAGE_KEY);
}

async function request<T>(path: string, options: RequestInit = {}): Promise<T> {
    const token = getSession()?.token;
    const res = await fetch(`${BASE}${path}`, {
        ...options,
        headers: {
            "Content-Type": "application/json",
            ...(token ? { Authorization: `Bearer ${token}` } : {}),
            ...options.headers,
        },
    });
    const body = await res.json().catch(() => ({}));
    if (res.status === 401 && token) {
        clearSession();
        window.dispatchEvent(new Event("np:logout"));
    }
    if (!res.ok) {
        throw new Error(body?.error?.message ?? `Request failed: ${res.status}`);
    }
    return body;
}

export const api = {
    getPlans: () => request<Plan[]>("/api/plans"),

    requestOtp: (msisdn: string) =>
        request<{ sent: boolean; expiresInSeconds: number; demoCode?: string }>("/api/auth/request-otp", {
            method: "POST",
            body: JSON.stringify({ msisdn }),
        }),

    verifyOtp: (msisdn: string, code: string, name?: string) =>
        request<{ token: string; user: { msisdn: string; name: string } }>("/api/auth/verify-otp", {
            method: "POST",
            body: JSON.stringify({ msisdn, code, ...(name ? { name } : {}) }),
        }),

    me: () => request<Me>("/api/auth/me"),

    bindDevice: () =>
        request<{ msisdn: string; ip: string }>("/api/auth/bind-device", { method: "POST", body: "{}" }),

    toggleRat: (rat: "4G_LTE" | "5G_NR") =>
        request<{ msisdn: string; rat: string }>("/api/network/toggle-rat", {
            method: "POST",
            body: JSON.stringify({ rat }),
        }),

    initiateRecharge: (msisdn: string, planId: number) =>
        request<{ gatewayTxnId: string; amountInr: number; status: string }>("/api/recharge/initiate", {
            method: "POST",
            body: JSON.stringify({ msisdn, planId }),
        }),

    simulatePayment: (gatewayTxnId: string, outcome: "SUCCESS" | "FAILED") =>
        request<{ ok: boolean; status: "SUCCESS" | "FAILED"; duplicate?: boolean; balanceBytes?: number }>(
            "/api/recharge/simulate-payment",
            { method: "POST", body: JSON.stringify({ gatewayTxnId, outcome }) }
        ),
};
