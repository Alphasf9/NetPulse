import { useState } from "react";
import { api } from "../api";

export function ConnectDevice({ boundIp, onBound }: { boundIp: string | null; onBound: (ip: string) => void }) {
    const [busy, setBusy] = useState(false);
    const [error, setError] = useState<string | null>(null);

    async function connect() {
        setBusy(true);
        setError(null);
        try {
            const r = await api.bindDevice();
            onBound(r.ip);
        } catch (e) {
            setError((e as Error).message);
        } finally {
            setBusy(false);
        }
    }

    return (
        <div className="bg-gray-900 rounded-lg p-4 border border-gray-800">
            <div className="text-gray-400 text-sm mb-1">Connected Device</div>
            <div className="text-sm mb-2">
                {boundIp ? (
                    <span className="text-emerald-400">Linked: <span className="font-mono">{boundIp}</span></span>
                ) : (
                    <span className="text-gray-300">No device linked yet</span>
                )}
            </div>
            <button
                onClick={connect}
                disabled={busy}
                className="w-full py-2 rounded font-semibold bg-gray-800 hover:bg-gray-700 disabled:opacity-50"
            >
                {busy ? "Linking..." : "Use this device for my number"}
            </button>
            <div className="mt-2 text-xs text-gray-500">
                Open this page on the device that uses the proxy, then tap the button.
            </div>
            {error && <div className="mt-2 text-xs text-rose-400">{error}</div>}
        </div>
    );
}
