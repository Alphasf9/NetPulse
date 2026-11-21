import { useState } from "react";
import { api } from "../api";

export function NetworkToggle({
    on5g,
    onChanged,
}: {
    on5g: boolean;
    onChanged: (v: boolean) => void;
}) {
    const [busy, setBusy] = useState(false);

    async function toggle() {
        setBusy(true);
        try {
            const next = on5g ? "4G_LTE" : "5G_NR";
            await api.toggleRat(next);
            onChanged(next === "5G_NR");
        } catch (e) {
            alert((e as Error).message);
        } finally {
            setBusy(false);
        }
    }

    return (
        <div className="bg-gray-900 rounded-lg p-4 border border-gray-800">
            <div className="text-gray-400 text-sm mb-1">Network Mode</div>
            <button
                onClick={toggle}
                disabled={busy}
                className={`w-full py-2 rounded font-semibold ${on5g ? "bg-purple-700" : "bg-blue-700"} disabled:opacity-50`}
            >
                {on5g ? "5G NR" : "4G LTE"} — tap to switch
            </button>
        </div>
    );
}
