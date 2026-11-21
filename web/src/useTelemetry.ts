/* eslint-disable react-hooks/set-state-in-effect */
import { useEffect, useRef, useState } from "react";
import { io } from "socket.io-client";
import { API_BASE } from "./api";

export type TelemetryEvent =
    | { type: "SUBSCRIBER_CONNECTED"; msisdn: string; balance: number; unlimited5g: boolean; on5g: boolean; ts: number }
    | { type: "QUOTA_UPDATE"; msisdn: string; previousBalance: number; deducted: number; newBalance: number; direction: string; ts: number }
    | { type: "QUOTA_EXHAUSTED"; msisdn: string; ts: number }
    | { type: "THROTTLE_STARTED"; msisdn: string; ts: number }
    | { type: "THROTTLE_STOPPED"; msisdn: string; ts: number }
    | { type: "NETWORK_ACTIVITY"; msisdn: string; kind: string; host: string; up: number; down: number; endReason: string; ts: number }
    | { type: "RAT_CHANGED"; msisdn: string; rat: string; ts: number };

// One socket per login. The server sends only this user's events.
export function useTelemetry(token: string, onEvent: (e: TelemetryEvent) => void) {
    const [connected, setConnected] = useState(false);
    const handlerRef = useRef(onEvent);

    useEffect(() => {
        handlerRef.current = onEvent;
    }, [onEvent]);

    useEffect(() => {
        const socket = io(API_BASE, { auth: { token } });

        socket.on("connect", () => setConnected(true));
        socket.on("disconnect", () => setConnected(false));
        socket.on("telemetry", (raw: string) => handlerRef.current(JSON.parse(raw)));

        return () => {
            socket.disconnect();
            setConnected(false);
        };
    }, [token]);

    return { connected };
}
