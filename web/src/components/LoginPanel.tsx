import { useEffect, useState } from "react";
import { api, saveSession, type Session } from "../api";

export function LoginPanel({ onLoggedIn }: { onLoggedIn: (s: Session) => void }) {
    const [step, setStep] = useState<"number" | "code">("number");
    const [msisdn, setMsisdn] = useState("");
    const [name, setName] = useState("");
    const [code, setCode] = useState("");
    const [demoCode, setDemoCode] = useState<string | null>(null);
    const [busy, setBusy] = useState(false);
    const [error, setError] = useState<string | null>(null);
    const [cooldown, setCooldown] = useState(0);

    useEffect(() => {
        if (cooldown <= 0) return;
        const t = setTimeout(() => setCooldown((c) => c - 1), 1000);
        return () => clearTimeout(t);
    }, [cooldown]);

    async function sendCode() {
        setBusy(true);
        setError(null);
        try {
            const r = await api.requestOtp(msisdn);
            setDemoCode(r.demoCode ?? null);
            setCode("");
            setStep("code");
            setCooldown(30);
        } catch (e) {
            setError((e as Error).message);
        } finally {
            setBusy(false);
        }
    }

    async function verify() {
        setBusy(true);
        setError(null);
        try {
            const r = await api.verifyOtp(msisdn, code, name.trim() || undefined);
            const session: Session = { token: r.token, msisdn: r.user.msisdn, name: r.user.name };
            saveSession(session);
            onLoggedIn(session);
        } catch (e) {
            setError((e as Error).message);
        } finally {
            setBusy(false);
        }
    }

    const numberOk = /^\d{10}$/.test(msisdn);
    const codeOk = /^\d{6}$/.test(code);

    return (
        <div className="min-h-screen bg-gray-950 text-gray-100 flex items-center justify-center p-6">
            <div className="w-full max-w-sm bg-gray-900 border border-gray-800 rounded-lg p-6 space-y-4">
                <h1 className="text-xl font-bold">NetPulse</h1>

                {step === "number" ? (
                    <>
                        <div className="text-sm text-gray-400">Log in or create an account with your mobile number.</div>
                        <input
                            inputMode="numeric"
                            maxLength={10}
                            placeholder="10-digit mobile number"
                            value={msisdn}
                            onChange={(e) => setMsisdn(e.target.value.replace(/\D/g, ""))}
                            className="w-full rounded bg-gray-800 px-3 py-2 outline-none focus:ring-2 ring-cyan-600"
                        />
                        <input
                            maxLength={100}
                            placeholder="Your name (new accounts only)"
                            value={name}
                            onChange={(e) => setName(e.target.value)}
                            className="w-full rounded bg-gray-800 px-3 py-2 outline-none focus:ring-2 ring-cyan-600"
                        />
                        <button
                            disabled={!numberOk || busy}
                            onClick={sendCode}
                            className="w-full rounded py-2 font-semibold bg-cyan-700 hover:bg-cyan-600 disabled:opacity-50"
                        >
                            {busy ? "Sending..." : "Send code"}
                        </button>
                    </>
                ) : (
                    <>
                        <div className="text-sm text-gray-400">Enter the 6-digit code for {msisdn}.</div>
                        {demoCode && (
                            <div className="rounded bg-amber-950 border border-amber-800 text-amber-300 text-sm px-3 py-2">
                                Demo mode — your code is <span className="font-mono font-bold">{demoCode}</span>
                                <div className="text-xs text-amber-500">In production this arrives by SMS.</div>
                            </div>
                        )}
                        <input
                            inputMode="numeric"
                            maxLength={6}
                            autoFocus
                            placeholder="123456"
                            value={code}
                            onChange={(e) => setCode(e.target.value.replace(/\D/g, ""))}
                            className="w-full rounded bg-gray-800 px-3 py-2 tracking-widest text-center font-mono text-lg outline-none focus:ring-2 ring-cyan-600"
                        />
                        <button
                            disabled={!codeOk || busy}
                            onClick={verify}
                            className="w-full rounded py-2 font-semibold bg-cyan-700 hover:bg-cyan-600 disabled:opacity-50"
                        >
                            {busy ? "Checking..." : "Verify & log in"}
                        </button>
                        <div className="flex justify-between text-xs text-gray-400">
                            <button onClick={() => { setStep("number"); setError(null); }} className="hover:text-gray-200">
                                Change number
                            </button>
                            <button
                                disabled={cooldown > 0 || busy}
                                onClick={sendCode}
                                className="hover:text-gray-200 disabled:opacity-50"
                            >
                                {cooldown > 0 ? `Resend in ${cooldown}s` : "Resend code"}
                            </button>
                        </div>
                    </>
                )}

                {error && <div className="text-sm text-rose-400">{error}</div>}
            </div>
        </div>
    );
}
