import { useMemo, useState } from "react";
import type { TelemetryEvent } from "../useTelemetry";

type Category = "traffic" | "quota" | "network" | "sessions";

const CATEGORY_LABEL: Record<Category, string> = {
    traffic: "Traffic",
    quota: "Quota",
    network: "Network mode",
    sessions: "Connections",
};

// Full class strings (not built dynamically) so Tailwind can see them.
const TONE = {
    green: "bg-emerald-500/15 text-emerald-300",
    blue: "bg-sky-500/15 text-sky-300",
    amber: "bg-amber-500/15 text-amber-300",
    red: "bg-rose-500/15 text-rose-300",
    purple: "bg-violet-500/15 text-violet-300",
    gray: "bg-gray-500/15 text-gray-300",
} as const;

type Tone = keyof typeof TONE;

function formatBytes(n: number): string {
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
    if (n < 1024 * 1024 * 1024) return `${(n / (1024 * 1024)).toFixed(1)} MB`;
    return `${(n / (1024 * 1024 * 1024)).toFixed(2)} GB`;
}

function describe(e: TelemetryEvent): {
    category: Category;
    label: string;
    tone: Tone;
    text: string;
} {
    switch (e.type) {
        case "NETWORK_ACTIVITY": {
            const failed = e.endReason === "connect_failed";
            return {
                category: "traffic",
                label: e.kind,
                tone: failed ? "red" : "blue",
                text: `${e.host}  ↑ ${formatBytes(e.up)}  ↓ ${formatBytes(e.down)}  (${e.endReason})`,
            };
        }
        case "QUOTA_UPDATE":
            return {
                category: "quota",
                label: "Quota",
                tone: "amber",
                text: `−${formatBytes(e.deducted)} ${e.direction}, ${formatBytes(e.newBalance)} left`,
            };
        case "QUOTA_EXHAUSTED":
            return { category: "quota", label: "Quota", tone: "red", text: "Data balance used up" };
        case "THROTTLE_STARTED":
            return { category: "quota", label: "Speed", tone: "red", text: "Speed limited to 64 kbps" };
        case "THROTTLE_STOPPED":
            return { category: "quota", label: "Speed", tone: "green", text: "Full speed restored" };
        case "RAT_CHANGED":
            return {
                category: "network",
                label: "Network",
                tone: "purple",
                text: `Switched to ${e.rat === "5G_NR" ? "5G NR" : "4G LTE"}`,
            };
        case "SUBSCRIBER_CONNECTED":
            return {
                category: "sessions",
                label: "Device",
                tone: "gray",
                text: `Connected, ${formatBytes(e.balance)} left, ${e.on5g ? "on 5G" : "on 4G"}${e.unlimited5g ? ", 5G unlimited plan" : ""
                    }`,
            };
    }
}

export function ActivityLog({ events }: { events: TelemetryEvent[] }) {
    // "Connections" fires on every new connection and is very chatty, so it starts hidden.
    const [shown, setShown] = useState<Set<Category>>(
        new Set<Category>(["traffic", "quota", "network"])
    );

    function toggle(c: Category) {
        setShown((prev) => {
            const next = new Set(prev);
            if (next.has(c)) next.delete(c);
            else next.add(c);
            return next;
        });
    }

    const rows = useMemo(
        () =>
            events
                .map((e) => ({ e, d: describe(e) }))
                .filter(({ d }) => shown.has(d.category))
                .reverse(), // newest first
        [events, shown]
    );

    return (
        <div className="bg-gray-900 rounded-lg p-4">
            <div className="flex flex-wrap items-center justify-between gap-2 mb-3">
                <h2 className="font-semibold">Activity</h2>
                <div className="flex flex-wrap gap-1.5">
                    {(Object.keys(CATEGORY_LABEL) as Category[]).map((c) => (
                        <button
                            key={c}
                            onClick={() => toggle(c)}
                            aria-pressed={shown.has(c)}
                            className={`text-xs rounded-full px-2.5 py-1 border transition-colors ${shown.has(c)
                                    ? "bg-gray-700 border-gray-500 text-gray-100"
                                    : "border-gray-700 text-gray-500 hover:text-gray-300"
                                }`}
                        >
                            {CATEGORY_LABEL[c]}
                        </button>
                    ))}
                </div>
            </div>

            {rows.length === 0 ? (
                <p className="text-sm text-gray-500 py-6 text-center">
                    Nothing to show yet. Browse on the bound device and events will appear here.
                </p>
            ) : (
                <ul className="max-h-[28rem] overflow-y-auto divide-y divide-gray-800 font-mono text-sm">
                    {rows.map(({ e, d }, i) => (
                        <li key={`${e.ts}-${i}`} className="flex items-baseline gap-3 py-1.5">
                            <time className="text-gray-500 shrink-0 tabular-nums">
                                {new Date(e.ts).toLocaleTimeString()}
                            </time>
                            <span className={`shrink-0 rounded px-1.5 py-0.5 text-xs ${TONE[d.tone]}`}>
                                {d.label}
                            </span>
                            <span className="text-gray-200 break-all">{d.text}</span>
                        </li>
                    ))}
                </ul>
            )}
        </div>
    );
}